"""Fine-tune Basic Pitch's head (frozen CQT front end) on synthetic + GuitarSet windows.
Logs '== step N ...' lines; writes model_step*.onnx and model.onnx into --out."""
import argparse, os, time, numpy as np, torch, torch.nn.functional as F
import bp_torch, synth_data, guitarset

p = argparse.ArgumentParser()
p.add_argument('--stock', required=True); p.add_argument('--out', required=True)
p.add_argument('--guitarset', default=''); p.add_argument('--gs_frac', type=float, default=0.5)
p.add_argument('--steps', type=int, default=3000); p.add_argument('--batch', type=int, default=16)
p.add_argument('--lr', type=float, default=1e-4); p.add_argument('--seed', type=int, default=0)
p.add_argument('--workers', type=int, default=6); p.add_argument('--save_every', type=int, default=500)

class Windows(torch.utils.data.IterableDataset):
    """Module-level (picklable) so DataLoader workers work on Windows (spawn)."""
    def __init__(self, args): super().__init__(); self.args = args
    def __iter__(self):
        args = self.args
        info = torch.utils.data.get_worker_info(); wid = info.id if info else 0
        rng = np.random.default_rng(args.seed * 1000 + wid)
        front = bp_torch.load(args.stock)[0]
        gs = []
        if args.guitarset:
            players = ['00', '01', '02', '03', '04']                 # 05 held out
            ts = guitarset.tracks(args.guitarset, players)
            for k in rng.permutation(len(ts))[:40]: gs.append(guitarset.Track(*ts[k]))   # 40 tracks per worker, resampled once
        while True:
            w = None
            if gs and rng.random() < args.gs_frac: w = guitarset.random_window(gs[rng.integers(len(gs))], rng)
            if w is None: w = synth_data.make_window(rng)
            a, note, onset, cont = w
            feat = bp_torch.features(front, a[None])[0]
            yield feat, note, onset, cont

def main():
  args = p.parse_args(); os.makedirs(args.out, exist_ok=True)
  torch.manual_seed(args.seed)
  dev = 'cuda' if torch.cuda.is_available() else 'cpu'
  _, head, model, convs = bp_torch.load(args.stock); head.to(dev)
  opt = torch.optim.Adam(head.parameters(), lr=args.lr)
  dl = torch.utils.data.DataLoader(Windows(args), batch_size=args.batch, num_workers=args.workers, persistent_workers=args.workers > 0)
  print(f'== start device={dev} steps={args.steps} batch={args.batch} lr={args.lr} seed={args.seed} guitarset={bool(args.guitarset)}', flush=True)
  t0 = time.time(); run = 0.0
  onset_w = torch.tensor(10.0, device=dev)
  for step, (feat, note, onset, cont) in enumerate(dl, 1):
      feat, note, onset, cont = feat.to(dev), note.to(dev), onset.to(dev), cont.to(dev)
      n_l, o_l, c_l = head(feat, logits=True)
      loss = F.binary_cross_entropy_with_logits(n_l, note) + F.binary_cross_entropy_with_logits(c_l, cont) \
           + F.binary_cross_entropy_with_logits(o_l, onset, pos_weight=onset_w)
      opt.zero_grad(); loss.backward(); opt.step()
      run = 0.98 * run + 0.02 * loss.item() if step > 1 else loss.item()
      if step % 50 == 0: print(f'== step {step} loss {run:.4f} {time.time() - t0:.0f}s', flush=True)
      if step % args.save_every == 0 or step == args.steps:
          bp_torch.save_onnx(head.cpu(), model, convs, os.path.join(args.out, f'model_step{step}.onnx')); head.to(dev)
      if step >= args.steps: break
  bp_torch.save_onnx(head.cpu(), model, convs, os.path.join(args.out, 'model.onnx'))
  print(f'== done {time.time() - t0:.0f}s', flush=True)

if __name__ == '__main__':
    main()
