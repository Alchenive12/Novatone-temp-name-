"""Prepare a vocal for harness2: mono float32 + per-block note targets.
usage: python3 -I prep_vocal.py <vocal.wav> <outdir> [shifts...]   (default shifts: 0 1 3)
writes <outdir>/yv.f32, yv_midi.npy, tg<k>.f32 (one target per 512-sample block, 0 = no override)
"""
import sys, os, numpy as np, soundfile as sf, librosa
wav, out = sys.argv[1], sys.argv[2]
shifts = [int(s) for s in sys.argv[3:]] or [0, 1, 3]
os.makedirs(out, exist_ok=True)
x, sr = sf.read(wav); assert sr == 44100, sr
m = (x.mean(1) if x.ndim > 1 else x).astype(np.float32); m.tofile(f'{out}/yv.f32')
hop = 512
f, _, _ = librosa.pyin(m.astype(float), fmin=70, fmax=900, sr=sr, frame_length=2048, hop_length=hop)
midi = librosa.hz_to_midi(f); np.save(f'{out}/yv_midi.npy', midi)
ok = ~np.isnan(midi); sm = np.full_like(midi, np.nan)
for i in range(len(midi)):
    w = midi[max(0, i - 6):i + 7]; w = w[~np.isnan(w)]
    if ok[i] and len(w) >= 4: sm[i] = np.median(w)
note = np.round(sm); nb = (len(m) + 511) // 512
for k in shifts:
    tg = np.zeros(nb, np.float32)
    for b in range(nb):
        i = int(round((b * 512 - 1024) / 512))      # engine analyses ~1024 samples behind the block start
        if 0 <= i < len(note) and not np.isnan(note[i]): tg[b] = note[i] + k
    tg.tofile(f'{out}/tg{k}.f32')
print('voiced', round(float(ok.mean()), 2), 'median midi', round(float(np.nanmedian(midi)), 1), 'blocks', nb)
