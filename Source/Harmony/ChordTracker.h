#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

/**
    Real-time chord tracker for the Harmony tab's sidechain (the beat / instrumental).

    Every hop: Hann-windowed FFT -> log-compressed chromagram (41 Hz..1 kHz) plus a separate
    bass chromagram (41..260 Hz), both smoothed over ~0.3 s. Candidate chords (12 roots x 9
    qualities) are scored by cosine similarity with weighted templates, plus a bonus when the
    bass pitch class is the root. A new chord must win by a margin for ~150 ms before it
    replaces the current one (no flicker on passing notes).

    All buffers are allocated in prepare(); process() is real-time safe.
*/
class ChordTracker
{
public:
    enum Quality { major = 0, minor, dom7, maj7, min7, sus2, sus4, dim, power, numQualities };

    static const char* qualitySuffix (int q) noexcept
    {
        static const char* s[] = { "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim", "5" };
        return s[juce::jlimit (0, numQualities - 1, q)];
    }

    /** Pitch classes of a chord as a 12-bit mask (bit 0 = C). */
    static uint16_t chordMask (int root, int quality) noexcept
    {
        static const std::array<std::vector<int>, numQualities> iv {{
            { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 },
            { 0, 2, 7 }, { 0, 5, 7 }, { 0, 3, 6 }, { 0, 7 } }};
        uint16_t m = 0;
        for (int i : iv[(size_t) juce::jlimit (0, numQualities - 1, quality)])
            m |= (uint16_t) (1u << ((root + i) % 12));
        return m;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        fftOrder = sampleRate > 60000.0 ? 13 : 12;
        fftSize = 1 << fftOrder;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder + 1); // 2x zero padding for peak resolution
        ring.assign ((size_t) fftSize, 0.0f);
        work.assign ((size_t) fftSize * 4, 0.0f);
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        hop = juce::roundToInt (sampleRate * 0.04);

        // bin -> pitch class map (and bass flag), for the 2x zero-padded spectrum
        binPc.assign ((size_t) fftSize, -1);
        binBass.assign ((size_t) fftSize, false);
        for (int k = 1; k < fftSize; ++k)
        {
            const double hz = k * sampleRate / (2.0 * fftSize);
            if (hz < 41.0 || hz > 2100.0) continue;
            const double midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
            binPc[(size_t) k] = ((int) std::lround (midi) % 12 + 12) % 12;
            binBass[(size_t) k] = hz < 260.0;
        }
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        writePos = 0; sinceHop = 0;
        chroma.fill (0.0f); bass.fill (0.0f);
        current = -1; candidate = -1; candidateHops = 0;
        level.store (0.0f);
        currentChord.store (-1);
    }

    /** Feed mono sidechain samples. */
    void process (const float* x, int n) noexcept
    {
        if (fft == nullptr) return;
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) writePos] = x[i];
            writePos = (writePos + 1) & (fftSize - 1);
            if (++sinceHop >= hop)
            {
                sinceHop = 0;
                analyse();
            }
        }
    }

    /** Current chord as root * numQualities + quality, or -1 if none (silence / unclear). */
    int getChord() const noexcept { return currentChord.load (std::memory_order_relaxed); }
    float getLevel() const noexcept { return level.load (std::memory_order_relaxed); }
    float getChroma (int pc) const noexcept { return chromaOut[(size_t) (pc % 12)].load (std::memory_order_relaxed); }

private:
    void analyse() noexcept
    {
        double energy = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const float s = ring[(size_t) ((writePos + i) & (fftSize - 1))];
            work[(size_t) i] = s * window[(size_t) i];
            energy += (double) s * s;
        }
        std::fill (work.begin() + fftSize, work.end(), 0.0f);
        const float rms = (float) std::sqrt (energy / fftSize);
        level.store (rms, std::memory_order_relaxed);

        fft->performFrequencyOnlyForwardTransform (work.data(), true);

        // Spectral peaks, low to high. A peak on the 3rd/5th/6th/7th harmonic of a stronger lower
        // peak is mostly an overtone (it would add a false 5th / 3rd / 7th to the chord), so it
        // only counts a little. Octave harmonics share the pitch class and are harmless.
        int np = 0;
        const int nb = fftSize; // bins of the 2x padded transform
        for (int k = 2; k < nb - 2 && np < maxPeaks; ++k)
        {
            if (binPc[(size_t) k] < 0) continue;
            const float v = work[(size_t) k];
            if (v > work[(size_t) k - 1] && v >= work[(size_t) k + 1] && v > work[(size_t) k - 2] && v >= work[(size_t) k + 2])
            {
                peakBin[(size_t) np] = k;
                peakMag[(size_t) np] = v;
                ++np;
            }
        }
        std::array<float, 12> c {}, b {};
        for (int i = 0; i < np; ++i)
        {
            const int k = peakBin[(size_t) i];
            float weight = 1.0f;
            for (int j = 0; j < i && weight > 0.2f; ++j)
            {
                if (peakMag[(size_t) j] < peakMag[(size_t) i] * 0.8f) continue;
                const float ratio = (float) k / (float) peakBin[(size_t) j];
                for (int hNum : { 3, 5, 6, 7 })
                    if (std::abs (ratio - (float) hNum) < 0.03f * (float) hNum) weight = 0.15f;
            }
            const int pc = binPc[(size_t) k];
            const float m = weight * std::log1p (100.0f * peakMag[(size_t) i] / (float) fftSize);
            c[(size_t) pc] += m;
            if (binBass[(size_t) k]) b[(size_t) pc] += m;
        }
        auto normalise = [] (std::array<float, 12>& v)
        {
            float mx = 0.0f; for (float x : v) mx = std::max (mx, x);
            if (mx > 1.0e-9f) for (auto& x : v) x /= mx;
        };
        normalise (c); normalise (b);

        constexpr float a = 0.13f; // ~0.3 s smoothing at 40 ms hops
        for (int i = 0; i < 12; ++i)
        {
            chroma[(size_t) i] += a * (c[(size_t) i] - chroma[(size_t) i]);
            bass[(size_t) i]   += a * (b[(size_t) i] - bass[(size_t) i]);
            chromaOut[(size_t) i].store (chroma[(size_t) i], std::memory_order_relaxed);
        }

        if (rms < 2.0e-4f) // silence: forget the chord after a while
        {
            if (++silentHops > 25) { current = -1; currentChord.store (-1); }
            return;
        }
        silentHops = 0;

        // Score all chords
        int best = -1; float bestScore = -1.0f, currentScore = -1.0f;
        float cNorm = 0.0f; for (float x : chroma) cNorm += x * x; cNorm = std::sqrt (cNorm) + 1.0e-9f;
        int bassPc = 0; for (int i = 1; i < 12; ++i) if (bass[(size_t) i] > bass[(size_t) bassPc]) bassPc = i;

        for (int root = 0; root < 12; ++root)
            for (int q = 0; q < numQualities; ++q)
            {
                const uint16_t mask = chordMask (root, q);
                float dot = 0.0f, tNorm = 0.0f;
                for (int pc = 0; pc < 12; ++pc)
                {
                    float t = (mask >> pc) & 1u ? 1.0f : 0.0f;
                    if (pc == root && t > 0.0f) t = 1.3f;   // root weighs more
                    dot += t * chroma[(size_t) pc];
                    tNorm += t * t;
                }
                float score = dot / (cNorm * std::sqrt (tNorm));
                if (bassPc == root && bass[(size_t) bassPc] > 0.5f) score += 0.06f;
                // A 7th only counts when it's clearly played: a triad's overtones (the 3rd's
                // 3rd harmonic is the major 7th) put some energy there anyway.
                if (q == dom7 || q == maj7 || q == min7)
                {
                    const int sev = (root + (q == maj7 ? 11 : 10)) % 12;
                    float triad = 0.0f; int cnt = 0;
                    for (int pc = 0; pc < 12; ++pc)
                        if (((chordMask (root, q) >> pc) & 1u) && pc != sev) { triad += chroma[(size_t) pc]; ++cnt; }
                    triad /= (float) juce::jmax (1, cnt);
                    if (chroma[(size_t) sev] < 0.75f * triad) continue;
                    score -= 0.02f;
                }
                if (q == sus2 || q == sus4 || q == dim) score -= 0.05f;
                if (q == power) score -= 0.08f;
                const int id = root * numQualities + q;
                if (score > bestScore) { bestScore = score; best = id; }
                if (id == current) currentScore = score;
            }

        if (best == current) { candidateHops = 0; return; }
        if (best == candidate) ++candidateHops; else { candidate = best; candidateHops = 1; }
        const bool clearlyBetter = current < 0 || bestScore > currentScore + 0.02f;
        if (clearlyBetter && candidateHops >= 4) // ~160 ms
        {
            current = best;
            candidateHops = 0;
            currentChord.store (current, std::memory_order_relaxed);
        }
    }

    double sr = 48000.0;
    int fftOrder = 12, fftSize = 4096, hop = 1920;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> ring, work, window;
    std::vector<int> binPc;
    std::vector<bool> binBass;
    static constexpr int maxPeaks = 160;
    std::array<int, maxPeaks> peakBin {};
    std::array<float, maxPeaks> peakMag {};
    int writePos = 0, sinceHop = 0, silentHops = 0;
    std::array<float, 12> chroma {}, bass {};
    std::array<std::atomic<float>, 12> chromaOut {};
    int current = -1, candidate = -1, candidateHops = 0;
    std::atomic<float> level { 0.0f };
    std::atomic<int> currentChord { -1 };
};
