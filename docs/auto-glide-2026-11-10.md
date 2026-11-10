# Auto-glide + level match (2026-11-10)

Branch `remote/auto-glide`. Follows `smooth-retune-2026-11-09.md` ("Next session" items 2 and 3).
Verification: level 2 (offline bench with the real TuneEngine, ASan/UBSan clean) + CI. Not yet heard in a DAW.

## What was still robotic
With a note moved in the piano roll the engine aimed at `target - running centre`. The running centre starts on the
first pitch it hears, so the singer's scoop into a note was pulled flat for ~100 ms (up to +87 cents wrong on the
test phrase), slides between notes were redrawn, and each note's last ~23 ms already got the next note's target.
Shifted notes also got quieter (-2 dB at +3 st, -3 dB at +-5).

## Changes
| Change | File | Result on the bench (`tools/retune-bench`, legato phrase, every note +1) |
|---|---|---|
| Moved notes follow a shift curve: constant shift = new centre - whole-note sung centre (per-note Center included), scaled by the global Center knob, blended in by Variation (100 % = pure shift, 0 % = old behaviour, unchanged) | `TuneEngine.h`, `PluginProcessor.cpp` | pitch error inside notes 17 -> 4 cents RMS; note edges 58 -> 10 cents RMS; worst 158 -> 83 cents (the 83 is the cold start of the file) |
| Auto-glide: where a moved note touches a neighbour the shift glides (raised cosine, ramp handle x note length, max 90 ms) from the neighbour's shift; two moved notes share the glide half and half; after a breath/gap the note starts already shifted | `ShiftGlide.h` (new, no JUCE), `PitchTraceComponent.cpp` | one note +2 between untouched notes: old jumped to +320 then settled to +218; new rises 0 -> 220 smoothly; biggest frame-to-frame step 90 -> 40 cents |
| Note lookup delayed by the engine's analysis delay (half window, 2 roll hops) | `PluginProcessor.cpp` | note ends no longer take the next note's pitch |
| Level match: PSOLA output follows the dry input's short-term power (30 ms, gain limited to -3..+4 dB) | `PsolaShifter.h` | level vs dry at -5..+5 st: was -3.6..-0.7 dB, now 0.0 dB everywhere |
| A hand-placed note lands exactly on its pitch (Humanize no longer loosens it by 6 %) | `TuneEngine.h` | bias -6 -> 0 cents |

## Known limits
- The ramp handles now set the glide length for moved notes; the piano roll's drawn curve still shows the old
  prediction (not yet the shift curve).
- Next to an unedited note the whole glide sits inside the moved note (unedited notes have no per-hop data).
- An unedited neighbour is treated as shift 0; with the Center knob up the engine still auto-tunes it a little.
- Poly clip mode is untouched (separate renderer).

## Next
Listen at home (yudy groovy chorus, +1 / +3, single note moved), then: drawn curve = shift curve,
I7 dual-mark grains for upward shifts, I9 formant correction above +3 st.
