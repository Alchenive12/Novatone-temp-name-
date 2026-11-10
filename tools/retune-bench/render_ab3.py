"""usage: python3 -I render_ab3.py <prepdir> <outdir> <h3_A> <h3_B> [shifts...]  - renders + measures old vs new."""
import sys, os, subprocess, numpy as np, soundfile as sf, librosa
d, out, A, B = sys.argv[1:5]; shifts = [int(s) for s in sys.argv[5:]] or [0, 1, 3]
os.makedirs(out, exist_ok=True); sr = 44100; hop = 512
x = np.fromfile(f'{d}/yv.f32', np.float32); mi = np.load(f'{d}/yv_midi.npy')
sf.write(f'{out}/0_dry.wav', x, sr, subtype='PCM_24')
for k in shifts:
    nt = np.loadtxt(f'{d}/notes{k}.txt')
    for lab, binp in (('old', A), ('new', B)):
        raw = f'{out}/r.f32'
        subprocess.run([binp, f'{d}/yv.f32', raw, f'{d}/notes{k}.txt', '1.0', '0.5', '1024'], check=True, stderr=subprocess.DEVNULL)
        y = np.fromfile(raw, np.float32); os.remove(raw)
        sf.write(f'{out}/shift{k:+d}_{lab}.wav', np.clip(y, -1, 1), sr, subtype='PCM_24')
        f, _, _ = librosa.pyin(y.astype(float), fmin=70, fmax=1100, sr=sr, frame_length=2048, hop_length=hop)
        mo = librosa.hz_to_midi(f); n = min(len(mo), len(mi)); dsh = mo[:n] - mi[:n]
        core, edge, lv = [], [], []
        for s, e, c, tg, _ in nt:
            a, b = int(s // hop), int(e // hop); err = (dsh[a:b] - (tg - c)) * 100
            err = np.where(np.abs(err) > 600, np.nan, err)               # octave slips of the pitch reader
            core += list(err[6:-6]); edge += list(err[:6]) + list(err[-6:])
            if e - s > 8000: lv.append(10 * np.log10(np.mean(y[int(s)+2000:int(e)-2000]**2) / np.mean(x[int(s)+2000:int(e)-2000]**2)))
        r = lambda v: np.sqrt(np.nanmean(np.array(v) ** 2))
        print(f'{k:+d} {lab}: pitch error vs sung shape - inside notes {r(core):.0f} c, note edges (70 ms) {r(edge):.0f} c | '
              f'biggest frame-to-frame jump {np.nanpercentile(np.abs(np.diff(dsh)), 99)*100:.0f} c (99%) | level vs dry {np.mean(lv):+.2f} dB')
