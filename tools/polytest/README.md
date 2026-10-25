# Poly detection regression test (vs Melodyne 5)

- `wrld_midi2_flkeys.wav`: Cymatics WRLD MIDI 2 (D minor, 140 BPM) played by FL Keys Grand Piano, rendered from FL Studio.
- `gt.py`: exact notes of bars 1-4 (33 notes, incl. bass line) + Melodyne 5 (Polyphonic Sustain) detections on the same file.
- `score.py`: scores `nt_out.txt` (PolyBench `file` output) vs the MIDI: exact pitch, onset within one 16th.

Baseline (2026-10-25): Melodyne F1 0.65 (precision 82%, recall 55%); NovaTone F1 0.30 (precision 20%, recall 58%).

    NovaTonePolyBench file tools/polytest/wrld_midi2_flkeys.wav > tools/polytest/nt_out.txt
    cd tools/polytest && python3 score.py
