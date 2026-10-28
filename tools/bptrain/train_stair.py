"""Staircase fine-tune: loud real note (NSynth) + quieter target at an interval. Per interval the target level
goes down 2 dB whenever the model hears it (>=70% on a held-out probe), and freezes as soon as hallucinations
(target pitch reported on base-only mixes, or any extra pitch) rise more than --fp_margin over stock.
Mix: --stair_frac staircase windows, --null_frac base-only windows, rest GuitarSet (keeps real-music skills).
Logs '== step', '== probe', '== limit' lines; writes model_step*.onnx, model.onnx, levels.json into --out."""
import argparse, json, os, time, zlib, numpy as np, torch, torch.nn.functional as F, soundfile as sf, scipy.signal as sg
import bp_torch, guitarset
SR, N, HOP, FR = 22050, 43844, 256, 172
INTERVALS = [12, 19, 24, 7, 4, 3, -12]
FAMS = {'keyboard', 'guitar', 'string', 'brass', 'reed', 'organ', 'mallet', 'flute', 'vocal', 'bass'}

p = argparse.ArgumentParser()
p.add_argument('--stock', required=True); p.add_argument('--out', required=True); p.add_argument('--nsynth', required=True)
p.add_argument('--guitarset', default=''); p.add_argument('--stair_frac', type=float, default=0.55); p.add_argument('--null_frac', type=float, default=0.15)
p.add_argument('--start', default='', help='json {interval: start dB} (stock limits); default -8 for all')
p.add_argument('--steps', type=int, default=8000); p.add_argument('--batch', type=int, default=16); p.add_argument('--lr', type=float, default=1e-5)
p.add_argument('--seed', type=int, default=0); p.add_argument('--workers', type=int, default=6)
p.add_argument('--probe_every', type=int, default=250); p.add_argument('--fp_margin', type=float, default=0.02)

def index(root):
    ex = json.load(open(os.path.join(root, 'examples.json'))); idx = {}
    for k, e in ex.items():
        if e['instrument_source_str'] == 'synthetic' or e['instrument_family_str'] not in FAMS: continue
        idx.setdefault((e['instrument_str'], e['velocity']), {})[e['pitch']] = os.path.join(root, 'audio', k + '.wav')
    keys = sorted(idx); probe = {k for i, k in enumerate(keys) if zlib.crc32(k[0].encode()) % 20 == 0}   # ~5% instruments held out for the probe
    return {k: idx[k] for k in keys if k not in probe}, {k: idx[k] for k in keys if k in probe}

def load_note(path):
    x, sr = sf.read(path, dtype='float32'); x = sg.resample_poly(x, SR, sr).astype(np.float32)
    return x / (np.sqrt(np.mean(x[:3 * SR] ** 2)) + 1e-9)

def pick(idx, k, rng, chord):
    """(paths, pitches): base [+ chord tone] and target = base + k, all from one instrument/velocity."""
    keys = list(idx)
    for _ in range(200):
        ps = idx[keys[rng.integers(len(keys))]]
        c = [q for q in ps if 36 <= q <= 84 and q + k in ps and 33 <= q + k <= 100]
        if not c: continue
        b = c[rng.integers(len(c))]; base = [b]
        if chord:
            o = [q for q in ps if q not in (b, b + k) and abs(q - b) in (3, 4, 5, 7, 8, 9) and 33 <= q <= 100]
            if o: base.append(o[rng.integers(len(o))])
        return [ps[q] for q in base], base, ps[b + k], b + k
    return None

def mix(base_paths, base_p, tgt_path, tgt_p, level, rng, s0=None):
    """2 s window, notes start at s0; NSynth note-off at 3 s. Target at `level` dB vs base (None = absent)."""
    if s0 is None: s0 = rng.uniform(0, 0.5) if rng.random() < 0.6 else -rng.uniform(0.2, 1.5)
    a = np.zeros(N, np.float32); notes = []
    parts = [(p, q, 1.0) for p, q in zip(base_paths, base_p)] + ([(tgt_path, tgt_p, 10 ** (level / 20))] if level is not None else [])
    for path, q, g in parts:
        x = g * load_note(path); i0 = int(s0 * SR)
        src = x[max(0, -i0):]; dst = max(0, i0); n = min(len(src), N - dst); a[dst:dst + n] += src[:n]
        notes.append((s0, 3.0, float(q)))
    a *= rng.uniform(0.2, 0.9) / (np.abs(a).max() + 1e-9)
    return (a,) + guitarset.labels_for(notes, 0.0)

def levels(path, cache={}):
    try:
        m = os.path.getmtime(path)
        if cache.get('m') != m: cache['m'] = m; cache['v'] = {int(k): v for k, v in json.load(open(path)).items()}
    except Exception: pass
    return cache.get('v', {k: -8.0 for k in INTERVALS})

class Windows(torch.utils.data.IterableDataset):
    def __init__(self, args, train_idx): super().__init__(); self.args = args; self.idx = train_idx
    def __iter__(self):
        a = self.args; info = torch.utils.data.get_worker_info(); wid = info.id if info else 0
        rng = np.random.default_rng(a.seed * 1000 + wid); front = bp_torch.load(a.stock)[0]; gs = []
        if a.guitarset:
            ts = guitarset.tracks(a.guitarset, ['00', '01', '02', '03', '04'])
            for k in rng.permutation(len(ts))[:30]: gs.append(guitarset.Track(*ts[k]))
        lv = os.path.join(a.out, 'levels.json')
        while True:
            r = rng.random(); w = None
            if r < a.stair_frac + a.null_frac or not gs:
                k = INTERVALS[rng.integers(len(INTERVALS))]; pk = pick(self.idx, k, rng, rng.random() < 0.3)
                if pk:
                    L = None if r >= a.stair_frac else levels(lv)[k] + rng.uniform(-3, 3)
                    w = mix(pk[0], pk[1], pk[2], pk[3], L, rng)
            else: w = guitarset.random_window(gs[rng.integers(len(gs))], rng)
            if w is None: continue
            yield (bp_torch.features(front, w[0][None])[0],) + tuple(w[1:])

class Probe:
    """Fixed held-out pairs. det(k, L): share of target notes heard at level L. fp(k): target pitch on base-only mixes.
    extra: any pitch not in the mix (>=50% of sustain frames over 0.3) on base-only mixes."""
    def __init__(self, args, idx, front, n=24):
        rng = np.random.default_rng(12345); self.front = front; self.pairs = {}
        for k in INTERVALS:
            self.pairs[k] = [q for q in (pick(idx, k, rng, False) for _ in range(n)) if q]
        self.notes = {}; self.null = {}
        for k, ps in self.pairs.items():
            self.null[k] = self.feats([self.render(q, None) for q in ps])
    def render(self, q, L):
        a, *_ = mix(q[0], q[1], q[2], q[3], L, np.random.default_rng(0), s0=0.0); return a
    def feats(self, audios): return torch.tensor(np.concatenate([bp_torch.features(self.front, a[None]) for a in audios]))
    @torch.no_grad()
    def note_prob(self, head, feats, dev):
        return torch.cat([head(feats[i:i + 16].to(dev))[0].cpu() for i in range(0, len(feats), 16)]).numpy()
    @staticmethod
    def heard(nt, m):   # sustain frames of the window that are inside the note (s0 = 0 in render: 0..2 s all note)
        return (nt[20:160, m - 21] > 0.3).mean() >= 0.5
    def run(self, head, lv, dev):
        det, fp, extra = {}, {}, {}
        for k, ps in self.pairs.items():
            on = self.note_prob(head, self.feats([self.render(q, lv[k]) for q in ps]), dev)
            nl = self.note_prob(head, self.null[k], dev)
            det[k] = float(np.mean([self.heard(on[i], q[3]) for i, q in enumerate(ps)]))
            fp[k] = float(np.mean([self.heard(nl[i], q[3]) for i, q in enumerate(ps)]))
            extra[k] = float(np.mean([any(self.heard(nl[i], m) for m in range(21, 109) if m not in q[1])
                                      for i, q in enumerate(ps)]))
        return det, fp, extra

def main():
    a = p.parse_args(); os.makedirs(a.out, exist_ok=True); torch.manual_seed(a.seed)
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    train_idx, probe_idx = index(a.nsynth)
    front, head, model, convs = bp_torch.load(a.stock); head.to(dev).eval()
    lv = {k: -8.0 for k in INTERVALS}
    if a.start: lv.update({int(k): float(v) for k, v in json.load(open(a.start)).items()})
    frozen = {k: False for k in INTERVALS}
    json.dump(lv, open(os.path.join(a.out, 'levels.json'), 'w'))
    probe = Probe(a, probe_idx, front)
    _, fp0, ex0 = probe.run(head, lv, dev)                     # stock hallucination baseline
    print(f'== start device={dev} train_sets={len(train_idx)} probe_sets={len(probe_idx)} stock_fp={fp0} stock_extra={ex0}', flush=True)
    opt = torch.optim.Adam(head.parameters(), lr=a.lr)
    dl = torch.utils.data.DataLoader(Windows(a, train_idx), batch_size=a.batch, num_workers=a.workers, persistent_workers=a.workers > 0)
    t0 = time.time(); run = 0.0; ow = torch.tensor(10.0, device=dev); head.train()
    for step, (feat, note, onset, cont) in enumerate(dl, 1):
        feat, note, onset, cont = feat.to(dev), note.to(dev), onset.to(dev), cont.to(dev)
        n_l, o_l, c_l = head(feat, logits=True)
        loss = F.binary_cross_entropy_with_logits(n_l, note) + F.binary_cross_entropy_with_logits(c_l, cont) \
             + F.binary_cross_entropy_with_logits(o_l, onset, pos_weight=ow)
        opt.zero_grad(); loss.backward(); opt.step()
        run = 0.98 * run + 0.02 * loss.item() if step > 1 else loss.item()
        if step % 50 == 0: print(f'== step {step} loss {run:.4f} {time.time() - t0:.0f}s', flush=True)
        if step % a.probe_every == 0 or step == a.steps:
            head.eval(); det, fp, ex = probe.run(head, lv, dev); head.train()
            print(f'== probe {step} ' + ' '.join(f'{k:+d}:{lv[k]:.0f}dB det{det[k]:.2f} fp{fp[k]:.2f} ex{ex[k]:.2f}' + ('*' if frozen[k] else '')
                                                   for k in INTERVALS), flush=True)
            for k in INTERVALS:
                if frozen[k]: continue
                if fp[k] > fp0[k] + a.fp_margin or ex[k] > ex0[k] + a.fp_margin:
                    lv[k] = min(lv[k] + 2, 0.0); frozen[k] = True
                    print(f'== limit {k:+d} hallucinations start below {lv[k]:.0f} dB (fp {fp[k]:.2f} vs stock {fp0[k]:.2f}, extra {ex[k]:.2f} vs {ex0[k]:.2f})', flush=True)
                elif det[k] >= 0.7 and lv[k] > -40: lv[k] -= 2
                elif det[k] < 0.4 and lv[k] < 0: lv[k] += 2      # too hard: make it easier (two-way staircase)
            json.dump(lv, open(os.path.join(a.out, 'levels.json'), 'w'))
            bp_torch.save_onnx(head.cpu(), model, convs, os.path.join(a.out, f'model_step{step}.onnx')); head.to(dev)
        if step >= a.steps or all(frozen.values()): break
    bp_torch.save_onnx(head.cpu(), model, convs, os.path.join(a.out, 'model.onnx'))
    print(f'== done {time.time() - t0:.0f}s levels {lv} frozen {frozen}', flush=True)

if __name__ == '__main__':
    main()
