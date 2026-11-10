# Handoff: testing NovaTone on the yudy groovy vocal

For any Claude session (e.g. Fable) picking up NovaTone retune work. Written 2026-11-10 from the 2026-11-09 session.
Results of that session: `docs/smooth-retune-2026-11-09.md`.

## 1. The test vocal
- File on the owner's PC (Windows, desktop `desktop-pof7hdc`):
  `E:\Fl studio 2023 - 2024 projects\yudy groovy\Samples\yudy groovy_2026-07-25 18-45-48_Insert 1 - Part_1.wav`
- 36 s, 44.1 kHz, stereo with identical channels (use mono), dry lead vocal, female range (median MIDI 62.4,
  ~290 Hz), 78 % voiced, notes average ~28 cents off. This is the owner's favourite test take ("very good vocal,
  chorus very out of tune").
- The project folder also has many other takes in `Audio\` (FL recordings) - not needed.
- Write test outputs to `E:\Fl studio 2023 - 2024 projects\yudy groovy\_novatone_test\` (exists; Melodyne reference
  renders from Nov 9 are there: `melodyne_tuned/plus1st/plus3st.wav`, 32-bit float, 0.43 s offset vs the dry file).

## 2. Getting it into the cloud sandbox
The PC must be linked (Claude desktop app open). The `mcp__remote-devices__device_*` tools only see connected folders.
1. `device_request_folder_access` with exactly `E:\Fl studio 2023 - 2024 projects\yudy groovy` (reason: get the
   test vocal). The owner approved this folder on Nov 9. Broader or system folders (e.g. `C:\Program Files\...`) were
   refused by the auto-mode classifier - ask only for this folder.
2. `device_stage_files` with the path above -> appears at
   `/mnt/user-data/uploads/yudy groovy/Samples/yudy groovy_2026-07-25 18-45-48_Insert 1 - Part_1.wav`
   (wait ~2 s before reading).
3. Results go to the owner with `SendUserFile` (24-bit WAVs, ~3 MB each), or back to the PC with `device_commit_files`
   into `_novatone_test\`.

## 3. Most efficient test: the offline bench (no DAW, no PC GUI)
`tools/retune-bench/` runs NovaTone's real `TuneEngine` + `PsolaShifter` on the file. Old vs new commit side by side,
~2 min per round after a one-time ~3 min JUCE compile. This was far faster and more reliable than driving FL Studio.

```bash
pip install --break-system-packages -q librosa soundfile
R=/home/claude/novatone-temp-name-; B=/home/claude/bench; mkdir -p $B && cd $B
cp $R/tools/retune-bench/{juceconf.h,harness2.cpp} .
# once: JUCE 8.0.4 modules + stamp (see tools/retune-bench/README.md for the exact commands)
# build A (baseline commit in a worktree) and B (your branch):
git -C $R worktree add -q $B/A <baseline-commit>
for t in A B; do S=$R/Source; [ $t = A ] && S=$B/A/Source
  g++ -std=c++17 -O2 -include juceconf.h -I $S -I $S/DSP -I ~/JUCE/modules harness2.cpp \
    $S/DSP/MPMDetector.cpp $S/DSP/Quantizer.cpp $S/DSP/SmoothingFilter.cpp $S/DSP/VoicingGate.cpp \
    $S/DSP/LookaheadBuffer.cpp juce_*.o stamp.o -o h2_$t -lpthread -ldl & done; wait
V="/mnt/user-data/uploads/yudy groovy/Samples/yudy groovy_2026-07-25 18-45-48_Insert 1 - Part_1.wav"
python3 -I $R/tools/retune-bench/prep_vocal.py "$V" prep 0 1 3      # mono + per-block note targets
python3 -I $R/tools/retune-bench/render_ab.py prep out ./h2_A ./h2_B 0 1 3
```
`render_ab.py` writes `out/0_dry.wav`, `out/shift+K_A.wav`, `out/shift+K_B.wav` and prints per version:
median shift (st), mean cents from the note centre, RMS vs dry. Reference (Nov 9, commit 7fd112b = B):
`+3 A/B: shift +2.60, 22 / 24 c off, rms 0.044 (dry 0.056)` - i.e. ~2 dB level loss at +3 (open issue).

What the bench does / doesn't model:
- Settings: Center 100 %, Variation 100 %, Humanize 50 %, chromatic. Each 512-sample block gets a live note
  override = pYIN note (median-smoothed, rounded) + shift, looked up ~1024 samples behind the block start (the engine's
  analysis lag). That emulates "every note moved in the piano roll"; it is not the exact Mono-clip GUI path
  (PitchTraceComponent curve -> processor overrides). Use the plugin for that.
- Output is latency-compensated and sample-aligned with the dry file.
- **Baseline before 7fd112b outputs silence on this voice** (PSOLA start bug above ~160 Hz). For an old baseline,
  patch `placeGrains`' first-grain branch the same way 7fd112b does, or the A file is empty.
- For quick DSP checks without a real voice: `mk.py` makes a synthetic sung signal (drift, vibrato, breath, "s"),
  `harness.cpp` + `ev.py`/`ev2.py` measure expression kept, correlation with the singer, and breath vs dry.

## 4. Testing in the real plugin (only when the owner can listen on the PC)
- Build + install: Antigravity, new conversation, model **Gemini 3.8 Flash** (cheap; Argon only for research).
  The brief: fetch + checkout the branch (refuse if the working tree is dirty), `cmake --build build --config Release
  --target NovaTone_VST3`, rename the installed `C:\Users\didi\AppData\Local\Programs\Common\VST3\NovaTone.vst3`
  to `NovaTone.vst3.prev-<hash>` (never delete), copy the new bundle, write `status.txt` in `%USERPROFILE%\gemini\<job>\`.
  Took ~4 min on Nov 9.
- Avoid driving FL Studio / Melodyne through computer use while the owner is away: each "open app" starts a NEW FL
  instance (unsaved work is lost), a McAfee popup masks the middle of the screen (hides dialogs), and FL's
  PAT/SONG switch was covered. The owner plans to remove McAfee / ReasonLabs on Thursday (2026-11-12).
- Melodyne reference: Melodyne 5 Studio is installed (plugin only). In FL it runs in Transfer mode (no ARA): load on
  the clip's mixer insert, arm Transfer, play in SONG mode, Algorithm -> **Melodic** (Universal sounded phasey),
  Correct Pitch 100 / 0, move notes, File > Export > Wave. Melodyne snaps to the detected key (D minor here), NovaTone
  to the nearest semitone - a few notes differ; compare sound quality, not note choice.

## 5. Rules that apply (from the owner's away-from-home protocol)
- All plugin work in the GitHub org **Alchenive12** (repo `Alchenive12/Novatone-temp-name-`); docs/tools/prompts
  commits don't build; branch pushes build Linux + Windows.
- Gemini/Argon never edit source, commit or push; they write only in their own `%USERPROFILE%\gemini\...` folder.
- Argon has a small weekly quota - check Antigravity Settings -> Models & Usage; verify its claims against the code
  before building (on Nov 9 its #1 idea was wrong).
