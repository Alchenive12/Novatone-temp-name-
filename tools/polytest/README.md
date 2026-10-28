# Poly detection regression test (vs Melodyne 5)

- `wrld_midi2_flkeys.wav`: Cymatics WRLD MIDI 2 (D minor, 140 BPM) played by FL Keys Grand Piano, rendered from FL Studio.
- `gt.py`: exact notes of bars 1-4 (33 notes, incl. bass line) + Melodyne 5 (Polyphonic Sustain) detections on the same file.
- `score.py`: scores `nt_out.txt` (PolyBench `file` output) vs the MIDI: exact pitch, onset within one 16th.

Baseline (2026-10-25): Melodyne F1 0.65 (precision 82%, recall 55%); NovaTone F1 0.30 (precision 20%, recall 58%, 95 notes found).

After fragment merging + ghost pruning (2026-10-26): NovaTone F1 0.48 (precision 40%, recall 58%, 47 notes found).
Round 2 (2026-10-26): range up to E7 + a second pass for high near-pure fundamentals (piano top line):
NovaTone F1 0.52 (precision 45%, recall 64%: now finds more real notes than Melodyne, 21 vs 18).
Synthetic clip bench F1 83%. Still behind Melodyne: octave ghosts over low piano notes (2nd-harmonic
residue after subtraction; taking more of each partial breaks real octave stacks), G1 (below A1).

    NovaTonePolyBench file tools/polytest/wrld_midi2_flkeys.wav > tools/polytest/nt_out.txt
    cd tools/polytest && python3 score.py
