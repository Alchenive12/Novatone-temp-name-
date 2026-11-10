"""Real vocal -> harness3 input. usage: python3 -I prep_notes.py <vocal.wav> <outdir> [shifts...]
writes <outdir>/yv.f32, yv_midi.npy and notes<k>.txt (startSample endSample sungCentre target edited):
every sung note snapped to the nearest semitone and moved k semitones, like moving all notes in the piano roll."""
import sys, os, numpy as np, soundfile as sf, librosa
wav, out = sys.argv[1], sys.argv[2]; shifts = [int(s) for s in sys.argv[3:]] or [0, 1, 3]
os.makedirs(out, exist_ok=True)
x, sr = sf.read(wav); assert sr == 44100, sr
m = (x.mean(1) if x.ndim > 1 else x).astype(np.float32); m.tofile(f'{out}/yv.f32'); hop = 512
f, _, _ = librosa.pyin(m.astype(float), fmin=70, fmax=900, sr=sr, frame_length=2048, hop_length=hop)
midi = librosa.hz_to_midi(f); np.save(f'{out}/yv_midi.npy', midi)
sm = np.full_like(midi, np.nan)
for i in range(len(midi)):
    w = midi[max(0, i - 6):i + 7]; w = w[~np.isnan(w)]
    if not np.isnan(midi[i]) and len(w) >= 4: sm[i] = np.median(w)
lab = np.round(sm); notes = []; i = 0
while i < len(lab):
    if np.isnan(lab[i]): i += 1; continue
    j = i
    while j < len(lab) and lab[j] == lab[i]: j += 1
    if j - i < 5 and notes and notes[-1][1] == i: notes[-1][1] = j      # blip inside a slide: keep with the note before
    else: notes.append([i, j])
    i = j
rows = [(a, b, float(np.nanmedian(midi[a:b]))) for a, b in notes if b - a >= 3]
for k in shifts:
    np.savetxt(f'{out}/notes{k}.txt', [(a * hop, b * hop, c, round(c) + k, 1) for a, b, c in rows], fmt='%d %d %.4f %.4f %d')
print(len(rows), 'notes, median midi', round(float(np.nanmedian(midi)), 1))
