# Retune bench (offline, no DAW)

Runs NovaTone's real `TuneEngine` on a mono float32 file so old vs new DSP can be compared with numbers and WAVs.
Used on 2026-11-09 for the smooth-retune work (see `docs/smooth-retune-2026-11-09.md`).

## Build (Linux, g++)
```bash
# JUCE 8.0.4 modules, compiled once
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git ~/JUCE
J=~/JUCE/modules
for m in juce_core juce_audio_basics juce_audio_formats juce_dsp; do
  g++ -std=c++17 -O2 -include juceconf.h -I $J -c $J/$m/$m.cpp -o $m.o & done; wait
echo 'namespace juce { const char* juce_compilationDate = __DATE__; const char* juce_compilationTime = __TIME__; }' > stamp.cpp
g++ -c stamp.cpp -o stamp.o
S=../../Source   # or a worktree of another commit, for A/B
g++ -std=c++17 -O2 -include juceconf.h -I $S -I $S/DSP -I $J harness2.cpp \
  $S/DSP/MPMDetector.cpp $S/DSP/Quantizer.cpp $S/DSP/SmoothingFilter.cpp $S/DSP/VoicingGate.cpp \
  $S/DSP/LookaheadBuffer.cpp juce_*.o stamp.o -o h2 -lpthread -ldl
```

## Tools
- `harness.cpp` - `h in.f32 out.f32 targetMidi variation humanize` - one fixed live-override target for the whole file.
- `harness2.cpp` - `h2 in.f32 out.f32 targets.f32 variation humanize` - one override target per 512-sample block
  (<= 0 = no override), output latency-compensated. Emulates moving every note in the piano roll.
- `mk.py` - synthetic sung test signal (`in.f32`, `in_midi.npy`): 3 notes with drift/jitter, one with vibrato,
  a breath and an "s" between them.
- `ev.py`, `ev2.py` - pYIN analysis: output centre, how much of the singer's own pitch movement is kept and how well
  it correlates, and how far breaths differ from the dry input. `ev2.py` reports per note.

Targets for a real vocal: pYIN the input (hop 512), median-smooth over ~13 frames, round to semitones, add the shift,
and look the note up at (block start - 1024 samples) - see the notes in the findings doc.
Needs `pip install librosa soundfile`.
