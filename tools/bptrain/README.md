# bptrain: Basic Pitch head fine-tune (2026-10-26 run)

Frozen ONNX front end (CQT + harmonic stack) + trainable 6-conv head, written back into the stock ONNX.
Run on the PC with `job.bat` (needs PY = python with CUDA torch, plus `nmp_stock.onnx`, `test_piano.wav`
and `test_guitar.wav` from `tools/polytest`). Mix: 70% GuitarSet (player 05 held out), 30% synthetic.
3 seeds x 3000 steps, batch 16, lr 1e-5. About 23 min per seed on the RTX.

## Result: no ship

| model | piano F1 | right octave | guitar /36 | extras | synth F1 | GuitarSet-05 F1 |
|---|---|---|---|---|---|---|
| stock | 0.605 | 88% | 28 | 2 | 0.624 | 0.821 |
| s0 | 0.556 | 80% | 24 | 2 | 0.704 | 0.822 |
| s1 | 0.563 | 83% | 25 | 2 | 0.707 | 0.824 |
| s2 | 0.514 | 78% | 23 | 2 | 0.702 | 0.821 |

GuitarSet barely moves: stock Basic Pitch was already trained on GuitarSet, so there's nothing new to learn from it.
The synthetic gain doesn't transfer and costs piano and guitar. Lower thresholds don't rescue it (`sweep.py`).

## Free win instead (no training)
Stock model, `frame_thresh` 0.30 -> 0.25: piano F1 0.605 -> 0.658 (Melodyne 0.65), right octave 93%, guitar 28 -> 30/36, extras 2.
Next try if training again: data Basic Pitch never saw (real stems with MIDI), not more GuitarSet or synthetic.

## Quiet-note test (`stair_test.py`, NSynth test set, real notes)
Loud base note + quieter target at an interval, -4 dB per step; limit = first level heard < 50%.
Halluc. = target pitch reported when it isn't played. Full table: `stair_stock.txt`.

| interval | Basic Pitch limit | BP halluc. | NovaTone classic limit |
|---|---|---|---|
| octave +12 | -16 dB | 18% | 0 dB (misses even at equal level) |
| 12th +19 | -4 dB | 0% | 0 dB |
| 2 oct +24 | -12 dB | 8% | -4 dB |
| 5th +7 | -28 dB | 2% | -16 dB |
| 3rd +4 / +3 | -32 dB | 0% | -16 dB |
| octave below -12 | below -36 dB | 0% | -24 dB |

Weak spot = notes on the harmonics of the note below (octave, 12th, 2 octaves). The octave already hallucinates 18%,
since a quiet octave on top looks like a strong 2nd harmonic.
`train_stair.py` + `stair.bat`: staircase fine-tune on the NSynth train set. Each interval goes -2 dB when heard >= 70%
and freezes (+2 dB back) as soon as hallucinations rise > 2 points over stock.

## Staircase run result (2026-10-26): no ship, stopped at step ~2500
Two-way staircase, NSynth train set (206k real notes). Detection on the probe stayed flat (~0.5 at stock-limit levels).
Full quiet-note test at step 2250 vs stock (limit dB / halluc.): octave -8 vs -16 (halluc. 5% vs 18%),
12th -8 vs -4 (10% vs 0%), 2 oct 0 vs -12, 5th -20 vs -28, 3rd maj -32 = -32, min -28 vs -32. Worse overall.
It traded hearing for fewer octave guesses. The frozen front end + small head is at its limit; more data/curriculum won't move it.
Note: the freeze rule fired on 1-case noise (24 probe pairs = 4%/case > 2% margin); use >= 3 cases next time.
