#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

/**
    Per-sample retune curve: a one-pole glide toward the target correction,
    followed by a slew-rate limiter. The one-pole sets the character (Flex);
    the slew limit caps how fast the transposition may move, so a note change
    becomes a short glide the phase vocoder can follow instead of a step it
    tears on.
*/
class SmoothingFilter
{
public:
    SmoothingFilter() = default;

    void prepare(double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
        setTimeConstant(timeSeconds);
        setMaxSlewRate(slewSemitonesPerSecond);
        reset();
    }

    void reset() noexcept
    {
        current = 0.0f;
        target = 0.0f;
        rampLeft = 0;
    }

    void setTimeConstant(double seconds) noexcept
    {
        timeSeconds = juce::jlimit(0.002, 0.500, seconds);
        coeff = 1.0f - std::exp(-1.0f / static_cast<float> (timeSeconds * sampleRate));
    }

    /** Maximum rate of change of the correction, in semitones per second. */
    void setMaxSlewRate(float semitonesPerSecond) noexcept
    {
        slewSemitonesPerSecond = juce::jmax(1.0f, semitonesPerSecond);
        maxStepPerSample = slewSemitonesPerSecond / static_cast<float> (sampleRate);
    }

    void setTargetSemitones(float semitones) noexcept
    {
        target = semitones;
        rampLeft = 0;
    }

    /** Glides the target itself to `semitones` over rampSamples, so a control
        value that arrives once per analysis hop becomes a straight line
        instead of a staircase. */
    void rampTargetSemitones(float semitones, int rampSamples) noexcept
    {
        if (rampSamples <= 1)
        {
            setTargetSemitones(semitones);
            return;
        }
        rampEnd = semitones;
        rampStep = (semitones - target) / static_cast<float> (rampSamples);
        rampLeft = rampSamples;
    }

    float getTargetSemitones() const noexcept { return rampLeft > 0 ? rampEnd : target; }
    float getCurrentSemitones() const noexcept { return current; }

    float processSample() noexcept
    {
        if (rampLeft > 0)
            target = --rampLeft == 0 ? rampEnd : target + rampStep;
        const float step = coeff * (target - current);
        current += juce::jlimit(-maxStepPerSample, maxStepPerSample, step);
        return current;
    }

private:
    double sampleRate = 44100.0;
    double timeSeconds = 0.045;
    float  slewSemitonesPerSecond = 150.0f;

    float coeff = 0.0f;
    float maxStepPerSample = 0.0f;
    float current = 0.0f;
    float target = 0.0f;
    float rampEnd = 0.0f, rampStep = 0.0f;
    int   rampLeft = 0;
};

