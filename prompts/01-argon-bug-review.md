<!-- File: prompts/01-argon-bug-review.md -->
# Argon: bug review of NovaTone (co-worker method)

Run in Antigravity with model **Argon**. One job: read the code, write ONE review file. Do NOT fix anything.

```text
ROLE: senior C++/JUCE audio-plugin reviewer. Find real bugs; do not edit code.

HARD RULES
- Repo: only the git commands in STEP 1. Never edit, commit or push. Do not build.
- Write only in  %USERPROFILE%\gemini\novatone-review-01\  (create it).
- Google AI Pro plan only: no API keys, no paid services. Do not click antivirus / security popups.
- Finish with status.txt in that folder: "done: <N> findings" or "failed: <step> <one sentence>".

STEP 1  cd $env:USERPROFILE\source\repos\NovaTone
        git status --short ; git fetch origin ; git merge --ff-only origin/remote/transport-grid ; git log --oneline -1
        (the log line must start with the commit that added this file or later)

STEP 2  Read, in this order (skip Source/Poly/BasicPitchWeights.h - generated weights):
        1. Source/PluginProcessor.cpp/.h, Source/GUI/PitchTraceQueue.h        (audio thread <-> GUI)
        2. Source/Poly/PolyClipSession.cpp/.h, PolyRenderer.cpp, PolyAnalyzer.cpp
        3. Source/GUI/PitchTraceComponent.cpp/.h, TuneTab.cpp/.h
        4. Source/Harmony/*.h, Source/Choir/ChoirSession.h, Source/GUI/HarmonyTab.cpp, ChoirTab.cpp
        5. Source/DSP/TuneEngine.h and the rest of Source/DSP
        If you run low on budget, stop after the group you are in and say so in the report.

STEP 3  Look for, most important first:
        A. Crashes: out-of-bounds indexing (negative or past-end), null/dangling pointers, use-after-free,
           integer overflow in sample/hop math, division by zero, uncaught exceptions.
        B. Threading: data races between audio thread / message thread / worker threads; locks or
           re-entrant locking (std::mutex locked twice = abort on MSVC); callbacks fired while a lock is held;
           audio-thread memory allocation, locks, file I/O or logging in processBlock.
        C. State: things not saved/restored with the project, sample-rate or buffer-size changes not handled,
           wrong behaviour after prepareToPlay is called again.
        D. Logic bugs a user would notice (wrong note, wrong position, button doing the wrong thing).
        Do NOT report style, naming, formatting or "could be refactored".

STEP 4  Write report.md in the output folder, exactly this format:
        # NovaTone bug review 01
        Commit: <hash>   Files read: <list>   Files not reached: <list or none>
        ## Findings
        ### F1 [critical|high|medium|low] <one-line title>
        - Where: <path>:<line> (function)
        - What happens: <2-4 sentences>
        - How to trigger: <concrete user/host steps or inputs>
        - Evidence: <the 1-10 relevant code lines, quoted exactly>
        - Suggested fix: <1-3 sentences>
        - Confidence: <high|medium|low>
        (repeat per finding; most severe first; max 25 findings; only real, evidenced bugs)
        ## Checked and fine
        <short list of risky areas you checked and found correct>

STEP 5  Write status.txt.
```
