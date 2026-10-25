#pragma once

#include <juce_dsp/juce_dsp.h>
#include <optional>
#include <vector>
#include <cstdint>
#include <array>

/**
    Result of analysing a single frame of audio for its fundamental pitch.
*/
struct MPMResult
{
    float pitchHz = 0.0f;   ///< Estimated fundamental frequency in Hz. 0 if unvoiced.
    float clarity = 0.0f;   ///< NSDF peak height at the chosen lag, roughly in [0, 1].
    bool  isVoiced = false;  ///< True if a confident pitch estimate was found this frame.
};

/**
    Real-time-safe monophonic pitch detector implementing the McLeod Pitch Method (MPM)
    [McLeod & Wyvill, "A Smarter Way to Find Pitch", ICMC 2005].

    Pipeline per frame:
      0. DC removal       -> a DC offset correlates with itself at every lag and would
                             swamp the NSDF, so the mean is subtracted first.
      1. NSDF numerator   -> FFT-based autocorrelation, O(N log N).
         NSDF denominator -> O(1)-per-lag running energy sum (double precision).
      2. Peak picking     -> ONE key maximum per positive lobe of the NSDF (the textbook
                             definition), then the first key maximum clearing both
                             k * n_max and C_min.
      3. Parabolic interpolation for sub-sample lag / frequency.
      4. Clarity = interpolated NSDF peak height.

    Lowest detectable pitch is sampleRate / (windowSize / 2). Pick the window so that
    getMinDetectableHz() is comfortably below the lowest note you care about
    (TuneEngine uses ~40 ms, i.e. ~47 Hz at 48 kHz).

    All heap allocation is confined to prepare(). processFrame() performs no allocation,
    locking, or file I/O, and is safe to call from the audio thread. A single instance
    tracks one monophonic voice and is not safe to call concurrently from multiple threads.
*/
class MPMDetector
{
public:
    MPMDetector() = default;

    /** Allocates every buffer used by processFrame(). NOT real-time safe.

        @param newSampleRate  the rate of the audio passed to processFrame().
        @param newWindowSize  the exact number of samples processFrame() will be given
                               on every call.
    */
    void prepare(double newSampleRate, int newWindowSize);

    /** Clears transient buffer contents. Allocation-free; safe on the audio thread. */
    void reset() noexcept;

    /** Analyses exactly getWindowSize() samples and returns the detected pitch.
        Real-time safe: no allocations, no locks, no I/O. */
    MPMResult processFrame(const float* samples) noexcept;

    int getWindowSize() const noexcept { return windowSize; }

    /** Lowest fundamental this configuration can report. */
    float getMinDetectableHz() const noexcept
    {
        return maxLag > 0 ? static_cast<float> (sampleRate / static_cast<double> (maxLag)) : 0.0f;
    }

    /** k in T = k * n_max. Lower = favours the shorter lag (fewer sub-octave errors),
        higher = favours the tallest peak. McLeod suggests 0.8 - 0.93. Not thread-safe
        against processFrame(); call from the same thread. */
    void setThresholdRatio(float k) noexcept { thresholdRatio = juce::jlimit(0.5f, 1.0f, k); }

    /** All key maxima of the last frame (one per positive NSDF lobe), shortest
        lag first, whether or not they passed the k / C_min gates. */
    int getNumCandidates() const noexcept { return numCandidates; }
    const MPMResult& getCandidate(int i) const noexcept { return candidates[static_cast<size_t> (i)]; }

    /** C_min: an absolute clarity floor below which a frame is declared unvoiced. */
    void setClarityFloor(float c) noexcept { clarityFloor = juce::jlimit(0.0f, 1.0f, c); }

private:
    void computeNSDF(const float* samples) noexcept;
    int  findKeyMaximum() noexcept; // returns tau0, or -1 if the frame is unvoiced
    float interpolatedLag(int tau0, float& peakValue) const noexcept;

    // ---- configuration, fixed by prepare() ----
    double sampleRate = 44100.0;
    int windowSize = 0;   // W
    static constexpr double maxDetectHz = 1200.0; // TuneEngine rejects anything higher
    int minLag = 2;   // tau_min = fs / maxDetectHz
    int maxLag = 0;   // tau_max: capped at W/2, see prepare() for rationale
    int fftOrder = 0;
    int fftSize = 0;   // N: power of two, >= 2 * W (avoids circular-convolution wrap)

    // ---- MPM tuning (blueprint step 2) ----
    float thresholdRatio = 0.90f; // k
    float clarityFloor = 0.70f; // C_min

    // ---- pre-allocated working buffers (sized once in prepare(), never resized after) ----
    std::optional<juce::dsp::FFT> fft;
    std::vector<float>   fftBuffer;    // 2 * fftSize floats, interleaved re/im
    std::vector<float>   nsdf;         // n(tau), tau = 0 .. maxLag
    std::vector<float>   frame;        // the analysed window with its DC removed

    static constexpr int maxKeyMaxima = 64;
    std::array<int, maxKeyMaxima> keyMaxima {};
    int numKeyMaxima = 0;
    std::array<MPMResult, maxKeyMaxima> candidates {};
    int numCandidates = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MPMDetector)
};
