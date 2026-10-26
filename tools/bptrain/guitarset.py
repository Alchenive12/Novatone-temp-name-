"""GuitarSet (Xi et al. 2018, CC BY 4.0): mono-mic audio + JAMS note annotations -> Basic Pitch-format windows.
Player 05 is held out for testing (standard split)."""
import json, os, glob, numpy as np, soundfile as sf, scipy.signal as sg
SR = 22050; N = 43844; HOP = 256; FR = 172

def load_notes(jams_path):
    j = json.load(open(jams_path))
    notes = []
    for a in j['annotations']:
        if a['namespace'] == 'note_midi':
            for d in a['data']:
                notes.append((float(d['time']), float(d['duration']), float(d['value'])))
    return notes

def tracks(root, players):
    out = []
    if not root or not os.path.isdir(root): return out
    wavs = {os.path.basename(w)[:-8]: w for w in glob.glob(os.path.join(root, '**', '*_mic.wav'), recursive=True)}
    for jp in sorted(glob.glob(os.path.join(root, '**', '*.jams'), recursive=True)):
        base = os.path.basename(jp)[:-5]
        if base[:2] in players and base in wavs: out.append((wavs[base], jp))
    return out

class Track:
    def __init__(self, wav, jp):
        x, sr = sf.read(wav, dtype='float32')
        if x.ndim > 1: x = x.mean(1)
        self.audio = sg.resample_poly(x, SR, sr).astype(np.float32) if sr != SR else x
        self.notes = load_notes(jp)

def labels_for(notes, t0):
    note = np.zeros((FR, 88), np.float32); onset = np.zeros((FR, 88), np.float32); cont = np.zeros((FR, 264), np.float32)
    for (s, d, v) in notes:
        m = int(round(v)); 
        if not 21 <= m <= 108: continue
        f0 = int(np.floor((s - t0) * SR / HOP)); f1 = int(np.ceil((s + d - t0) * SR / HOP))
        if f1 < 0 or f0 >= FR: continue
        cb = (v - 21) * 3 + 1
        for f in range(max(0, f0), min(FR, f1)):
            note[f, m - 21] = 1
            for k in range(int(cb) - 1, int(cb) + 3):
                if 0 <= k < 264: cont[f, k] = max(cont[f, k], float(np.exp(-0.5 * ((k - cb) / 0.6) ** 2)))
        if 0 <= f0 < FR: onset[f0, m - 21] = 1
    return note, onset, cont

def random_window(track, rng):
    if len(track.audio) <= N: return None
    s = int(rng.integers(0, len(track.audio) - N)); t0 = s / SR
    a = track.audio[s:s + N].copy(); g = np.abs(a).max()
    if g < 1e-4: return None
    a *= rng.uniform(0.2, 0.9) / g
    return (a,) + labels_for(track.notes, t0)
