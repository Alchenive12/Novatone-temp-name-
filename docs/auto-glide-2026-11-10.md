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

## Versus Melodyne (yudy groovy, Melodyne renders of Nov 9, `tools/retune-bench/cmp_mel.py`)
Each render judged per note against its own shift, so Melodyne's key-snapped note choices don't count against it.

| Move | Sung shape bent inside notes (Mel / old / new) | at note edges | Level vs dry | Timbre change vs dry |
|---|---|---|---|---|
| tune only | 9 / 14 / 6 c | 27 / 39 / 17 c | -0.3 / -0.2 / 0.0 dB | 3.1 / 2.0 / 1.6 dB |
| +1 st | 15 / 17 / 9 c | 26 / 42 / 29 c | -0.8 / -0.3 / 0.0 dB | 3.9 / 3.6 / 4.1 dB |
| +3 st | 13 / 18 / 6 c | 34 / 59 / 52 c | -0.2 / -2.2 / -0.5 dB | 7.6 / 6.5 / 7.0 dB |

Reading: on these numbers the new build is level with Melodyne inside notes and on level; Melodyne is smoother at
note edges at +3. The numbers do not measure grain/phase artifacts - only ears do. "How close notes land to the
semitone" is not comparable (the bench and the metric share one pitch reader; Melodyne uses its own note centres).

## Round 3: note edges at +3
The "edges 52 c vs Melodyne 34 c" figure was mostly not real: the frames that hurt were consonants, breaths and
fading tails where the bench's pitch reader invents a pitch and the engine (correctly) passes the sound dry.
Measured only where the dry take is clearly pitched (`tools/retune-bench/edge2.py`: pYIN voiced prob > 0.8,
level > -30 dB), the real gap was smaller and one-sided: notes that touch the note before them started ~25 ms late.

| +3 st, clearly pitched frames | Melodyne | before | now |
|---|---|---|---|
| inside notes | 8 c | 5 c | 5 c |
| note edges (70 ms) | 8 c | 14 c | 11 c |
| first frame of a touching note | 7 c | 57 c | 38 c |

(+1 st: edges 7 / 15 / 11 c.)

- Cause: the per-block note lookup looked two roll hops back; that is right for the first sample of a 512 block but
  late for the block as a whole. Now it aims at the block's middle (`PluginProcessor.cpp`), one hop at 512.
- The shift curve is now applied after the Humanize onset ramp and breathy-tail release, so a quiet tail of a
  moved note is not let go back towards the unmoved pitch (`TuneEngine.h`). Small effect on this take.
- What is left against Melodyne (11 vs 8 c) is the auto-glide itself: where two moved notes with different shifts
  meet, the shift slides between them over up to 90 ms, and this metric counts that slide as error.
- Tried and dropped (no measurable gain): letting the tracker follow breathy starts/tails inside a roll note at
  lower clarity.

## Round 4: "+3 still has noticeable artifacts"
On clearly sung frames nothing separates this build from Melodyne (`tools/retune-bench/rough.py`: periodicity,
level flutter, both level with dry). What does: short bits INSIDE moved notes that came out unshifted
(`leak.py`) - at +3 st that is a 3-semitone pitch blip. Melodyne 0.1 % of note frames, this build 0.9 %.

Causes found in the engine trace:
- Note start (8.03 s): the tracker stayed on the previous note / a harmonic (19 st away, clarity 0.65) for ~50 ms
  while a clear pitch (0.77-0.96) sat on the new note; then lost one more hop to the jump guard.
- Voiced consonants (13.83 s start, 13.27 s and 33.19 s ends): the fricative gate (zero-crossing rate) calls the
  frame unvoiced although it is clearly pitched (0.5-0.76), so it passes dry and the correction is zeroed.

Fix (`TuneEngine.h`): inside a moved roll note, when the tracker has no pitch or is more than 4 st from the note's
sung centre, but a candidate with clarity >= 0.45 lies within 4 st of that centre, use it (and skip the jump guard
and the sibilant zeroing for that hop). 0.38 fixed more ends but let one 11 ms growl back in.

| +3 st | Melodyne | before | now |
|---|---|---|---|
| unshifted frames inside moved notes | 0.1 % | 0.9 % (5 notes) | 0.4 % (3 notes, 17-23 ms, all note ends) |
| sung shape bent, all frames: inside notes / edges | 13 / 34 c | 25 / 65 c | 8 / 45 c |
| growl spots, octave drops | - | 0 / 0 | 0 / 0 |

Left: three note ends fading into a consonant or into near-silence (below the energy gate). Added hiss above
6 kHz is +1.4 dB vs dry (Melodyne -1.1 dB) - not addressed.
Limit of this method: the bench can only find artifacts that show up as pitch, level, periodicity or
sub-fundamental energy. Anything else needs a timestamp from the owner's ears.

## Round 5: closing the open items (branch `remote/close-open-items`)
Owner listened to PR #6: sounds good. This round closes what was still listed as open.
Verification: level 2 (offline bench, ASan/UBSan clean, Linux VST3 + Standalone build) + CI. Not heard in a DAW.
The yudy groovy vocal was not reachable this session (PC folder access not granted), so the numbers below are
from synthetic vocals (`mk3.py`, `mk3b.py`) - re-check on yudy groovy at home.

| Item | Change | Result |
|---|---|---|
| Drawn curve showed the old prediction | `PitchTraceComponent::shiftCurveAtHop` is now the one place the glided shift is built; the engine sync and the drawn cyan curve both use it, and the drawn curve applies it the way `TuneEngine` does (sung pitch + Center x shift, blended by Variation, only within 7.5 st) | piano roll draws what is played. Not looked at on screen |
| Note-end blips (tails fading below the energy gate pass dry) | `TuneEngine.h`: inside a moved roll note the energy gate is 12 dB lower; the roll-guided pick still needs clarity >= 0.45 near the note | `mk3b` +3 st: unshifted frames inside moved notes 0.5 % (1 note, 17 ms) -> 0.1 % (none >= 3 frames); growl spots 0 / 0 |
| Hiss above 6 kHz when shifting up | `TuneEngine.h`, in the existing sibilant crossover: the band above 6 kHz of the shifted signal is never louder than the dry band (reduce only, max -6 dB, 30 ms) | `mk3` +3 st, 6-8 kHz vs dry: +0.8 -> +0.5 dB, 8-16 kHz: +0.5 -> +0.1 dB. Halves it, does not zero it |
| Old sessions opened Note split on More | `TuneMacro::noteSplitKnobFromSaved`, used in `setStateInformation`: an off-grid (old 0-100 %) value opens on the mode with the nearest sensitivity | old 73 % -> Balanced. `tools/retune-bench/notesplit_test.cpp` |

Unchanged on the legato bench (`ev3.py` A/B): core 3.8 c, edges 10.6 c, level +0.02 dB - same as before.

Not done: poly clip mode has no auto-glide or level match. It is a separate (spectral) renderer and would be a new
feature, not a fix - left until it is asked for.
