#include "MPMDetector.h"

//==============================================================================
void MPMDetector::prepare(double newSampleRate, int newWindowSize)
{
    jassert(newSampleRate > 0.0);
    jassert(newWindowSize >= 8);

    sampleRate = newSampleRate;
    windowSize = newWindowSize;

    // A lag close to W is supported by very few overlapping samples and is
    // statistically unreliable, so - as is standard practice for MPM - we only
    // search the lower half of the window. For an 8 kHz analysis stream this
    // still reaches well below any playable fundamental (W = 1024 -> f0_min ~ 15.6 Hz).
    maxLag = windowSize / 2;

    // Lags shorter than one 1200 Hz period are never searched: a sibilant or
    // formant ripple there would win as the "smallest lag" key maximum and the
    // whole frame would then be thrown away as out of range.
    minLag = juce::jlimit(2, juce::jmax(2, maxLag - 1), static_cast<int>(std::floor(sampleRate / maxDetectHz)));

    // N = smallest power of two >= 2W. Zero-padding to 2W is what turns the FFT's
    // circular convolution into the linear autocorrelation we actually want
    // (architecture constraint / blueprint step 1).
    fftSize = 1;
    fftOrder = 0;
    while (fftSize < 2 * windowSize)
    {
        fftSize <<= 1;
        ++fftOrder;
    }

    fft.emplace(fftOrder);

    fftBuffer.assign(static_cast<size_t>(2 * fftSize), 0.0f);
    nsdf.assign(static_cast<size_t>(maxLag + 1), 0.0f);
    frame.assign(static_cast<size_t>(windowSize), 0.0f);
}

void MPMDetector::reset() noexcept
{
    std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
    std::fill(nsdf.begin(), nsdf.end(), 0.0f);
    std::fill(frame.begin(), frame.end(), 0.0f);
}

//==============================================================================
void MPMDetector::computeNSDF(const float* input) noexcept
{
    const int W = windowSize;

    // Step 0: remove DC. A constant offset correlates with itself at every lag
    // and lifts the whole NSDF, which hides the period peaks.
    double mean = 0.0;
    for (int j = 0; j < W; ++j)
        mean += input[j];
    mean /= static_cast<double> (W);
    for (int j = 0; j < W; ++j)
        frame[static_cast<size_t> (j)] = input[j] - static_cast<float> (mean);
    const float* samples = frame.data();

    // ---------------- Numerator: r(tau), tau = 0..maxLag ----------------
    // r(tau) = sum_{j=0}^{W-tau-1} x[j] * x[j+tau], computed via FFT autocorrelation
    // instead of an O(N^2) nested loop (architecture constraint).
    std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
    for (int j = 0; j < W; ++j)
        fftBuffer[static_cast<size_t>(j)] = samples[j];

    fft->performRealOnlyForwardTransform(fftBuffer.data());

    // Overwrite the spectrum with the (real) power spectrum |X[k]|^2 = X[k] * conj(X[k]).
    for (int k = 0; k < fftSize; ++k)
    {
        const size_t reIdx = static_cast<size_t>(2 * k);
        const size_t imIdx = reIdx + 1;
        const float  re = fftBuffer[reIdx];
        const float  im = fftBuffer[imIdx];
        fftBuffer[reIdx] = re * re + im * im;
        fftBuffer[imIdx] = 0.0f;
    }

    fft->performRealOnlyInverseTransform(fftBuffer.data());
    // By the Wiener-Khinchin theorem, fftBuffer[tau] is now the circular
    // autocorrelation of the zero-padded frame. Because fftSize >= 2W, the padded
    // zeros absorb any wraparound, so this equals the exact linear r(tau) for
    // every tau <= maxLag (maxLag <= W/2 << fftSize).

    // ---------------- Denominator: m'(tau) via O(1)-per-lag running sum ----------------
    // m'(0)   = 2 * sum_{j=0}^{W-1} x_j^2
    // m'(tau) = m'(tau-1) - x[tau-1]^2 - x[W-tau]^2   (derived from the definition,
    //           avoids recomputing the sum from scratch at every lag).
    // Kept in double: the running subtraction loses float precision at long lags.
    double sumSq = 0.0;
    for (int j = 0; j < W; ++j)
        sumSq += static_cast<double>(samples[j]) * samples[j];

    double m = 2.0 * sumSq;

    for (int tau = 0; tau <= maxLag; ++tau)
    {
        const double r = fftBuffer[static_cast<size_t>(tau)];
        nsdf[static_cast<size_t>(tau)] = (m > 1.0e-9) ? static_cast<float>(2.0 * r / m) : 0.0f;

        if (tau < maxLag) // roll m'(tau) forward to m'(tau + 1)
        {
            const double a = samples[tau], b = samples[W - 1 - tau];
            m -= a * a + b * b;
        }
    }
}

//==============================================================================
int MPMDetector::findKeyMaximum() noexcept
{
    // Key maxima are the highest point of each positive lobe of the NSDF, i.e.
    // between a positive-going and the next negative-going zero crossing.
    // The lobe around tau = 0 is NOT a candidate: it is the signal matching
    // itself, and a ripple on its slope can look like a tiny-lag peak (that is
    // how a 150 Hz voice used to read as 20 kHz and go untuned). So the search
    // starts at the first negative value.
    numKeyMaxima = 0;
    int tau = 1;
    while (tau <= maxLag && nsdf[static_cast<size_t> (tau)] >= 0.0f)
        ++tau;
    if (tau > maxLag)
        return -1; // never goes negative: no oscillation, or a pitch below the range

    float globalMax = 0.0f;
    while (tau <= maxLag && numKeyMaxima < maxKeyMaxima)
    {
        while (tau <= maxLag && nsdf[static_cast<size_t> (tau)] < 0.0f)  // skip the negative region
            ++tau;
        int best = -1;
        float bestVal = 0.0f;
        while (tau <= maxLag && nsdf[static_cast<size_t> (tau)] >= 0.0f)
        {
            if (nsdf[static_cast<size_t> (tau)] > bestVal)
            {
                bestVal = nsdf[static_cast<size_t> (tau)];
                best = tau;
            }
            ++tau;
        }
        // A lobe cut off by the end of the range only counts if its peak is a
        // real local maximum (not still rising at maxLag). Peaks below minLag
        // (above 1200 Hz) never count, so they can't set n_max either.
        if (best >= minLag && best < maxLag)
        {
            keyMaxima[static_cast<size_t> (numKeyMaxima++)] = best;
            globalMax = juce::jmax(globalMax, bestVal);
        }
    }

    if (globalMax <= 0.0f)
        return -1;

    const float threshold = thresholdRatio * globalMax; // T = k * n_max
    for (int i = 0; i < numKeyMaxima; ++i)
    {
        const int k = keyMaxima[static_cast<size_t> (i)];
        const float v = nsdf[static_cast<size_t> (k)];
        if (v >= threshold && v >= clarityFloor)
            return k;
    }
    return -1; // key maxima existed, but none passed both gates -> unvoiced
}

//==============================================================================
MPMResult MPMDetector::processFrame(const float* samples) noexcept
{
    jassert(samples != nullptr);
    jassert(fft.has_value()); // prepare() must be called before processFrame()

    computeNSDF(samples);
    const int tau0 = findKeyMaximum();

    // Every key maximum becomes a candidate, so a tracker can prefer the one
    // that continues the current note over the frame's own first pick.
    numCandidates = 0;
    for (int i = 0; i < numKeyMaxima; ++i)
    {
        const int k = keyMaxima[static_cast<size_t> (i)];
        float clarity = 0.0f;
        const float lag = interpolatedLag(k, clarity);
        if (lag > 0.0f)
        {
            candidates[static_cast<size_t> (numCandidates)] = { static_cast<float> (sampleRate / lag), clarity, true };
            ++numCandidates;
        }
    }

    if (tau0 < 0)
        return {}; // unvoiced: pitchHz = 0, clarity = 0, isVoiced = false

    MPMResult result;
    const float lag = interpolatedLag(tau0, result.clarity);
    result.pitchHz = lag > 0.0f ? static_cast<float> (sampleRate / lag) : 0.0f;
    result.isVoiced = result.pitchHz > 0.0f;
    return result;
}

float MPMDetector::interpolatedLag(int tau0, float& peakValue) const noexcept
{
    // tau0 is a key maximum, so it lies in [1, maxLag - 1]: both neighbours exist.
    const float yM1 = nsdf[static_cast<size_t>(tau0 - 1)];
    const float y0 = nsdf[static_cast<size_t>(tau0)];
    const float yP1 = nsdf[static_cast<size_t>(tau0 + 1)];

    const float denom = yM1 - 2.0f * y0 + yP1;
    float delta = 0.0f;
    if (denom < 0.0f) // only a genuine, concave-down peak is interpolated
        delta = juce::jlimit(-1.0f, 1.0f, 0.5f * (yM1 - yP1) / denom);

    // Clarity = the interpolated peak height (blueprint step 4), capped at 1.
    peakValue = juce::jmin(1.0f, y0 - 0.25f * (yM1 - yP1) * delta);
    return static_cast<float> (tau0) + delta;
}

