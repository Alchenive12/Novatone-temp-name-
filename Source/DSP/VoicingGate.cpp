#include "VoicingGate.h"

//==============================================================================
void VoicingGate::prepare (double newSampleRate, int newHopSize)
{
    jassert (newSampleRate > 0.0);
    jassert (newHopSize > 0);

    sampleRate = newSampleRate;
    hopSize    = newHopSize;

    // T_bridge = ceil(t_bridge * fs / H)                                     (blueprint step 3)
    const double framesPerSecond = sampleRate / static_cast<double> (hopSize);
    bridgeFrames = static_cast<int> (std::ceil (kBridgeSeconds * framesPerSecond));
    bridgeFrames = juce::jmax (1, bridgeFrames);

    // Anti-click smoother: alpha = 1 - exp(-H / (fs * tau)), tau = 5 ms fixed.
    gateAlpha = 1.0f - std::exp (static_cast<float> (-hopSize)
                                  / (static_cast<float> (sampleRate) * kGateSmoothSeconds));

    reset();
}

void VoicingGate::reset() noexcept
{
    state       = State::unvoiced;
    dropCounter = 0;
    gSmoothed   = 0.0f;
}

//==============================================================================
float VoicingGate::process (float clarity) noexcept
{
    // ---- Hysteresis state machine (blueprint step 3) ----
    if (clarity < kClarityDrop)
        ++dropCounter; // d = d + 1
    else
        dropCounter = 0; // d = 0

    if (state == State::unvoiced)
    {
        if (clarity >= kClarityOpen)
            state = State::voiced; // UNVOICED -> VOICED: immediate
    }
    else // state == State::voiced
    {
        if (dropCounter >= bridgeFrames)
            state = State::unvoiced; // VOICED -> UNVOICED: only once the bridge holds
    }

    // ---- Anti-click smoothing of the binary state ----
    const float target = (state == State::voiced) ? 1.0f : 0.0f;
    gSmoothed = gateAlpha * target + (1.0f - gateAlpha) * gSmoothed; // g_sm[n]
    return gSmoothed;
}

