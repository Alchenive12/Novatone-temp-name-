#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <functional>
#include <vector>
#include <cmath>
#include <algorithm>

/**
    Offline polyphonic analysis of a whole audio clip (Melodyne-style "clip" model).

    Pipeline:
      1. STFT (85 ms Hann window, 10.67 ms hop, 4x zero padding) -> interpolated spectral peaks.
      2. Spectral whitening of the peak amplitudes so quiet upper voices can compete with
         a loud bass.
      3. Per frame, iterative multi-F0 estimation by harmonic-amplitude summation
         (Klapuri 2006): pick the most salient F0 over the full range (A1..F#6), subtract
         its partials using the spectral-smoothness principle (so a shared partial, e.g. an
         octave doubling, keeps the energy the next voice needs), repeat until the
         polyphony-estimation criterion says stop.
      4. Note tracking across frames: continuous pitch tracks (tolerant of vocal vibrato),
         gap bridging, minimum duration, and re-attack splitting (re-plucked strings).

    Runs on a background thread; analyse() is self-contained and allocation is fine here.
*/
namespace Poly
{
    struct Note
    {
        int   id          = 0;
        int   startFrame  = 0;      ///< first analysis frame (inclusive)
        int   endFrame    = 0;      ///< last analysis frame (exclusive)
        float midi        = 0.0f;   ///< representative (median) pitch
        float level       = 0.0f;   ///< mean linear amplitude of the fundamental-weighted partials
        std::vector<float> pitchMidi; ///< per-frame continuous pitch, size endFrame - startFrame
        std::vector<float> amp;       ///< per-frame partial amplitude, same size
    };

    struct Analysis
    {
        double sampleRate = 48000.0;
        int    hopSize    = 512;     ///< samples between frames (10.67 ms); frame i is centred on sample i * hopSize
        int    numFrames  = 0;
        std::vector<Note> notes;

        double frameToSeconds(double f) const noexcept { return f * hopSize / sampleRate; }
    };

    /** Optional learned pitch prior (e.g. a neural note posteriorgram): per-frame confidence
        for MIDI 21..108 (88 bins). The analyser keeps its own exact pitch/timing measurement and
        uses this only to decide which candidates are real notes. */
    struct PitchPrior
    {
        int   frames = 0;
        float fps = 86.13f;
        std::vector<float> note;   ///< frames x 88
        float at (double seconds, int midi) const noexcept
        {
            if (frames <= 0 || midi < 21 || midi > 108) return 1.0f;
            const int f = (int) std::lround (seconds * fps);
            float m = 0.0f;
            for (int k = f - 1; k <= f + 1; ++k)        // +-1 frame (11.6 ms) of slack
                if (k >= 0 && k < frames) m = std::max (m, note[(size_t) k * 88 + (size_t) (midi - 21)]);
            return m;
        }
    };

    struct AnalyzerSettings
    {
        float minMidi      = 33.0f;   ///< A1
        float maxMidi      = 100.0f;  ///< E7 (piano / synth top lines)
        int   maxPolyphony = 10;
        float sensitivity  = 0.5f;    ///< 0 = only clear notes, 1 = catch quiet voices
        const PitchPrior* prior = nullptr; ///< optional; not owned
        float priorFloor   = 1.0f;    ///< frame-level weight of a candidate the prior gives ~0 (1 = off)
        float priorNoteGate = 0.1f;   ///< note-level: octave/12th-related notes below this mean prior are ghosts
    };

    /** Analyses mono audio. `shouldCancel` may be polled from the analysing thread. */
    Analysis analyse (const float* mono, int numSamples, double sampleRate,
                      const AnalyzerSettings& settings = {},
                      const std::function<bool()>& shouldCancel = {},
                      const std::function<void (float)>& progress = {});
}
