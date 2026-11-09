# Smooth retune - findings and changes (2026-11-09)

Goal: moving a sung note (e.g. +1 / +3 st) should sound like the singer sang it - no buzz, phasiness or robotic wobble.
Branch: `remote/smooth-retune` (commit 7fd112b, CI green on Linux + Windows, built x64 Release on the PC and installed
to `C:\Users\didi\AppData\Local\Programs\Common\VST3\NovaTone.vst3`; previous build kept as `NovaTone.vst3.prev-a4b63bc`).
Owner's first listening verdict: "sounds way better".

## How we got there
1. Argon brainstorm (brief: `prompts/02-argon-smooth-retune-brainstorm.md`) - read the shifter / TuneEngine code +
   research, wrote 16 ranked ideas with an auto-glide design sketch. ~4% of Argon's weekly quota.
2. Claude checked each claim against the code, built an offline bench (`tools/retune-bench/`) running the real
   TuneEngine, and A/B-tested old vs new on a synthetic sung signal and on the yudy groovy vocal
   (`Samples/yudy groovy_2026-07-25 18-45-48_Insert 1 - Part_1.wav`, ~290 Hz female-range voice).

## Changes in 7fd112b
| Change | File | Why | Bench result |
|---|---|---|---|
| First grain starts on the newest pitch mark behind the synthesis frontier | `PsolaShifter.h` placeGrains | The old "3 marks back" start never got behind the frontier when 2 periods + maxPeriod < synthesisLag, i.e. voices above ~160 Hz at 44.1 kHz: **the shifter stayed silent** until a low note came by | yudy groovy vocal: old engine outputs silence without this fix |
| Unvoiced bypass: breaths / s / sh / t play dry (latency aligned), ~4 ms crossfade | `PsolaShifter.h` | Unpitched audio was resynthesised from fixed 200 Hz grains = tonal buzz | breath vs dry: +1.6 dB (totally different) -> -50 dB (identical) |
| At Variation >= 100% keep the singer's own pitch deviation 1:1 around the new centre (no 28% squash, no +-0.12 st clamp, no intent-slope reshaping) | `TuneEngine.h` computeDesiredMidiAtHop | Moved notes lost ~70% of their natural micro-pitch -> robotic | Variation 0 unchanged (identical numbers) |
| Humanize's synthetic sine drift (1.37 / 2.83 Hz) + 5.4 Hz "gentle vibrato" fade out at natural Variation | `TuneEngine.h` | They only stood in for flattened expression; on top of real expression they are fake vibrato | non-vibrato note: movement correlation with the singer -0.09 -> 0.22 |
| Over-unity amp normalisation continuous in the ratio | `PsolaShifter.h` | old `> 1.02` switch = small level step at ~+34 cents | - |

## Argon ideas: verdicts
- I1 "pitch-mark rounding bug" - **rejected**. Adding the correlation lag to the exact (fractional) previous mark is
  correct; Argon's version re-anchors on the rounded mark every period, a random walk of the phase.
- I2 keep 100% micro-pitch / remove synthetic LFOs - **done** (above).
- I6 unvoiced bypass - **done**.
- I5 COLA window-sum normalisation - partly (continuous ampNorm); full window-sum normalisation still open.
- Open, in suggested order: I3 + I4 (one shared pitch curve for piano roll and engine - the GUI uses 512-sample hops,
  the engine 256 - then **auto-glide in the shift domain**: glide the *shift* between notes, not the absolute pitch,
  18-30 ms for fast articulation, 45-90 ms legato, notes after a gap start already shifted); I7 dual-mark interpolated
  grains for upward shifts; I12 separate Drift vs Modulation like Melodyne; I9 formant correction at +3 st and more;
  I10 offline two-pass render for Mono clip mode; I14 55 Hz high-pass on the mark-placement signal.

## yudy groovy A/B (36 s vocal, Center 100 %, Variation 100 %, Humanize 50 %, every note snapped + shifted)
| Version | median shift | mean cents from note centre | level vs dry |
|---|---|---|---|
| NovaTone old (+start fix) tuned / +1 / +3 | -0.1 / +0.8 / +2.6 | 15 / 16 / 22 | 0 / 0 / -2 dB at +3 |
| NovaTone new tuned / +1 / +3 | -0.1 / +0.7 / +2.6 | 20 / 20 / 24 (more natural movement kept) | 0 / 0 / -2 dB at +3 |
| Melodyne 5 Studio (FL, Transfer, Center 100 Drift 0) | -0.5 / +0.5 / +2.5 (snapped to detected D minor) | 15 / 15 / 15 | 0 dB at +3 |

Notes:
- The owner found the Melodyne render phasey. Checked: L/R identical, no dry copy mixed in (best match with the dry
  vocal 0.01-0.045 vs ~1.0 for a leak), level not doubled. Likely Melodyne's algorithm (possibly "Universal").
  To redo fairly: Algorithm -> Melodic, Correct Pitch 100/0, +3, export; compare with NovaTone's +3.
- NovaTone loses ~2 dB at +3 st (ampNorm `ratio^-0.65`); Melodyne keeps level -> to fix.
- Exports were sent to the owner as `novatone-ab/0..9_*.wav`; Melodyne renders are in the project's `_novatone_test` folder.

## Next session
1. Owner's listening notes on B vs C (Melodyne Melodic re-render when home Thursday).
2. Level loss at +3 (proper window-sum normalisation).
3. Shared pitch curve + shift-domain auto-glide.
