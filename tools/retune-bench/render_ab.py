"""Render + measure A/B. usage: python3 -I render_ab.py <dir with yv.f32/tg*.f32> <outdir> <binA> <binB> [shifts...]
Runs each binary (harness2) per shift, writes 24-bit WAVs and prints median shift, mean cents off, RMS.
"""
import sys, os, subprocess, numpy as np, soundfile as sf, librosa
d, out, A, B = sys.argv[1:5]; shifts = [int(s) for s in sys.argv[5:]] or [0, 1, 3]
os.makedirs(out, exist_ok=True); sr = 44100
x = np.fromfile(f'{d}/yv.f32', np.float32); mi = np.load(f'{d}/yv_midi.npy')
sf.write(f'{out}/0_dry.wav', x, sr, subtype='PCM_24')
for k in shifts:
    for lab, binp in (('A', A), ('B', B)):
        raw = f'{out}/r_{lab}_{k}.f32'
        subprocess.run([binp, f'{d}/yv.f32', raw, f'{d}/tg{k}.f32', '1.0', '0.5'], check=True)
        y = np.fromfile(raw, np.float32); os.remove(raw)
        sf.write(f'{out}/shift{k:+d}_{lab}.wav', np.clip(y, -1, 1), sr, subtype='PCM_24')
        f, _, _ = librosa.pyin(y.astype(float), fmin=70, fmax=1100, sr=sr, frame_length=2048, hop_length=512)
        mo = librosa.hz_to_midi(f); n = min(len(mo), len(mi)); okk = ~np.isnan(mo[:n]) & ~np.isnan(mi[:n])
        print(f'{k:+d} {lab}: shift {np.median(mo[:n][okk]-mi[:n][okk]):+.2f} st, '
              f'{np.mean(np.abs(mo[:n][okk]-np.round(mo[:n][okk])))*100:.0f} c off, '
              f'rms {np.sqrt(np.mean(y**2)):.3f} (dry {np.sqrt(np.mean(x**2)):.3f})')
