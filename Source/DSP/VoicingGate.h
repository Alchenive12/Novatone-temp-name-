#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

/**
    Voicing gate with hysteresis (blueprint step 3): decides, per hop, whether the input is
    "voiced" enough to apply pitch correction, using separate open/drop clarity thresholds
    plus a minimum bridge duration to avoid chattering on brief clarity dips (e.g. during
    consonants or vibrato nulls).

    The raw (binary) state is smoothed by a 5 ms one-pole low-pass filter into a float gate
    g_sm[n], intended to drive a wet/dry crossfader without producing clicks at the
    voiced/unvoiced transition.

    prepare() precomputes coefficients and resets state; process() does no allocation and
    is safe on the audio thread.
*/
class VoicingGate
{
public:
    VoicingGate() = default;

    /** Precomputes T_bridge and the anti-click smoothing coefficient, and resets state.
        Not real-time safe.
        @param newSampleRate  audio sample rate, in Hz.
        @param newHopSize     H: number of audio samples between successive clarity values
                               (the control-rate step one process() call advances by). */
    void prepare (double newSampleRate, int newHopSize);

    /** Resets the state machine to UNVOICED and clears the smoothed gate. Allocation-free;
        safe on the audio thread. */
    void reset() noexcept;

    /** Feeds one hop's clarity value through the hysteresis state machine and the
        anti-click smoother.
        @param clarity  the pitch detector's clarity value for this hop (e.g. MPMResult::clarity).
        @returns g_sm[n] in [0, 1], for the wet/dry crossfader. */
    float process (float clarity) noexcept;

    /** The raw (unsmoothed) hysteresis state, for UI/diagnostics. */
    bool isVoiced() const noexcept { return state == State::voiced; }

private:
    enum class State { unvoiced, voiced };

    double sampleRate = 44100.0;
    int    hopSize    = 0;

    static constexpr float kClarityOpen       = 0.75f;  // C_open
    static constexpr float kClarityDrop       = 0.55f;  // C_drop
    static constexpr float kBridgeSeconds     = 0.02f;  // t_bridge
    static constexpr float kGateSmoothSeconds = 0.005f; // 5 ms anti-click low-pass

    int   bridgeFrames = 1;    // T_bridge, in hops: ceil(t_bridge * fs / H)
    float gateAlpha     = 1.0f; // one-pole coefficient for the anti-click smoother

    State state       = State::unvoiced;
    int   dropCounter = 0;    // d
    float gSmoothed   = 0.0f; // g_sm[n-1]

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoicingGate)
};

