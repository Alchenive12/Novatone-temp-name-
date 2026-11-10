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

## Real vocal check (yudy groovy, 142 notes, every note snapped + moved)
`tools/retune-bench/prep_notes.py` + `render_ab3.py` (harness3; old = main a5726b3, new = this branch).
Pitch error = how far (output pitch - sung pitch) is from each note's own shift, i.e. how much of the sung shape was bent.

| Move | inside notes old -> new | note edges old -> new | level vs dry old -> new |
|---|---|---|---|
| snap only | 32 -> 5 c | 40 -> 18 c | -0.16 -> 0.00 dB |
| +1 st | 41 -> 10 c | 52 -> 32 c | -0.27 -> -0.03 dB |
| +3 st | 106 -> 23 c | 97 -> 60 c | -2.16 -> -0.30 dB |

Not yet judged by ear. Note: at Variation 100 % a note that drifts while held keeps its drift (only its centre moves);
lower Variation still flattens it.

## Round 2 (same day): "monster" artifact + Note split modes
Owner's ears on the first A/B: better half the time, but split-second "monster" growls that the old build did not have.

**Cause.** The live pitch tracker can lock an octave LOW when a phrase starts breathy (the double period is just as
periodic) and continuity then holds it there - on yudy groovy for a whole 1.6 s phrase at 19.1 s, 8 regions / 2.0 s in
total. PSOLA then cuts two-period grains, and shifting those gives an octave-down growl. The old build hid it by
accident: an override more than 12 st from the detected pitch was ignored, so those regions were simply left
unshifted (a wrong note, but no growl). The shift curve applied the move anyway.

**Fixes** (`TuneEngine.h`, + `sungMidi` passed through the hop slot):
1. Tracker: inside a note, if the octave above is equally clear (clarity >= 0.90 and within 0.03), take it.
   A genuinely doubled/creaky voice is clearly less periodic at the half period and stays put.
2. Octave guard towards the roll note's sung centre now works in both directions (was: only octave-too-high).
3. The shift curve is only used while the live pitch is within 7.5 st of the roll's note; otherwise old behaviour.

Result on yudy groovy: octave-wrong regions 8 (2.0 s) -> 3 (0.19 s, all <= 145 ms and not shifted); octave-down
spots in the render 1 x 540 ms (+1) / 700 ms (+3) -> none; sub-fundamental growl spots at +3: 3 -> 0
(`tools/retune-bench/find_sub.py`). Pitch/level numbers unchanged from the table above. Not yet re-heard.
Caveat: in the plugin fix 2 leans on the piano roll's own analysis of the note being in the right octave.

**Note split knob** (`tune_sensitivity`, same ID): now 5 modes - Fewest / Fewer / Balanced / More / Most
(knob at 0 / 25 / 50 / 75 / 100 %). Balanced = the old default (old knob 73 %); the others step evenly
(sensitivity 0.455 / 0.591 / 0.7275 / 0.864 / 1.0). The SOFT-HARD macro no longer moves it. Style buttons:
Natural -> Fewer, Pop -> Balanced, Hard -> More. Sessions saved with the old knob load on the nearest mode
(an untouched old default lands on More - set it back to Balanced once).
