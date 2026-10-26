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
