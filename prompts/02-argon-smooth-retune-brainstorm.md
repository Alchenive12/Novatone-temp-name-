<!-- File: prompts/02-argon-smooth-retune-brainstorm.md -->
# Argon: brainstorm — make NovaTone's retuning as smooth and natural as possible (co-worker method)

Run in Antigravity with model **Argon**. One job: read the shifter / tuning code, research, and write ONE
brainstorm report. Do NOT change any code. Claude reads the report and decides what to build.

```text
ROLE: senior DSP engineer for vocal pitch correction (Melodyne / Newtone / Auto-Tune level). Brainstorm, do not edit code.

GOAL
The user wants retuned vocals in NovaTone to sound natural, not robotic: e.g. moving a sung note +1 semitone
should sound like the singer sang it, with no phasiness, buzz, warble, clicks or "chipmunk" colour, and without
the user doing lots of manual work. Test vocal: FL Studio project "yudy groovy" (E: drive, FL Studio 2023-2024
projects folder) - its chorus is very out of tune. Already decided (do not re-argue): keep TD-PSOLA as the main
shifter (an A/B showed it beats Signalsmith Stretch on this vocal); an "auto glide" tool is wanted that fixes the
transition into and out of a moved note.

HARD RULES
- Repo: only the git commands in STEP 1. Never edit, commit or push. Do not build.
- Write only in  %USERPROFILE%\gemini\novatone-smooth-02\  (create it).
- Google AI Pro plan only: no API keys, no paid services, no installs. Do not click antivirus / security popups.
- Budget: Argon quota is limited. Check Antigravity Settings > Models & Usage at the start and the end and write
  both numbers in the report. If it drops below 15% left, stop, write what you have, say so.
- Finish with status.txt in that folder: "done: <N> ideas" or "failed: <step> <one sentence>".

STEP 1  cd $env:USERPROFILE\source\repos\NovaTone
        git status --short ; git fetch origin ; git merge --ff-only origin/main ; git log --oneline -1
        If merge fails because of local changes, do NOT stash or reset: read the files as they are and note it.

STEP 2  Read (skip Source/Poly/BasicPitchWeights.h):
        1. Source/DSP/PsolaShifter.h                       (the shifter: pitch marks, grains, ratio smoothing)
        2. Source/DSP/SmoothPlanner.h, TuneMacro.h, VibratoTracker.h, SmoothingFilter.h, VoicingGate.*
        3. Source/DSP/TuneEngine.h  (how target pitch / correction curves are made; it is long - skim structure,
           read the parts that build the pitch-shift curve fed to the shifter)
        4. Source/Poly/PolyRenderer.cpp  (clip re-render, formant envelope, 15 ms crossfades)
        5. Source/PluginProcessor.cpp  (how mono clip edits reach the shifter)

STEP 3  Research (web) how the best tools get natural results, then map it onto THIS code. Cover at least:
        A. Pitch marks: are NovaTone's marks (cross-correlation, one period apart) aligned to glottal closure
           instants / waveform peaks? Effect of mark jitter on phasiness; epoch detection methods (e.g. SEDREAMS,
           ZFF, DYPSA) and cheap real-time alternatives.
        B. Grains & windows: window shape/length (2 periods Hann vs asymmetric), grain selection when shifting UP
           (repeat) vs DOWN (skip), keeping grain phase continuous, energy normalisation.
        C. Unvoiced / breath / consonants: currently unvoiced audio uses a fixed 200 Hz grain period - does that
           cause buzz on "s", "sh", breaths? Options: pass unvoiced through unshifted with crossfade, voicing-aware mix.
        D. Formants: PSOLA keeps formants roughly; at +/-2..5 semitones what goes wrong and how to fix
           (envelope correction, formant knob, LPC/cepstral pre-warp).
        E. Pitch curve shaping (the "musical" part, likely the biggest win): keep the singer's vibrato and micro-
           drift (shift the note's centre, not flatten it), transition/glide shapes into and out of moved notes
           (what Melodyne's "Pitch Transition" and Newtone's transition do), onset scoops, note tails, avoiding
           steps when one note is moved and its neighbours are not. Design ideas for the auto-glide tool.
        F. Ratio smoothing: current 2.5 ms / 2.8 ms exponential smoothing on the ratio - is it right? Should the
           ratio change only at grain boundaries?
        G. Offline advantage: Mono clip mode is offline (Independent / Newtone-like). What can an offline renderer do
           that real-time can't (look-ahead mark placement, two-pass analysis, global optimisation of marks,
           choosing per-note between PSOLA and another method, high-quality resampling)?
        H. Anything else you find that clearly matters (e.g. DC/low-freq handling, polarity, latency alignment).

STEP 4  Write report.md in the output folder, exactly this format:
        # NovaTone smooth retune brainstorm 02
        Commit: <hash>   Argon usage: start <x%> / end <y%>   Files read: <list>
        ## How NovaTone currently retunes (max 12 lines, your own words, with file:line refs)
        ## Likely causes of unnatural sound in the current code (ranked)
        - <cause> - Where: <file:line> - Why it hurts - Evidence: <1-6 code lines quoted exactly>
        ## Ideas
        ### I1 <title>   [impact: high|med|low] [effort: S|M|L] [risk: low|med|high] [area: A..H]
        - What: <2-4 sentences>
        - Where in NovaTone: <files / functions>
        - How to test it: <what to listen for / measure on the yudy groovy chorus, +1 and +3 semitones>
        - Sources: <links>
        (repeat; best impact/effort first; max 20 ideas; concrete, not generic)
        ## Auto-glide design sketch (one page max)
        ## Suggested order (top 5 to build first, one line each)

STEP 5  Write status.txt.
```
