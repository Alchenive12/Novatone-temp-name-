"""Synthetic polyphonic training windows with exact labels in Basic Pitch's format.
Instruments: piano (inharmonic, decaying), plucked guitar (Karplus-Strong), sung voice (formants, vibrato),
strings/pad, organ/synth, bass. 1-6 simultaneous notes, chords + lines, reverb, noise, detuning."""
import numpy as np, scipy.signal as sg
SR = 22050; N = 43844; HOP = 256; FR = 172          # Basic Pitch window / frames
def midi_hz(m): return 440.0 * 2 ** ((m - 69) / 12)

def _env_adsr(n, a, d, s, r, sustain_len):
    e = np.zeros(n); a = max(1, int(a * SR)); d = max(1, int(d * SR)); sl = max(0, sustain_len)
    e[:a] = np.linspace(0, 1, a)[:n]
    if a < n: e[a:a + d] = np.linspace(1, s, d)[:max(0, n - a)]
    if a + d < sl: e[a + d:sl] = s
    rel = max(1, int(r * SR)); tail = e[min(sl, n - 1)] if sl < n else 0
    if sl < n: e[sl:sl + rel] = np.linspace(tail, 0, rel)[:n - sl]
    return e

def render_note(inst, f0_curve, dur_n, rng):
    """f0_curve: per-sample Hz (len n). Returns mono signal."""
    n = len(f0_curve); t = np.arange(n) / SR; ph = 2 * np.pi * np.cumsum(f0_curve) / SR; f0 = f0_curve[0]
    out = np.zeros(n)
    if inst == 'piano':
        B = 0.0001 * (f0 / 262) ** 1.4
        for k in range(1, 40):
            fk = k * np.sqrt(1 + B * k * k)
            if fk * f0 > 9000: break
            amp = (1 / k ** rng.uniform(0.8, 1.6)) * (rng.uniform(0.5, 1.4) if k == 2 else 1)  # strong 2nd harmonic sometimes
            out += amp * np.exp(-t * (0.4 + 0.3 * k) * (f0 / 262) ** 0.5) * np.sin(fk * ph + rng.uniform(0, 6.3))
        out *= np.minimum(1, t / 0.003); out[dur_n:] *= np.exp(-(t[dur_n:] - t[dur_n]) / 0.08) if dur_n < n else 1
    elif inst == 'guitar':                                   # Karplus-Strong with body tone
        p = max(2, int(round(SR / f0))); buf = rng.uniform(-1, 1, p); buf = sg.lfilter([1], [1, -rng.uniform(0.2, 0.7)], buf)
        y = np.zeros(n); damp = rng.uniform(0.993, 0.999)
        for i in range(n):
            y[i] = buf[i % p]; buf[i % p] = damp * 0.5 * (buf[i % p] + buf[(i + 1) % p])
        b, a = sg.iirpeak(rng.uniform(90, 220), 3, SR); out = y + 0.6 * sg.lfilter(b, a, y)
        if dur_n < n: out[dur_n:] *= np.exp(-(t[dur_n:] - t[dur_n]) / 0.03)
    elif inst == 'voice':
        src = sum(np.sin(k * ph) / k ** 1.1 for k in range(1, 30) if k * f0 < 8000)
        F = [(730, 1090, 2440), (270, 2290, 3010), (530, 1840, 2480), (570, 840, 2410), (440, 1020, 2240)][rng.integers(5)]
        y = np.zeros(n)
        for fc, bw in zip(F, (90, 110, 170)):
            b, a = sg.iirpeak(fc, fc / bw, SR); y += sg.lfilter(b, a, src)
        out = y * _env_adsr(n, 0.04, 0.1, 0.9, 0.12, dur_n)
    elif inst == 'strings':
        saw = sum(np.sin(k * ph + rng.uniform(0, 6.3)) / k for k in range(1, 40) if k * f0 < 9000)
        b, a = sg.butter(2, min(0.99, rng.uniform(1500, 5000) / (SR / 2))); out = sg.lfilter(b, a, saw) * _env_adsr(n, rng.uniform(0.05, 0.3), 0.2, 0.8, 0.3, dur_n)
    elif inst == 'organ':
        out = sum(rng.uniform(0.2, 1) * np.sin(k * ph) for k in (1, 2, 3, 4, 6, 8) if k * f0 < 9000) * _env_adsr(n, 0.01, 0.05, 0.9, 0.05, dur_n)
    else:  # bass
        sq = np.sign(np.sin(ph)) * 0.6 + np.sin(ph); b, a = sg.butter(2, min(0.99, 900 / (SR / 2))); out = sg.lfilter(b, a, sq) * _env_adsr(n, 0.005, 0.2, 0.6, 0.08, dur_n)
    return out

def make_window(rng):
    """Returns audio (N,), note (FR,88), onset (FR,88), contour (FR,264)."""
    audio = np.zeros(N + SR); note = np.zeros((FR, 88), np.float32); onset = np.zeros((FR, 88), np.float32); cont = np.zeros((FR, 264), np.float32)
    insts = rng.choice(['piano', 'guitar', 'voice', 'strings', 'organ', 'bass'], size=rng.integers(1, 3), replace=False)
    detune = rng.uniform(-0.3, 0.3)                       # whole-recording tuning offset (semitones)
    style = rng.choice(['chords', 'lines', 'arp'])
    key = rng.integers(0, 12); scale = [0, 2, 3, 5, 7, 8, 10] if rng.random() < 0.5 else [0, 2, 4, 5, 7, 9, 11]
    events = []
    t = rng.uniform(-0.5, 0.2)
    while t < 2.0:
        poly = 1 if style == 'lines' else rng.integers(2, 7)
        dur = rng.uniform(0.12, 1.2) if style != 'arp' else rng.uniform(0.08, 0.3)
        root = rng.integers(36, 72)
        degs = sorted(rng.choice(len(scale) * 3, size=poly, replace=False))
        for d in degs:
            m = key + 12 * (root // 12) + scale[d % 7] + 12 * (d // 7)
            if 28 <= m <= 100: events.append((t, dur, int(m), rng.choice(insts), rng.uniform(0.3, 1.0)))
        t += dur * (rng.uniform(0.5, 1.0) if style == 'arp' else rng.uniform(0.8, 1.3))
    for (t0, dur, m, inst, vel) in events:
        s0 = int(t0 * SR); ln = int((dur + 0.5) * SR); dn = int(dur * SR)
        vib = 0.0
        if inst in ('voice', 'strings') and rng.random() < 0.7:
            vib = rng.uniform(0.1, 0.4) * np.sin(2 * np.pi * rng.uniform(4.5, 6.5) * np.arange(ln) / SR) * np.clip(np.arange(ln) / (0.25 * SR), 0, 1)
        mm = m + detune + vib
        sig = vel * render_note(inst, midi_hz(mm) * np.ones(ln) if np.isscalar(mm) else midi_hz(mm), dn, rng)
        a, b = max(0, s0), min(len(audio), s0 + ln)
        if b > a: audio[a:b] += sig[a - s0:b - s0]
        # labels (frame i covers time i*HOP/SR)
        f0, f1 = int(np.floor(t0 * SR / HOP)), int(np.ceil((t0 + dur) * SR / HOP))
        for f in range(max(0, f0), min(FR, f1)):
            note[f, m - 21] = 1
            mf = m + detune + (vib[min(ln - 1, max(0, int(f * HOP - s0)))] if not np.isscalar(vib) else 0)
            cb = (mf - 21) * 3 + 1                          # contour bin (bin 1 = A0)
            for k in range(int(cb) - 1, int(cb) + 3):
                if 0 <= k < 264: cont[f, k] = max(cont[f, k], float(np.exp(-0.5 * ((k - cb) / 0.6) ** 2)))
        if 0 <= f0 < FR: onset[f0, m - 21] = 1
    audio = audio[:N]
    if rng.random() < 0.7:                                  # room reverb
        L = int(rng.uniform(0.2, 1.2) * SR)
        ir = rng.normal(0, 1, L) * np.exp(-np.arange(L) / (rng.uniform(0.05, 0.4) * SR))
        wet = sg.fftconvolve(audio, ir)[:N]; audio = audio + rng.uniform(0.05, 0.3) * wet / (np.abs(wet).max() + 1e-9) * np.abs(audio).max()
    audio += rng.normal(0, rng.uniform(0, 0.01), N)
    audio = audio / (np.abs(audio).max() + 1e-9) * rng.uniform(0.2, 0.9)
    return audio.astype(np.float32), note, onset, cont

if __name__ == '__main__':
    import time, soundfile as sf
    rng = np.random.default_rng(1); t = time.time(); a, n, o, c = make_window(rng)
    print('one window', round(time.time() - t, 2), 's', a.shape, int(n.sum()), 'note-frames', int(o.sum()), 'onsets')
    sf.write('/tmp/claude-0/synth_example.wav', a, SR)
