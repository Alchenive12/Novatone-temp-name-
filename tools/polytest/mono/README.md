# Single-voice pitch: live MPM vs Basic Pitch

    python3 synth_sing.py female 3 60 79; python3 synth_sing.py male 5 45 64
    python3 evalmono.py female.wav male.wav <real vocal>.wav   # real vocal is scored vs pYIN
    python3 live.py                                             # Basic Pitch accuracy vs look-ahead

2026-10-26: MPM 99.2% within 50 c (median 1.3 c) vs Basic Pitch ~86% (6-8 c) on synthetic singers;
real vocal vs pYIN: MPM 92.7%, Basic Pitch 64.6%. Basic Pitch is no octave referee either
(sides with pYIN 16x, with MPM 14x on 62 disputed frames). Live: needs ~50 ms look-ahead,
22-30 ms CPU per 2 s window call. Verdict: keep MPM for the live single-voice tuner.
