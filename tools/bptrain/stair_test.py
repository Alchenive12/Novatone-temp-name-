"""Quiet-note test: real NSynth notes, loud base + quieter target at an interval (octave, 12th, ...).
Lowers the target level step by step and records where each detector stops hearing it.
Null trials (base only) count hallucinations: a detected note at the target pitch that isn't there.
Usage: stair_test.py <nsynth-test dir> [--pairs 30] [--out stair.txt]   (needs evaluate.py next to it)"""
import argparse, json, os, re, subprocess, sys, tempfile, numpy as np, soundfile as sf, scipy.signal as sg
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
src = open(os.path.join(HERE, 'evaluate.py')).read().split('\nap = ')[0]; exec(src)   # Model, SR, notes_from
from basic_pitch.note_creation import model_output_to_notes

INTERVALS = [12, 19, 24, 7, 4, 3, -12]          # target relative to base (semitones)
LEVELS = list(range(0, -40, -4))                # target level vs base, dB (equal RMS = 0)
SEG, GAP = 4.0, 0.6                             # NSynth note length, silence between trials
FAMS = {'keyboard', 'guitar', 'string', 'brass', 'reed', 'organ', 'mallet', 'flute', 'vocal', 'bass'}
BENCH = os.environ.get('NT_POLYBENCH', '/home/claude/build-tools/NovaTonePolyBench_artefacts/Release/NovaTonePolyBench')

def load_nsynth(root):
    ex = json.load(open(os.path.join(root, 'examples.json')))
    idx = {}
    for k, e in ex.items():
        if e['instrument_source_str'] == 'synthetic' or e['instrument_family_str'] not in FAMS: continue
        idx.setdefault((e['instrument_str'], e['velocity']), {})[e['pitch']] = os.path.join(root, 'audio', k + '.wav')
    return idx

def note(path):
    x, sr = sf.read(path, dtype='float32')
    x = sg.resample_poly(x, SR, sr).astype(np.float32)[:int(SEG * SR)]
    r = np.sqrt(np.mean(x[:int(3 * SR)] ** 2)) + 1e-9
    return x / r

def make_trials(idx, pairs, rng):
    keys = list(idx); trials = []
    for k in INTERVALS:
        got = 0; tries = 0
        while got < pairs and tries < 20000:
            tries += 1; key = keys[rng.integers(len(keys))]; ps = idx[key]
            cands = [p for p in ps if 36 <= p <= 84 and p + k in ps and 33 <= p + k <= 100]
            if not cands: continue
            p = cands[rng.integers(len(cands))]
            trials.append((k, key[0], p, p + k, ps[p], ps[p + k])); got += 1
    return trials

def build(trials):
    """One long track per run: for each trial, every level, then a null (base only). Returns audio + segment list."""
    segs, chunks, t = [], [], 0.0
    gap = np.zeros(int(GAP * SR), np.float32)
    for tr in trials:
        b, g = note(tr[4]), note(tr[5]); n = min(len(b), len(g)); b, g = b[:n], g[:n]
        for L in LEVELS + [None]:
            x = b + (0 if L is None else 10 ** (L / 20)) * g
            x = 0.4 * x / (np.abs(x).max() + 1e-9)
            segs.append((tr, L, t)); chunks += [x, gap]; t += (n + len(gap)) / SR
    return np.concatenate(chunks), segs

def hit(notes, pitch, t0):   # a note at this pitch overlapping the sustain of the segment by >= 0.3 s
    a, b = t0 + 0.1, t0 + 3.0
    return any(p == pitch and min(e, b) - max(s, a) >= 0.3 for p, s, e in notes)

def bp_notes(m, audio, frame):
    out = m.track(audio)
    _, ev = model_output_to_notes(out, onset_thresh=0.5, frame_thresh=frame, min_note_len=11, infer_onsets=True, melodia_trick=True, midi_tempo=120)
    return [(int(p), s, e) for s, e, p, a, b in ev]

NAMES = {n: i for i, n in enumerate(['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'])}
def nt_notes(audio, segs, chunk=600.0):
    """NovaTone classic via NovaTonePolyBench, in ~10 min pieces cut at trial boundaries."""
    cuts = [0.0]
    for _, _, t in segs:
        if t - cuts[-1] >= chunk: cuts.append(t)
    cuts.append(len(audio) / SR); res = []; d = tempfile.mkdtemp(); w = os.path.join(d, 'x.wav')
    for a, b in zip(cuts, cuts[1:]):
        sf.write(w, audio[int(a * SR):int(b * SR)], SR)
        txt = subprocess.run([BENCH, 'file', w], cwd=d, capture_output=True, text=True, timeout=7200,
                             env={**os.environ, 'NT_LIST_ONLY': '1'}).stdout
        for m in re.finditer(r'^\s+([\d.]+)-([\d.]+)\s+([A-G]#?)(-?\d+)', txt, re.M):
            res.append((NAMES[m.group(3)] + 12 * (int(m.group(4)) + 1), a + float(m.group(1)), a + float(m.group(2))))
    return res

def report(name, rows, f):
    """rows[k][L] = list of (target hit, base hit); L None = null trial (target absent)."""
    print(f'\n== {name}', file=f)
    print('interval | ' + ' | '.join(f'{L:>3}dB' for L in LEVELS) + ' | limit dB | halluc. | base kept', file=f)
    for k in INTERVALS:
        r = [np.mean([h for h, _ in rows[k][L]]) for L in LEVELS]
        lim = next((L for L, x in zip(LEVELS, r) if x < 0.5), None)
        fp = np.mean([h for h, _ in rows[k][None]]); base = np.mean([b for L in rows[k] for _, b in rows[k][L]])
        print(f'{k:+4d}     | ' + ' | '.join(f'{x:5.0%}' for x in r) + f' | {"<"+str(LEVELS[-1]) if lim is None else lim:>8} | {fp:6.0%} | {base:5.0%}', file=f)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('nsynth'); ap.add_argument('--pairs', type=int, default=30)
    ap.add_argument('--out', default='stair.txt'); ap.add_argument('--models', nargs='*', default=[os.path.join(HERE, 'nmp_stock.onnx')])
    ap.add_argument('--frames', nargs='*', type=float, default=[0.3, 0.25]); ap.add_argument('--no_nt', action='store_true')
    a = ap.parse_args()
    trials = make_trials(load_nsynth(a.nsynth), a.pairs, np.random.default_rng(7))
    models = [(os.path.basename(mp), Model(mp)) for mp in a.models]
    dets = [(f'{n} frame {fr}', (lambda au, sg_, m=m, fr=fr: bp_notes(m, au, fr))) for n, m in models for fr in a.frames]
    if not a.no_nt: dets.append(('NovaTone classic', nt_notes))
    res = {d: {k: {L: [] for L in LEVELS + [None]} for k in INTERVALS} for d, _ in dets}
    for k in INTERVALS:                      # one interval at a time keeps memory small
        audio, segs = build([t for t in trials if t[0] == k])
        for d, fn in dets:
            notes = fn(audio, segs)
            for tr, L, t in segs: res[d][k][L].append((hit(notes, tr[3], t), hit(notes, tr[2], t)))
        print('done interval', k, flush=True)
    with open(a.out, 'w') as f:
        print(f'{len(trials)} pairs x {len(LEVELS)} levels + null. limit = first level detected < 50%; halluc. = target pitch reported when absent', file=f)
        for d, _ in dets: report(d, res[d], f)
    print(open(a.out).read())
