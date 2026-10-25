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
