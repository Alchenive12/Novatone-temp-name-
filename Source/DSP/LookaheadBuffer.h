#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <cstdint>

/**
    Circular lookahead delay line (blueprint section 4): buffers the input signal so both
    the pitch-shifter's wet output and the untouched dry signal can be read back time-
    aligned at the plugin's output, letting the analysis/quantizer/smoother chain react to
    audio before it is heard.

    Two synchronised read taps are provided:
      - readWet(): delayed by L_lookahead samples, feeding the pitch-shifting engine.
      - readDry(): delayed by L_lookahead + L_wet samples (L_total), feeding the other
        side of the voicing-gate crossfader.

    Both taps support fractional-sample delay via linear or cubic Hermite interpolation
    (blueprint 4.6), since t_lookahead and the pitch shifter's own latency need not land on
    an integer number of samples.

    All heap allocation happens in prepare(); pushSample()/readWet()/readDry()/
    pushAndRead() perform no allocation, locking, or I/O and are safe on the audio thread.
*/
class LookaheadBuffer
{
public:
    enum class Interpolation { linear, cubic };

    /** Result of pushAndRead(): the two time-aligned taps described above. */
    struct Reads
    {
        float wet = 0.0f;
        float dry = 0.0f;
    };

    LookaheadBuffer() = default;

    /** Sizes the circular buffer and precomputes both delay taps. Not real-time safe.
        @param newSampleRate         audio sample rate, in Hz.
        @param newWindowSize         W: the MPM detector's analysis window, in samples.
        @param newLookaheadSeconds   t_lookahead: how far the analysis leads the output, in
                                      seconds. Per blueprint 4.7, should be >= W / (2 * fs).
        @param newWetLatencySamples  L_wet: the pitch-shifting engine's own internal
                                      latency, in (possibly fractional) samples.
        @param newInterpolation      interpolation used by both read taps (default: cubic).
    */
    void prepare (double newSampleRate, int newWindowSize, float newLookaheadSeconds,
                  float newWetLatencySamples, Interpolation newInterpolation = Interpolation::cubic);

    /** Zeroes the buffer and rewinds the write position. Allocation-free; safe on the
        audio thread. */
    void reset() noexcept;

    /** Writes one input sample and advances the write head: buf[w[n]] = x[n] (4.3). */
    void pushSample (float x) noexcept;

    /** Reads the wet tap at the current write position: x[n - L_lookahead] (4.3). Call
        after the pushSample() for the same n. */
    float readWet() const noexcept;

    /** Reads the dry tap at the current write position: x[n - L_total] (4.3). Call after
        the pushSample() for the same n. */
    float readDry() const noexcept;

    /** Convenience: pushSample() followed by both reads, matching control-flow steps
        1, 9 and 11 (blueprint section 5) in one call. */
    Reads pushAndRead (float x) noexcept;

    /** L_report = ceil(L_total), for AudioProcessor::setLatencySamples() (blueprint 4.5). */
    int getReportedLatencySamples() const noexcept;

    /** t_report = L_report / fs (blueprint 4.5). */
    double getReportedLatencySeconds() const noexcept;

private:
    float readInterpolated (float delaySamples) const noexcept; // blueprint 4.6

    // N_buf is always a power of two, so modulo reduces to a mask. index & bufferMask is
    // well-defined for negative index under C++20's mandated two's-complement signed
    // integers, and correctly reproduces index mod N_buf in [0, N_buf).
    size_t wrapIndex (int64_t index) const noexcept { return static_cast<size_t> (index & bufferMask); }

    double sampleRate           = 44100.0;
    int    windowSize           = 0;    // W
    float  lookaheadSecondsCfg  = 0.0f; // t_lookahead
    float  wetLatencySamplesCfg = 0.0f; // L_wet
    Interpolation interpolation = Interpolation::cubic;

    float   lookaheadSamples    = 0.0f; // L_lookahead
    float   totalLatencySamples = 0.0f; // L_total
    int64_t bufferSize          = 1;    // N_buf
    int64_t bufferMask          = 0;    // N_buf - 1

    std::vector<float> buffer;
    int64_t sampleCounter = -1; // n: index of the most recently written sample

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LookaheadBuffer)
};

