#pragma once
#include "PolyAnalyzer.h"
#include <functional>

namespace Poly
{
/** Spotify Basic Pitch (stock ICASSP 2022 model, Apache-2.0) in plain C++: CQT front end + 6 conv layers.
    Runs the whole clip offline (2 s windows, 30-frame overlap like basic_pitch.inference) and returns the
    note posteriorgram as a PitchPrior (88 notes from A0, 86.13 frames/s). Empty prior if cancelled. */
PitchPrior basicPitchPrior (const float* mono, int numSamples, double sampleRate,
                            std::function<bool()> shouldExit = {}, PitchPrior* onsetsOut = nullptr);

namespace BasicPitchDetail
{
    constexpr int windowSamples = 43844, frames = 172, hop = 256, overlapFrames = 30;
    /** One 2 s window at 22050 Hz -> note / onset (172 x 88) and contour (172 x 264), row-major. */
    void runWindow (const float* audio22k, float* note, float* onset, float* contour);
}
}
