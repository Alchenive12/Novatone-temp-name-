# Smoothness tests (Nov 5–6, 2026)

Test vocal: `yudy groovy/Samples/yudy groovy_2026-07-25 18-45-48_Insert 1 - Part_1.wav` (36 s, dry, chorus ~32 cents out).
Harness: `tools/abtest` (offline, Linux; `cmake -S tools/abtest -B build-ab && cmake --build build-ab`).

## Shifter: TD-PSOLA vs Signalsmith (+1 st, whole clip)
- User: **PSOLA (current) sounds better and less phasy.** Signalsmith "Natural engine" dropped (branch deleted locally; remote `experiment/natural-shifter` still to delete).

## Correction settings (chromatic, median cents off nearest note)
| render | settings | cents |
|---|---|---|
| dry | – | 32 |
| C | no lookahead, vibrato removed, Humanize 0 | 2.7 |
| D | lookahead, vibrato kept, Humanize 0.5 | 12.5 |
| E | Smooth, vibrato kept, Humanize 1 | 16.7 |
| E2 | E + Center 0.75 | 19.2 |
| E3 | E + Center 0.6 | 20.8 |
- User picked E; E2/E3/E4 "all sound way better". E still a bit too hard.
- Phase check (4–10 kHz comb ripple): dry 7.02 dB, E2 6.81, E4 6.81 → no added phasing; voiced-sibilant HF blend is not the cause.

## Auto Glide (`feature/auto-glide`)
- After a note pitch drag, ramps between touching notes (gap < 60 ms) sized to the jump: ~100 ms for 1 st up to ~190 ms for 7+ st, after the Transition knob scale; never below the 18 % default; hand-dragged ramps kept.
- First version made glides shorter than default (Transition knob scales ramps afterwards) — fixed in f23e9e0.
- CI Linux + Windows pass. **Not yet tested in FL / standalone by ear.**

## PC test attempt (Nov 6, remote)
- Manual Windows build (run 42107742994) put in `E:\Fl studio 2023 - 2024 projects\yudy groovy\_novatone_test\` (Standalone exe + VST3 bundle).
- Standalone runs, but computer use can't be granted to a non-installed exe, so it can't be driven remotely.
- Gemini installed the VST3 to `%LOCALAPPDATA%\Programs\Common\VST3\NovaTone.vst3` (no admin for Common Files). The older NovaTone in `C:\Program Files\Common Files\VST3` is untouched and has the same plugin ID, so FL will likely load the old one.
- To test at home: double-click the exe in `_novatone_test`, or build `feature/auto-glide` locally (VS, x64 Release) so it replaces the Common Files copy.

## Open
- Ear test of Auto Glide in the plugin.
- UI toggle for Auto Glide.
- Delete remote branch `experiment/natural-shifter`.
