#pragma once
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

/**
    Pitch-synchronous overlap-add (TD-PSOLA) shifter for NovaTone.

    Analysis: pitch marks one period apart are placed on the mono sum. Each new
    mark is the lag, near the period hint, whose preceding one-period segment
    best matches (normalised cross-correlation) the segment before the last
    mark, refined to sub-sample accuracy via 3-point parabolic interpolation.

    Synthesis: output grains are placed (period / smoothedRatio) apart.
    Includes per-sample exponential smoothing on `smoothedRatio`, optional
    Humanize ballistics (6.5 ms silky ratio glide), and continuous overlap-add
    window/gain normalization.
*/
class PsolaShifter
{
public:
    void prepare(const juce::dsp::ProcessSpec& spec, int analysisLagSamples)
    {
        sampleRate = spec.sampleRate;
        numChannels = static_cast<int> (spec.numChannels);

        maxPeriod = static_cast<int> (std::ceil(sampleRate / minHz));
        minPeriod = static_cast<int> (std::floor(sampleRate / maxHz));
        unvoicedPeriod = static_cast<int> (sampleRate / unvoicedHz);

        synthesisLag = juce::jmax(maxPeriod * 3 / 2 + 4, analysisLagSamples);
        latency = synthesisLag + maxPeriod + 4;

        ringSize = juce::nextPowerOfTwo(latency + 4 * maxPeriod + 32);
        ringMask = ringSize - 1;
        inputRing.setSize(numChannels, ringSize);
        outputRing.setSize(numChannels, ringSize);
        monoRing.assign(static_cast<size_t> (ringSize), 0.0f);

        fastRatioSmoothAlpha  = 1.0 - std::exp(-1.0 / (sampleRate * 0.0025));
        humanRatioSmoothAlpha = 1.0 - std::exp(-1.0 / (sampleRate * 0.0028));
        ratioSmoothAlpha      = humanizeMode ? humanRatioSmoothAlpha : fastRatioSmoothAlpha;

        reset();
    }

    void reset() noexcept
    {
        inputRing.clear();
        outputRing.clear();
        std::fill(monoRing.begin(), monoRing.end(), 0.0f);
        written = 0;
        markCount = 0;
        lastGrain = -1;
        synthPos = 0.0;
        targetRatio = 1.0;
        smoothedRatio = 1.0;
        passthroughMix = polyPassthrough ? 1.0f : 0.0f;
    }

    void setHumanizeMode(bool enabled) noexcept
    {
        humanizeMode = enabled;
        ratioSmoothAlpha = humanizeMode ? humanRatioSmoothAlpha : fastRatioSmoothAlpha;
    }

    bool isHumanizeMode() const noexcept { return humanizeMode; }

    void setPolyphonicPassthrough(bool enabled) noexcept
    {
        polyPassthrough = enabled;
    }

    void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const int chans = juce::jmin(numChannels, static_cast<int> (block.getNumChannels()));
        const int numSamples = static_cast<int> (block.getNumSamples());
        if (chans == 0 || ringSize == 0)
            return;

        const float channelScale = 1.0f / static_cast<float> (chans);

        for (int i = 0; i < numSamples; ++i)
        {
            smoothedRatio += ratioSmoothAlpha * (targetRatio - smoothedRatio);

            const int w = static_cast<int> (written & ringMask);
            float mono = 0.0f;
            for (int ch = 0; ch < chans; ++ch)
            {
                const float x = block.getSample(ch, i);
                inputRing.setSample(ch, w, x);
                mono += x;
            }
            monoRing[static_cast<size_t> (w)] = mono * channelScale;
            ++written;

            placeMarks();
            placeGrains();

            const float targetPass = polyPassthrough ? 1.0f : 0.0f;
            passthroughMix += 0.004f * (targetPass - passthroughMix);

            const int r = static_cast<int> ((written - 1 - latency) & ringMask);
            for (int ch = 0; ch < chans; ++ch)
            {
                const float psolaOut = outputRing.getSample(ch, r);
                const float exactDry = inputRing.getSample(ch, r);
                block.setSample(ch, i, psolaOut + passthroughMix * (exactDry - psolaOut));
                outputRing.setSample(ch, r, 0.0f);
            }
        }
    }

    int getLatencySamples() const noexcept { return latency; }
    int getAnalysisLatencySamples() const noexcept { return synthesisLag; }

    void setPitchSemitones(float semitones) noexcept
    {
        targetRatio = std::exp2(static_cast<double> (juce::jlimit(-12.0f, 12.0f, semitones)) / 12.0);
    }

    void setPeriodHint(float hz) noexcept { hintHz = hz; }

private:
    double mark(int64_t k) const noexcept { return marks[static_cast<size_t> (k & (markRingSize - 1))]; }
    float mono(int64_t pos) const noexcept { return monoRing[static_cast<size_t> (pos & ringMask)]; }

    void placeMarks() noexcept
    {
        const double newest = static_cast<double>(written - 1);
        if (markCount == 0)
        {
            marks[0] = newest;
            markCount = 1;
            return;
        }

        while (newest >= mark(markCount - 1) + static_cast<double>(maxPeriod + 2))
        {
            const double last = mark(markCount - 1);
            marks[static_cast<size_t> (markCount & (markRingSize - 1))] = last + findNextPeriod(last);
            ++markCount;
        }
    }

    double findNextPeriod(double lastD) const noexcept
    {
        if (hintHz <= 0.0f)
            return static_cast<double>(unvoicedPeriod);

        const int64_t last = static_cast<int64_t>(std::llround(lastD));
        const double exactExpected = sampleRate / static_cast<double>(hintHz);
        const int expected = juce::jlimit(minPeriod, maxPeriod, static_cast<int> (std::lround(exactExpected)));
        const int window = expected;

        float refEnergy = 0.0f;
        for (int j = 1; j <= window; ++j)
            refEnergy += mono(last - j) * mono(last - j);
        if (refEnergy < 1.0e-8f * static_cast<float> (window))
            return exactExpected;

        const int lo = juce::jmax(minPeriod, expected * 4 / 5);
        const int hi = juce::jmin(maxPeriod, expected * 5 / 4);

        auto scoreAtLag = [&](int lag) noexcept -> float
        {
            float dot = 0.0f, energy = 0.0f;
            for (int j = 1; j <= window; ++j)
            {
                const float c = mono(last + lag - j);
                dot += c * mono(last - j);
                energy += c * c;
            }
            return dot / std::sqrt(refEnergy * energy + 1.0e-12f);
        };

        int best = expected;
        float bestScore = -2.0f;
        for (int lag = lo; lag <= hi; ++lag)
        {
            const float s = scoreAtLag(lag);
            if (s > bestScore)
            {
                bestScore = s;
                best = lag;
            }
        }

        if (bestScore < 0.7f)
            return exactExpected;

        double delta = 0.0;
        if (best > lo && best < hi)
        {
            const float sM1 = scoreAtLag(best - 1);
            const float s0  = bestScore;
            const float sP1 = scoreAtLag(best + 1);
            const float denom = sM1 - 2.0f * s0 + sP1;
            if (denom < -1.0e-6f)
                delta = juce::jlimit(-0.5, 0.5, 0.5 * static_cast<double>(sM1 - sP1) / static_cast<double>(denom));
        }

        return static_cast<double>(best) + delta;
    }

    void placeGrains() noexcept
    {
        const double frontier = static_cast<double> (written - 1 - synthesisLag);
        const int64_t minValidMark = std::max<int64_t> (1, markCount - markRingSize + 4);

        for (;;)
        {
            double pos;
            if (lastGrain < 0)
            {
                if (markCount < 3)
                    return;
                pos = mark(std::max<int64_t> (1, markCount - 3));
            }
            else
            {
                if (lastGrain + 1 >= markCount)
                    return;
                const int64_t gIdx = std::max<int64_t> (lastGrain, minValidMark);
                const double spacing = juce::jmax(2.0, mark(gIdx + 1) - mark(gIdx));
                pos = synthPos + spacing / juce::jmax(0.25, smoothedRatio);
            }

            if (pos > frontier)
                return;

            int64_t k = std::max<int64_t> (lastGrain, minValidMark);
            while (k + 1 < markCount && std::abs(mark(k + 1) - pos) < std::abs(mark(k) - pos))
                ++k;
            if (k + 1 >= markCount)
                return;

            addGrain(pos, k);
            synthPos = pos;
            lastGrain = k;
        }
    }

    void addGrain(double pos, int64_t k) noexcept
    {
        const double centre   = mark(k);
        const double rawLeft  = juce::jmax(2.0, centre - mark(k - 1));
        const double rawRight = juce::jmax(2.0, mark(k + 1) - centre);

        const float ampNorm = (smoothedRatio > 1.02)
            ? static_cast<float>(1.0 / std::pow(smoothedRatio, 0.65))
            : 1.0f;

        const double left  = rawLeft;
        const double right = rawRight;

        const int64_t first = static_cast<int64_t> (std::ceil(pos - left));
        const int64_t last  = static_cast<int64_t> (std::floor(pos + right));

        for (int64_t o = first; o <= last; ++o)
        {
            const double offset = static_cast<double> (o) - pos;
            const float gain = ampNorm * static_cast<float> (
                0.5 + 0.5 * std::cos(juce::MathConstants<double>::pi * offset / (offset < 0.0 ? left : right)));
            if (gain <= 0.0f)
                continue;

            const double src = centre + offset;
            const int64_t base = static_cast<int64_t> (std::floor(src));
            const float frac = static_cast<float> (src - static_cast<double> (base));
            const int out = static_cast<int> (o & ringMask);

            for (int ch = 0; ch < numChannels; ++ch)
                outputRing.addSample(ch, out, gain * readInput(ch, base, frac));
        }
    }

    float readInput(int ch, int64_t base, float frac) const noexcept
    {
        const float* d = inputRing.getReadPointer(ch);
        const float x1 = d[base & ringMask];
        if (frac == 0.0f)
            return x1;
        const float x0 = d[(base - 1) & ringMask], x2 = d[(base + 1) & ringMask], x3 = d[(base + 2) & ringMask];
        const float c1 = 0.5f * (x2 - x0);
        const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * frac + c2) * frac + c1) * frac + x1;
    }

    static constexpr double minHz = 60.0;
    static constexpr double maxHz = 1600.0;
    static constexpr double unvoicedHz = 200.0;
    static constexpr int    markRingSize = 256;

    double sampleRate = 44100.0;
    int numChannels = 0;
    int minPeriod = 36, maxPeriod = 735, unvoicedPeriod = 220;
    int synthesisLag = 0, latency = 0;
    int ringSize = 0, ringMask = 0;

    juce::AudioBuffer<float> inputRing, outputRing;
    std::vector<float> monoRing;

    int64_t written = 0;
    std::array<double, markRingSize> marks {};
    int64_t markCount = 0;
    int64_t lastGrain = -1;
    double  synthPos = 0.0;

    double targetRatio = 1.0;
    double smoothedRatio = 1.0;
    double fastRatioSmoothAlpha = 0.01;
    double humanRatioSmoothAlpha = 0.0035;
    double ratioSmoothAlpha = 0.01;
    bool   humanizeMode = false;
    bool   polyPassthrough = false;
    float  passthroughMix = 0.0f;
    float  hintHz = 0.0f;
};