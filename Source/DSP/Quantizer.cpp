#include "Quantizer.h"
#include <limits>

//==============================================================================
void Quantizer::prepare (double newSampleRate, int newHopSize)
{
    jassert (newSampleRate > 0.0);
    jassert (newHopSize > 0);

    sampleRate = newSampleRate;
    hopSize    = newHopSize;

    setHoldTime (holdTimeSeconds); // (re)computes holdFrames now that fs/H are known
    reset();
}

void Quantizer::setScale (uint16_t newPitchClassMask) noexcept
{
    jassert (newPitchClassMask != 0 && newPitchClassMask < (1u << 12)); // 1 <= K <= 12
    pitchClassMask = newPitchClassMask;
}

void Quantizer::setHysteresisMargin (float newMarginCents) noexcept
{
    marginCents     = newMarginCents;
    marginSemitones = marginCents / 100.0f; // delta_m = delta / 100          (blueprint 1)
}

void Quantizer::setHoldTime (float newHoldTimeSeconds) noexcept
{
    holdTimeSeconds = juce::jmax (0.0f, newHoldTimeSeconds);

    if (sampleRate <= 0.0 || hopSize <= 0)
        return; // prepare() hasn't run yet; keep the current value

    // T_hold = ceil(t_hold * fs / H)                                        (blueprint 3.1)
    const double framesPerSecond = sampleRate / static_cast<double> (hopSize);
    holdFrames = static_cast<int> (std::ceil (holdTimeSeconds * framesPerSecond));
    holdFrames = juce::jmax (1, holdFrames);
}

void Quantizer::reset() noexcept
{
    initialized       = false;
    activeNote        = 69;
    previousCandidate = 69;
    holdCounter       = 0;
}

//==============================================================================
float Quantizer::hzToMidi (float hz) noexcept
{
    return 69.0f + 12.0f * std::log2 (hz / 440.0f);                          // blueprint 1
}

float Quantizer::midiToHz (float midiNote) noexcept
{
    return 440.0f * std::exp2 ((midiNote - 69.0f) / 12.0f);                  // blueprint 1
}

bool Quantizer::isNoteAllowed (int note) const noexcept
{
    const int pitchClass = ((note % 12) + 12) % 12; // normalise to [0, 11]
    return (pitchClassMask & static_cast<uint16_t> (1u << pitchClass)) != 0;
}

int Quantizer::findNearestAllowedNote (float continuousMidi) const noexcept
{
    // Blueprint 2 notes the answer usually lies within [floor(m)-1, ceil(m)+1]. We instead
    // scan a full octave-plus-margin window (13 candidates) so the search stays exact even
    // for a sparse scale (e.g. K = 1) - still a fixed, tiny loop independent of any buffer
    // or block size, so the real-time-safety cost is unchanged.
    const int base = static_cast<int> (std::floor (continuousMidi)) - 6;

    int   bestNote = base;
    float bestDist = std::numeric_limits<float>::max();

    for (int k = base; k <= base + 12; ++k)
    {
        if (! isNoteAllowed (k))
            continue;

        const float dist = std::abs (continuousMidi - static_cast<float> (k));

        // Strict '<' keeps the first (lowest) note reached at the minimum distance - the
        // "choose the lower value" tie-break rule (blueprint 2).
        if (dist < bestDist)
        {
            bestDist = dist;
            bestNote = k;
        }
    }

    return bestNote;
}

int Quantizer::findLowerNeighbor (int note) const noexcept
{
    for (int k = note - 1; k >= note - 12; --k)
        if (isNoteAllowed (k))
            return k;

    jassertfalse; // unreachable if the scale has at least one pitch class
    return note - 1;
}

int Quantizer::findUpperNeighbor (int note) const noexcept
{
    for (int k = note + 1; k <= note + 12; ++k)
        if (isNoteAllowed (k))
            return k;

    jassertfalse; // unreachable if the scale has at least one pitch class
    return note + 1;
}

//==============================================================================
QuantizerResult Quantizer::process (float inputHz) noexcept
{
    // Snapshot before this hop's logic runs, so noteChanged reflects only what happens
    // below - not any difference from an arbitrarily-old hop.
    const int noteBeforeThisHop = activeNote;

    if (inputHz <= 0.0f)
    {
        // Unvoiced/invalid: hold, don't perturb state. activeNote is untouched by
        // construction, so noteChanged is unconditionally false here.
        return { midiToHz (static_cast<float> (activeNote)), false };
    }

    const float mIn = hzToMidi (inputHz);

    // Step 1: nearest allowed note                                          (blueprint 3.4)
    const int mCand = findNearestAllowedNote (mIn);

    if (! initialized)
    {
        // 3.5 Initialization: m_active = m_cand[0], h[0] = 0
        activeNote        = mCand;
        previousCandidate = mCand;
        holdCounter       = 0;
        initialized       = true;
        return { midiToHz (static_cast<float> (activeNote)), activeNote != noteBeforeThisHop };
    }

    // Step 2: forced boundary crossing (primary hysteresis)                 (blueprint 3.4)
    const int mLo = findLowerNeighbor (activeNote);
    const int mHi = findUpperNeighbor (activeNote);

    const float betaLo = 0.5f * static_cast<float> (activeNote + mLo) - marginSemitones;
    const float betaHi = 0.5f * static_cast<float> (activeNote + mHi) + marginSemitones;

    bool forced = false;

    // Jump straight to the nearest allowed note: stepping one scale degree per
    // hop turned every leap into an audible run through the notes in between.
    if (mIn < betaLo || mIn > betaHi)
    {
        activeNote        = mCand;
        previousCandidate = mCand;
        holdCounter       = 0;
        forced            = true;
    }

    if (! forced)
    {
        // Step 3: hold branch (secondary hysteresis)
        if (mCand == activeNote)
            holdCounter = 0;
        else if (mCand != previousCandidate)
            holdCounter = 1; // new candidate trajectory begins
        else
            ++holdCounter;

        previousCandidate = mCand;

        // Step 4: hold-time transition check
        if (holdCounter >= holdFrames)
        {
            activeNote  = mCand;
            holdCounter = 0;
        }
    }

    // Step 5: emit - noteChanged is true iff activeNote actually moved this hop, whether
    // via the forced branch or the hold-time branch above (blueprint: humanize sustain
    // timer reset condition "m_target[n] != m_target[n-1]").
    return { midiToHz (static_cast<float> (activeNote)), activeNote != noteBeforeThisHop };
}

