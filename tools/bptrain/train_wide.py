"""From-scratch Basic Pitch net at width x (same layout as stock: onset/contour/note convs, 39-bin octave kernel),
on the stock front end (CQT + harmonic stack, no learned weights). Data: NSynth real-note chords (1-4 notes,
random levels/onsets), GuitarSet, synthetic. Writes head-only ONNX (input = stacked CQT) as model_step*.head.onnx
and model.head.onnx; evaluate.py / stair_test.py run them on the stock front end.
Logs '== step N loss' lines."""
import argparse, json, os, time, numpy as np, torch, torch.nn as nn, torch.nn.functional as F
import bp_torch, guitarset, synth_data
from train_stair import index, load_note, FAMS
SR, N = 22050, 43844

p = argparse.ArgumentParser()
p.add_argument('--stock', required=True); p.add_argument('--out', required=True); p.add_argument('--nsynth', required=True)
p.add_argument('--guitarset', default=''); p.add_argument('--width', type=float, default=2.0)
p.add_argument('--ns_frac', type=float, default=0.6); p.add_argument('--gs_frac', type=float, default=0.3)   # rest synthetic
p.add_argument('--steps', type=int, default=20000); p.add_argument('--batch', type=int, default=16); p.add_argument('--lr', type=float, default=1e-3)
p.add_argument('--seed', type=int, default=0); p.add_argument('--workers', type=int, default=6); p.add_argument('--save_every', type=int, default=5000)

class WideHead(nn.Module):
    """Stock layout, channels x width, BatchNorm after hidden convs (folded away at export by ONNX eval mode)."""
    def __init__(self, w):
        super().__init__(); c = lambda n: max(1, int(round(n * w)))
        self.onset1 = nn.Conv2d(8, c(32), (5, 5), stride=(1, 3)); self.bo = nn.BatchNorm2d(c(32))
        self.cont1 = nn.Conv2d(8, c(8), (3, 39)); self.bc = nn.BatchNorm2d(c(8))
        self.cont2 = nn.Conv2d(c(8), 1, (5, 5))
        self.note1 = nn.Conv2d(1, c(32), (7, 7), stride=(1, 3)); self.bn = nn.BatchNorm2d(c(32))
        self.note2 = nn.Conv2d(c(32), 1, (7, 3))
        self.onset2 = nn.Conv2d(c(32) + 1, 1, (3, 3))
    @staticmethod
    def same(x, k, sf=1):   # 'same' padding (time, freq) as in the stock graph; freq stride 3 maps 264 -> 88
        kt, kf = k; pf = max(kf - sf, 0)
        return F.pad(x, (pf // 2, pf - pf // 2, (kt - 1) // 2, kt // 2))
    def forward(self, x, logits=False):
        on1 = F.relu(self.bo(self.onset1(self.same(x, (5, 5), 3))))
        c = F.relu(self.bc(self.cont1(self.same(x, (3, 39)))))
        c_l = self.cont2(self.same(c, (5, 5))); contour = torch.sigmoid(c_l)
        n1 = F.relu(self.bn(self.note1(self.same(contour, (7, 7), 3))))
        n_l = self.note2(self.same(n1, (7, 3))); note = torch.sigmoid(n_l)
        o_l = self.onset2(self.same(torch.cat([note, on1], 1), (3, 3)))
        if logits: return n_l[:, 0], o_l[:, 0], c_l[:, 0]
        return note[:, 0], torch.sigmoid(o_l)[:, 0], contour[:, 0]

def export(head, path):
    head = head.cpu().eval()
    torch.onnx.export(head, torch.zeros(1, 8, 172, 264), path, input_names=['feat'], output_names=['note', 'onset', 'contour'],
                      dynamic_axes={'feat': {0: 'b'}, 'note': {0: 'b'}, 'onset': {0: 'b'}, 'contour': {0: 'b'}}, opset_version=13, dynamo=False)

def chord(idx, keys, rng):
    """1-4 real NSynth notes, own level (-30..0 dB) and start; one instrument or several."""
    n = int(rng.choice([1, 2, 3, 4], p=[0.2, 0.3, 0.3, 0.2])); same = rng.random() < 0.5
    k0 = keys[rng.integers(len(keys))]; a = np.zeros(N, np.float32); notes = []; used = set()
    s_common = rng.uniform(0, 0.5) if rng.random() < 0.6 else -rng.uniform(0.2, 1.5)
    for i in range(n):
        ps = idx[k0 if same else keys[rng.integers(len(keys))]]
        cand = [q for q in ps if 28 <= q <= 100 and q not in used]
        if not cand: continue
        q = cand[rng.integers(len(cand))]; used.add(q)
        s0 = s_common if rng.random() < 0.6 else rng.uniform(-1.5, 1.5)
        x = (1.0 if i == 0 else 10 ** (rng.uniform(-30, 0) / 20)) * load_note(ps[q]); i0 = int(s0 * SR)
        src = x[max(0, -i0):]; dst = max(0, i0); m = min(len(src), N - dst)
        if m <= 0: continue
        a[dst:dst + m] += src[:m]; notes.append((s0, 3.0, float(q)))
    if not notes: return None
    a *= rng.uniform(0.2, 0.9) / (np.abs(a).max() + 1e-9)
    return (a,) + guitarset.labels_for(notes, 0.0)

class Windows(torch.utils.data.IterableDataset):
    def __init__(self, args, idx): super().__init__(); self.args = args; self.idx = idx
    def __iter__(self):
        a = self.args; info = torch.utils.data.get_worker_info(); wid = info.id if info else 0
        rng = np.random.default_rng(a.seed * 1000 + wid); front = bp_torch.load(a.stock)[0]; gs = []; keys = list(self.idx)
        if a.guitarset:
            ts = guitarset.tracks(a.guitarset, ['00', '01', '02', '03', '04'])
            for k in rng.permutation(len(ts))[:30]: gs.append(guitarset.Track(*ts[k]))
        while True:
            r = rng.random()
            if r < a.ns_frac: w = chord(self.idx, keys, rng)
            elif r < a.ns_frac + a.gs_frac and gs: w = guitarset.random_window(gs[rng.integers(len(gs))], rng)
            else: w = synth_data.make_window(rng)
            if w is None: continue
            yield (bp_torch.features(front, w[0][None])[0],) + tuple(w[1:])

def main():
    a = p.parse_args(); os.makedirs(a.out, exist_ok=True); torch.manual_seed(a.seed)
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    tr, pr = index(a.nsynth); tr.update(pr)                 # all train-set instruments (test set stays separate)
    head = WideHead(a.width).to(dev)
    npar = sum(p.numel() for p in head.parameters())
    opt = torch.optim.Adam(head.parameters(), lr=a.lr); sch = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.steps)
    dl = torch.utils.data.DataLoader(Windows(a, tr), batch_size=a.batch, num_workers=a.workers, persistent_workers=a.workers > 0)
    print(f'== start device={dev} width={a.width} params={npar} steps={a.steps} lr={a.lr}', flush=True)
    t0 = time.time(); run = 0.0; ow = torch.tensor(10.0, device=dev); head.train()
    for step, (feat, note, onset, cont) in enumerate(dl, 1):
        feat, note, onset, cont = feat.to(dev), note.to(dev), onset.to(dev), cont.to(dev)
        n_l, o_l, c_l = head(feat, logits=True)
        loss = F.binary_cross_entropy_with_logits(n_l, note) + F.binary_cross_entropy_with_logits(c_l, cont) \
             + F.binary_cross_entropy_with_logits(o_l, onset, pos_weight=ow)
        opt.zero_grad(); loss.backward(); opt.step(); sch.step()
        run = 0.98 * run + 0.02 * loss.item() if step > 1 else loss.item()
        if step % 50 == 0: print(f'== step {step} loss {run:.4f} {time.time() - t0:.0f}s', flush=True)
        if step % a.save_every == 0 or step == a.steps:
            export(head, os.path.join(a.out, f'model_step{step}.head.onnx')); head.to(dev).train()
        if step >= a.steps: break
    export(head, os.path.join(a.out, 'model.head.onnx'))
    print(f'== done {time.time() - t0:.0f}s', flush=True)

if __name__ == '__main__':
    main()
