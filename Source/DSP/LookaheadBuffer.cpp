#include "LookaheadBuffer.h"
#include <algorithm>

//==============================================================================
void LookaheadBuffer::prepare (double newSampleRate, int newWindowSize, float newLookaheadSeconds,
                                float newWetLatencySamples, Interpolation newInterpolation)
{
    jassert (newSampleRate > 0.0);
    jassert (newWindowSize > 0);
    jassert (newLookaheadSeconds >= 0.0f);
    jassert (newWetLatencySamples >= 0.0f);

    sampleRate           = newSampleRate;
    windowSize           = newWindowSize;
    lookaheadSecondsCfg  = newLookaheadSeconds;
    wetLatencySamplesCfg = newWetLatencySamples;
    interpolation        = newInterpolation;

    // L_lookahead = t_lookahead * fs                                        (blueprint 4.1)
    lookaheadSamples = lookaheadSecondsCfg * static_cast<float> (sampleRate);

    // Sanity check against blueprint 4.7: the analysis window must be fully populated
    // before the corresponding output sample leaves the plugin.
    jassert (lookaheadSamples >= static_cast<float> (windowSize) * 0.5f);

    // L_total = L_lookahead + L_wet                                         (blueprint 4.1)
    totalLatencySamples = lookaheadSamples + wetLatencySamplesCfg;

    // N_buf = 2^ceil(log2(L_total + W + 1))                                 (blueprint 4.2)
    const double minSize = static_cast<double> (totalLatencySamples)
                          + static_cast<double> (windowSize) + 1.0;
    int64_t size = 1;
    while (static_cast<double> (size) < minSize)
        size <<= 1;

    bufferSize = size;
    bufferMask = bufferSize - 1;

    buffer.assign (static_cast<size_t> (bufferSize), 0.0f);
    reset();
}

void LookaheadBuffer::reset() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    sampleCounter = -1;
}

//==============================================================================
void LookaheadBuffer::pushSample (float x) noexcept
{
    ++sampleCounter;
    buffer[wrapIndex (sampleCounter)] = x; // buf[w[n]] = x[n]                (blueprint 4.3)
}

float LookaheadBuffer::readWet() const noexcept
{
    return readInterpolated (lookaheadSamples); // r_wet[n] = n - L_lookahead (4.3)
}

float LookaheadBuffer::readDry() const noexcept
{
    return readInterpolated (totalLatencySamples); // r_dry[n] = n - L_total  (4.3)
}

LookaheadBuffer::Reads LookaheadBuffer::pushAndRead (float x) noexcept
{
    pushSample (x);
    return { readWet(), readDry() };
}

int LookaheadBuffer::getReportedLatencySamples() const noexcept
{
    return static_cast<int> (std::ceil (totalLatencySamples)); // L_report    (blueprint 4.5)
}

double LookaheadBuffer::getReportedLatencySeconds() const noexcept
{
    return static_cast<double> (getReportedLatencySamples()) / sampleRate; // t_report (4.5)
}

//==============================================================================
float LookaheadBuffer::readInterpolated (float delaySamples) const noexcept
{
    const int   k  = static_cast<int> (std::floor (delaySamples));
    const float mu = delaySamples - static_cast<float> (k);

    const int64_t base = sampleCounter - k; // index of s_0 = buf[n - k]

    if (interpolation == Interpolation::linear)
    {
        // read_frac(n - L) = (1 - mu) * buf[n - k] + mu * buf[n - k - 1]     (blueprint 4.6)
        const float s0 = buffer[wrapIndex (base)];
        const float s1 = buffer[wrapIndex (base - 1)];
        return (1.0f - mu) * s0 + mu * s1;
    }

    // Cubic Hermite (blueprint 4.6). Indexing matches the linear case's convention: s_0 =
    // buf[n-k] and s_1 = buf[n-k-1] are the two samples straddling the read position, with
    // s_-1 and s_2 the extra support points on either side.
    const float sM1 = buffer[wrapIndex (base + 1)];
    const float s0  = buffer[wrapIndex (base)];
    const float s1  = buffer[wrapIndex (base - 1)];
    const float s2  = buffer[wrapIndex (base - 2)];

    const float mu2 = mu * mu;
    const float mu3 = mu2 * mu;

    const float hM1 = -0.5f * mu + mu2 - 0.5f * mu3;
    const float h0  = 1.0f - 2.5f * mu2 + 1.5f * mu3;
    const float h1  = 0.5f * mu + 2.0f * mu2 - 1.5f * mu3;
    const float h2  = -0.5f * mu2 + 0.5f * mu3;

    return hM1 * sM1 + h0 * s0 + h1 * s1 + h2 * s2;
}

