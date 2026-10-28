# Guitar agreement test vs Melodyne 5

Cymatics Titan Guitar Chord Loop 8 (140 BPM, F# min). No MIDI exists, so this measures agreement with Melodyne's notes (read from its editor), not correctness.

    NovaTonePolyBench file titan8.wav > nt.txt && python3 cmp.py v

2026-10-26: agreed 27/36 -> 32/36; NovaTone-only notes 29 -> 9.
