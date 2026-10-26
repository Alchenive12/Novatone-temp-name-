"""Compare Basic Pitch models (stock vs fine-tuned) on held-out tests. Writes one small text table.
Tests: piano (FL Keys render of Cymatics WRLD MIDI 2, exact MIDI, bars 1-4), guitar loop (agreement with
Melodyne), synthetic held-out windows (seed 999), GuitarSet player 05 (if --guitarset)."""
import argparse, os, glob, sys, numpy as np, warnings, logging
warnings.filterwarnings('ignore'); logging.disable(logging.WARNING)
import onnxruntime as ort, soundfile as sf, scipy.signal as sg
import synth_data, guitarset, gt_piano
from basic_pitch.note_creation import model_output_to_notes
SR = 22050; N = 43844; HOP = 256; SIX = 60 / 140 / 4
MEL_GUITAR = [(76,27.9),(76,59.9),(71,27.9),(71,59.9)]+[(69,s) for s in (3.8,11.8,19.6,35.8,39.9,43.9,51.8)]\
   +[(66,s) for s in (19.6,27.6,51.8,59.6)]+[(64,s) for s in (3.5,11.5,36.0,43.7)]\
   +[(61,s) for s in (3.5,11.5,19.6,27.7,35.7,43.6,51.5,59.4)]+[(54,s) for s in (0.0,9.6,31.7,41.8)]\
   +[(50,s) for s in (15.5,25.7,47.5,57.5)]+[(49,28.7)]

class Model:
    def __init__(self, path):
        self.s = ort.InferenceSession(path, providers=['CPUExecutionProvider'])
        self.inp = self.s.get_inputs()[0].name
    def window(self, a):                                     # (n,N) -> note, onset, contour (n,172,*)
        o = self.s.run(None, {self.inp: a[:, :, None].astype(np.float32)})
        n88 = [x for x in o if x.shape[-1] == 88]; c = [x for x in o if x.shape[-1] == 264][0]
        return n88[1], n88[0], c                             # ONNX output order: onset, note
    def track(self, audio):                                  # overlap-add like basic_pitch.inference (30-frame overlap)
        ov = 30 * HOP; hop = N - ov; pad = ov // 2
        x = np.r_[np.zeros(pad), audio, np.zeros(N)]
        notes, onsets, conts = [], [], []
        for s in range(0, len(audio) + pad, hop):
            w = x[s:s + N]
            if len(w) < N: w = np.r_[w, np.zeros(N - len(w))]
            n, o, c = self.window(w[None])
            k = 15
            notes.append(n[0, k:-k]); onsets.append(o[0, k:-k]); conts.append(c[0, k:-k])
        T = int(len(audio) / HOP) + 1
        return {'note': np.concatenate(notes)[:T], 'onset': np.concatenate(onsets)[:T], 'contour': np.concatenate(conts)[:T]}

def load22(path):
    x, sr = sf.read(path, dtype='float32'); x = x.mean(1) if x.ndim > 1 else x
    return sg.resample_poly(x, SR, sr).astype(np.float32) if sr != SR else x

def notes_from(out):
    _, ev = model_output_to_notes(out, onset_thresh=0.5, frame_thresh=0.3, min_note_len=11, infer_onsets=True, melodia_trick=True, midi_tempo=120)
    return [(int(p), s, e) for s, e, p, a, b in ev]

def piano(m):
    det = [(p, s / SIX, e / SIX) for p, s, e in notes_from(m.track(load22('test_piano.wav')))]
    gt = [g for g in gt_piano.GT if g[2] > 2.0]; det = [d for d in det if d[2] > 2.0 and d[1] < 63]
    used = set(); hit = 0; right = found = 0
    for g in gt:
        c = [i for i, d in enumerate(det) if i not in used and d[0] == g[0] and abs(max(d[1], 1.9) - max(g[1], 1.9)) <= 1.5]
        if c: used.add(c[0]); hit += 1
        pc = [d for d in det if d[0] % 12 == g[0] % 12 and abs(max(d[1], 1.9) - max(g[1], 1.9)) <= 1.5]
        if pc: found += 1; right += any(d[0] == g[0] for d in pc)
    P = hit / max(1, len(det)); R = hit / len(gt)
    return 2 * P * R / max(1e-9, P + R), right / max(1, found)

def guitar(m):
    det = [(p, s / SIX) for p, s, e in notes_from(m.track(load22('test_guitar.wav')))]
    used = set(); agree = 0
    for p, s in MEL_GUITAR:
        c = [i for i, d in enumerate(det) if i not in used and d[0] == p and abs(d[1] - s) <= 1.5]
        if c: used.add(c[0]); agree += 1
    return agree, len(det) - agree

def frame_f1(pred, lab, th=0.5):
    tp = ((pred > th) & (lab > 0.5)).sum(); fp = ((pred > th) & (lab < 0.5)).sum(); fn = ((pred <= th) & (lab > 0.5)).sum()
    return 2 * tp / max(1, 2 * tp + fp + fn)

def synthetic(m, n=120):
    rng = np.random.default_rng(999); f = []
    for _ in range(n):
        a, note, onset, cont = synth_data.make_window(rng)
        p, _, _ = m.window(a[None]); f.append(frame_f1(p[0], note))
    return float(np.mean(f))

def gset(m, root, limit):
    import mir_eval
    ts = guitarset.tracks(root, ['05'])[:limit]; f = []
    for wav, jp in ts:
        tr = guitarset.Track(wav, jp)
        det = notes_from(m.track(tr.audio))
        ref = [(s, s + d, 440 * 2 ** ((v - 69) / 12)) for s, d, v in tr.notes]
        if not det or not ref: continue
        ri = np.array([[s, e] for s, e, _ in ref]); rp = np.array([p for _, _, p in ref])
        ei = np.array([[s, e] for _, s, e in det]); ep = np.array([440 * 2 ** ((p - 69) / 12) for p, _, _ in det])
        f.append(mir_eval.transcription.precision_recall_f1_overlap(ri, rp, ei, ep, offset_ratio=None)[2])
    return float(np.mean(f)) if f else float('nan')

ap = argparse.ArgumentParser(); ap.add_argument('models', nargs='+'); ap.add_argument('--guitarset', default=''); ap.add_argument('--gs_limit', type=int, default=30)
ap.add_argument('--out', default='run_compare.txt'); ap.add_argument('--synth_n', type=int, default=120)
a = ap.parse_args()
rows = []
for path in a.models:
    m = Model(path); pf, po = piano(m); ga, gx = guitar(m); sy = synthetic(m, a.synth_n)
    gs = gset(m, a.guitarset, a.gs_limit) if a.guitarset else float('nan')
    rows.append((os.path.basename(os.path.dirname(path)) + '/' + os.path.basename(path), pf, po, ga, gx, sy, gs))
    print('== eval', rows[-1], flush=True)
with open(a.out, 'w') as f:
    f.write('model | piano F1 | piano right-octave | guitar agreed/36 | guitar extras | synthetic frame F1 | GuitarSet-05 note F1\n')
    for r in rows: f.write(f'{r[0]} | {r[1]:.3f} | {r[2]:.0%} | {r[3]} | {r[4]} | {r[5]:.3f} | {r[6]:.3f}\n')
    f.write('ship rule: GuitarSet-05 F1 >= stock + 0.02, piano F1 >= stock, synthetic F1 >= stock, guitar agreed >= stock - 1, guitar extras <= stock + 2\n')
print(open(a.out).read())
