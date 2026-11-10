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

## Auto-glide bench (2026-11-10)
- `mk2.py` - legato phrase with scoops, slides between notes, vibrato and a breath (`in.f32`, `notes_base.txt`).
- `harness3.cpp` - `h3 in.f32 out.f32 notes.txt variation humanize lagSamples` - feeds the engine the piano roll's
  notes the way the plugin does (per-hop target + `ShiftGlide` shift curve). Builds against old commits too
  (no shift curve there). Build like `harness2.cpp`.
- `ev3.py A|B exeA,exeB lag...` - A: every note snapped +1; B: two notes +2 between untouched neighbours.
  Prints how far (output pitch - input pitch) is from each note's shift, in the note core and at its edges.
- `lvl.py exe...` - output level vs shift (-5..+5 st), using `harness2`.
- `glide_test.cpp` - assert self-check of `Source/DSP/ShiftGlide.h` (no JUCE).
- `prep_notes.py <vocal.wav> <dir> [shifts]` + `render_ab3.py <dir> <out> <h3_old> <h3_new> [shifts]` - the same
  old-vs-new comparison on a real vocal (notes from pYIN, every note snapped and moved), with WAVs to listen to.

## Open-items bench (2026-11-10, evening)
- `mk3.py` / `mk3b.py` - synthetic female-range vocal with breath noise, voiced "z" note ends and tails fading
  into near-silence. Warning: they did NOT predict the real vocal (blips and brightness behaved differently) -
  use yudy groovy for DSP decisions.
- `render.py <h3> <label> <shifts...>` - renders `prep/` (from `prep_notes.py`) to `r_<label>_<k>.wav`.
- `bands.py label=path ...` - output vs dry power per band on clearly voiced frames (where the hiss sits).
- `notesplit_test.cpp` - assert self-check of the Note split mapping for sessions saved with the old knob.
