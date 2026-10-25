# NovaTone (working title)

Melodyne-style vocal / instrument pitch editor — JUCE 8 VST3 + Standalone.

## Build
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
JUCE 8.0.4 and Signalsmith Stretch 1.4.0 are fetched automatically.
Use `-DJUCE_DIR=...` / `-DSIGNALSMITH_DIR=...` for local checkouts.

## CI
- Branch pushes: Linux + Windows compile check, no uploads.
- `main` / manual run: adds macOS.
- Manual "Run workflow": zips uploaded as artifacts, kept 1 day.
- Push a `v*` tag: all platforms, zips attached to a GitHub Release.

## Poly mode (Melodyne-style clip editing)
1. Switch to **Poly**, press **Rec**, then play the part in your DAW. Press **Stop** (the Rec button).
   Or File > *Load audio into poly clip*.
2. The clip is analysed in the background; its notes (chords included) appear on the piano roll.
3. Drag notes to new pitches, change gain/formant in the inspector, slice notes. Each edit re-renders
   the clip in the background (only the edited notes are touched; everything else stays bit-identical).
4. Play your DAW: inside the clip's range NovaTone plays the edited clip in place of the track.
   The play button previews it without the DAW. File > *Export edited clip (WAV)* bounces it.

The clip audio is saved as FLAC in the user's app-data folder (`Alchenive/NovaTone/Clips`); the
analysis and edits are saved in the project with the plugin state.

Engine: `Source/Poly/` (analyser, renderer, session). Benchmark: `tools/PolyBench.cpp`
(`-DNOVATONE_BUILD_TOOLS=ON`, then `NovaTonePolyBench [live|clip|render|session|listen]`).

## Harmony tab (sidechain chords)
Switch to **Harmony** in the header. Route your beat / instrumental into NovaTone's **sidechain**
input; the tab detects its chords live and generates up to four harmony voices from the lead vocal
(chord tone above / below, 2 tones above / below, octave, chord root, unison double), each with
level, pan and formant. Without a sidechain, harmonies follow the Tune tab's key & scale.
Turn **Lead** all the way down to print the harmonies on their own track.

## UI previews without a DAW
```
cmake -S . -B build-snap -DNOVATONE_BUILD_SNAPSHOT=ON
cmake --build build-snap --target NovaToneSnapshot
NovaToneSnapshot <outDir> [width height scale theme selectNote fileName]
```
Renders the editor to a PNG (on Linux run it under `xvfb-run`). Dev-only, never built in CI.
Screenshots of the current design are in `docs/ui/`.

The UI font is Inter (SIL Open Font License, see `Resources/Fonts/Inter-LICENSE.txt`).
