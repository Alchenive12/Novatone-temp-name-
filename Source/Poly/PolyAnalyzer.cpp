#include "PolyAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace Poly
{
namespace
{
    inline float hzToMidi (float hz) noexcept   { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
    inline float midiToHz (float m) noexcept    { return 440.0f * std::exp2 ((m - 69.0f) / 12.0f); }

    struct Peak
    {
        float hz     = 0.0f;
        float rawAmp = 0.0f;   ///< linear sinusoid amplitude
        float white  = 0.0f;   ///< whitened amplitude (salience domain)
        float resid  = 0.0f;   ///< whitened amplitude still unexplained by chosen F0s
    };

    struct FrameF0
    {
        float midi     = 0.0f;
        float salience = 0.0f;
        float amp      = 0.0f;
    };

    //==========================================================================
    class FrameAnalyzer
    {
    public:
        FrameAnalyzer (double sr, const AnalyzerSettings& s)
            : sampleRate (sr), settings (s)
        {
            winLen  = juce::nextPowerOfTwo (juce::roundToInt (sr * 0.08));
            fftSize = winLen * 4;
            int order = 0;
            while ((1 << order) < fftSize) ++order;
            fft = std::make_unique<juce::dsp::FFT> (order);
            fftBuf.assign ((size_t) fftSize * 2, 0.0f);
            mag.assign ((size_t) fftSize / 2 + 1, 0.0f);
            powPrefix.assign (mag.size() + 1, 0.0);
            window.resize ((size_t) winLen);
            for (int i = 0; i < winLen; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) winLen);
            ampScale = 4.0f / (float) winLen; // Hann coherent gain 0.5, one-sided spectrum x2
            binHz = (float) (sr / fftSize);

            // Candidate F0 grid, quarter-semitone steps (refined later)
            for (float m = settings.minMidi; m <= settings.maxMidi + 1.0e-3f; m += 0.25f)
                grid.push_back (midiToHz (m));

            // Tuned on tools/PolyBench (guitar + vocal stacks); sensitivity 0.5 = bench optimum.
            gamma    = juce::jmap (settings.sensitivity, 0.0f, 1.0f, 0.55f, 0.25f);
            relFloor = juce::jmap (settings.sensitivity, 0.0f, 1.0f, 0.16f, 0.05f);
        }

        int getWindowLength() const noexcept { return winLen; }
        float getLastFlux() const noexcept { return lastFlux; }

        /** Returns the loudest raw partial amplitude of this frame (for the silence gate). */
        float computePeaks (const float* x, int numSamples, int centre)
        {
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            const int start = centre - winLen / 2;
            for (int i = 0; i < winLen; ++i)
            {
                const int n = start + i;
                if (n >= 0 && n < numSamples)
                    fftBuf[(size_t) i] = x[n] * window[(size_t) i];
            }
            fft->performFrequencyOnlyForwardTransform (fftBuf.data(), true);
            const int nb = (int) mag.size();
            if (prevLog.size() != mag.size()) prevLog.assign (mag.size(), 0.0f);
            lastFlux = 0.0f;
            const int fluxHi = juce::jmin (nb, (int) (8000.0f / binHz));
            for (int k = 0; k < nb; ++k)
            {
                mag[(size_t) k] = fftBuf[(size_t) k] * ampScale;
                powPrefix[(size_t) k + 1] = powPrefix[(size_t) k] + (double) mag[(size_t) k] * mag[(size_t) k];
                if (k < fluxHi)
                {
                    const float l = std::log1p (1000.0f * mag[(size_t) k]);
                    lastFlux += juce::jmax (0.0f, l - prevLog[(size_t) k]);
                    prevLog[(size_t) k] = l;
                }
            }

            peaks.clear();
            const int kMin = juce::jmax (3, (int) std::ceil (40.0f / binHz));
            const int kMax = juce::jmin (nb - 4, (int) std::floor (juce::jmin (7000.0f, (float) sampleRate * 0.45f) / binHz));
            float frameMax = 0.0f;
            for (int k = kMin; k <= kMax; ++k)
                frameMax = juce::jmax (frameMax, mag[(size_t) k]);
            if (frameMax <= 1.0e-7f)
                return 0.0f;

            const float floorAmp = frameMax * 0.0010f; // -60 dB below the frame's loudest partial
            for (int k = kMin; k <= kMax; ++k)
            {
                const float y0 = mag[(size_t) k];
                if (y0 < floorAmp) continue;
                if (! (y0 > mag[(size_t) k - 1] && y0 >= mag[(size_t) k + 1] && y0 > mag[(size_t) k - 2] && y0 >= mag[(size_t) k + 2]))
                    continue;

                // Reject window side-lobes: must be within 30 dB of the local maximum.
                float localMax = y0;
                for (int j = juce::jmax (kMin, k - 12); j <= juce::jmin (kMax, k + 12); ++j)
                    localMax = juce::jmax (localMax, mag[(size_t) j]);
                if (y0 < localMax * 0.035f) continue;

                const float l1 = std::log (juce::jmax (1.0e-12f, mag[(size_t) k - 1]));
                const float l0 = std::log (y0);
                const float r1 = std::log (juce::jmax (1.0e-12f, mag[(size_t) k + 1]));
                const float den = l1 - 2.0f * l0 + r1;
                float d = 0.0f;
                if (den < -1.0e-9f) d = juce::jlimit (-0.5f, 0.5f, 0.5f * (l1 - r1) / den);

                Peak p;
                p.hz = ((float) k + d) * binHz;
                p.rawAmp = std::exp (l0 - 0.25f * (l1 - r1) * d);
                peaks.push_back (p);
            }

            // Spectral whitening: divide by the local (1/3-octave) spectral level ^ (1 - nu)
            constexpr float nu = 0.33f;
            for (auto& p : peaks)
            {
                const int lo = juce::jmax (0, (int) std::floor (p.hz * 0.891f / binHz));
                const int hi = juce::jmin (nb - 1, (int) std::ceil (p.hz * 1.122f / binHz));
                const double meanPow = (powPrefix[(size_t) hi + 1] - powPrefix[(size_t) lo]) / juce::jmax (1, hi - lo + 1);
                const float sigma = (float) std::sqrt (juce::jmax (1.0e-20, meanPow));
                p.white = p.rawAmp * std::pow (sigma, nu - 1.0f);
                p.resid = p.white;
            }
            return frameMax;
        }

        /** Iterative multi-F0 estimation on the current peaks. */
        void estimate (std::vector<FrameF0>& out)
        {
            out.clear();
            if (peaks.empty()) return;

            float sumS = 0.0f, prevScore = 0.0f, firstS = 0.0f;
            for (int j = 1; j <= settings.maxPolyphony; ++j)
            {
                // Coarse search
                float bestS = 0.0f, bestF = 0.0f;
                for (float f : grid)
                {
                    const float s = salience (f);
                    if (s > bestS) { bestS = s; bestF = f; }
                }
                if (bestS <= 0.0f) break;

                // Refine within +-0.15 st, then by the matched partials themselves
                for (float dm = -0.15f; dm <= 0.15f + 1.0e-4f; dm += 0.05f)
                {
                    const float f = bestF * std::exp2 (dm / 12.0f);
                    const float s = salience (f);
                    if (s > bestS) { bestS = s; bestF = f; }
                }
                bestF = refineFromPartials (bestF);
                bestS = juce::jmax (bestS, salience (bestF));

                if (j == 1) firstS = bestS;
                if (j > 1 && bestS < relFloor * firstS) break;

                sumS += bestS;
                const float score = sumS / std::pow ((float) j, gamma);
                if (j > 1 && score <= prevScore) break;
                prevScore = score;

                FrameF0 f0;
                f0.midi = hzToMidi (bestF);
                f0.salience = bestS;
                f0.amp = subtract (bestF);
                out.push_back (f0);
            }
        }

    private:
        static float weight (float f0, int m) noexcept
        {
            constexpr float alpha = 52.0f, beta = 320.0f;
            return (f0 + alpha) / ((float) m * f0 + beta);
        }

        int numHarmonics (float f0) const noexcept
        {
            return juce::jlimit (1, 20, (int) std::floor (juce::jmin (6000.0f, (float) sampleRate * 0.45f) / f0));
        }

        /** Index of the peak with the largest residual inside the tolerance around `hz`, or -1. */
        int matchPeak (float hz, bool useResidual) const noexcept
        {
            const float tol = juce::jmax (1.5f * binHz, hz * 0.022f);
            auto it = std::lower_bound (peaks.begin(), peaks.end(), hz - tol,
                                        [] (const Peak& p, float v) { return p.hz < v; });
            int best = -1;
            float bestA = 0.0f;
            for (; it != peaks.end() && it->hz <= hz + tol; ++it)
            {
                const float a = useResidual ? it->resid : it->white;
                // Prefer the closest-in-frequency of comparable peaks
                const float closeness = 1.0f - 0.5f * std::abs (it->hz - hz) / tol;
                if (a * closeness > bestA) { bestA = a * closeness; best = (int) (it - peaks.begin()); }
            }
            return best;
        }

        float salience (float f0) const noexcept
        {
            const int M = numHarmonics (f0);
            float s = 0.0f, maxTerm = 0.0f;
            int found = 0;
            for (int m = 1; m <= M; ++m)
            {
                const int p = matchPeak (f0 * (float) m, true);
                if (p >= 0)
                {
                    const float term = weight (f0, m) * peaks[(size_t) p].resid;
                    s += term;
                    maxTerm = juce::jmax (maxTerm, term);
                    if (m <= 4) ++found;
                }
            }
            // A real pitched note shows at least two of its first four partials, and its
            // evidence isn't concentrated in one partial (a formant-boosted overtone of
            // another voice is).
            if (found < 2 || maxTerm > dominance * s) return 0.0f;
            return s;
        }

        float refineFromPartials (float f0) const noexcept
        {
            const int M = juce::jmin (8, numHarmonics (f0));
            double num = 0.0, den = 0.0;
            for (int m = 1; m <= M; ++m)
            {
                const int p = matchPeak (f0 * (float) m, true);
                if (p < 0) continue;
                const double w = weight (f0, m) * peaks[(size_t) p].resid;
                num += w * peaks[(size_t) p].hz / m;
                den += w;
            }
            return den > 0.0 ? (float) (num / den) : f0;
        }

        /** Removes the chosen F0's partials from the residual (spectral smoothness) and
            returns its raw amplitude. */
        float subtract (float f0)
        {
            const int M = numHarmonics (f0);
            std::array<int, 21> idx {};
            std::array<float, 21> a {};
            for (int m = 1; m <= M; ++m)
            {
                idx[(size_t) m] = matchPeak (f0 * (float) m, true);
                a[(size_t) m] = idx[(size_t) m] >= 0 ? peaks[(size_t) idx[(size_t) m]].resid : 0.0f;
            }

            float rawEnergy = 0.0f;
            for (int m = 1; m <= M; ++m)
            {
                const int p = idx[(size_t) m];
                if (p < 0) continue;
                // Smooth spectral envelope expectation for this harmonic: mean of its neighbours.
                const float lo = a[(size_t) juce::jmax (1, m - 1)], hi = a[(size_t) juce::jmin (M, m + 1)];
                const float expected = (lo + a[(size_t) m] + hi) / 3.0f;
                const float take = juce::jmin (a[(size_t) m], juce::jmax (expected, 0.5f * a[(size_t) m]));
                auto& pk = peaks[(size_t) p];
                const float frac = pk.white > 0.0f ? take / pk.white : 0.0f;
                if (m <= 6) rawEnergy += (frac * pk.rawAmp) * (frac * pk.rawAmp);
                pk.resid = juce::jmax (0.0f, pk.resid - take);
            }
            return std::sqrt (rawEnergy);
        }

        double sampleRate;
        AnalyzerSettings settings;
        int winLen = 4096, fftSize = 16384;
        float ampScale = 1.0f, binHz = 1.0f;
        float gamma = 0.7f, relFloor = 0.12f;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> fftBuf, mag, window, grid;
        std::vector<double> powPrefix;
        std::vector<Peak> peaks;
        std::vector<float> prevLog;
        float lastFlux = 0.0f;
        float dominance = 0.60f;
    };

    //==========================================================================
    // Note tracking
    struct Track
    {
        int start = 0;
        int lastFrame = 0;
        float lastMidi = 0.0f;
        std::vector<float> midi, amp, sal; // per frame from start (gaps filled later)
        std::vector<bool> present;
        bool open = true;
    };

    void appendToTrack (Track& t, int frame, const FrameF0& d)
    {
        // fill any gap by holding
        while (t.start + (int) t.midi.size() < frame)
        {
            t.midi.push_back (t.lastMidi);
            t.amp.push_back (0.0f);
            t.sal.push_back (0.0f);
            t.present.push_back (false);
        }
        t.midi.push_back (d.midi);
        t.amp.push_back (d.amp);
        t.sal.push_back (d.salience);
        t.present.push_back (true);
        t.lastMidi = d.midi;
        t.lastFrame = frame;
    }

    /** Splits a continuous pitch track into notes: at semitone changes of the vibrato-centred
        pitch (legato lines) and at re-attacks (re-plucked strings / re-sung syllables). */
    void segmentTrack (const Track& t, int minLen, const std::vector<bool>& onsets, std::vector<Note>& out, int& nextId)
    {
        const int n = (int) t.midi.size();
        if (n < minLen) return;

        // Vibrato-centred pitch: ~200 ms moving average (one vibrato cycle).
        std::vector<float> centre ((size_t) n);
        const int half = 10;
        for (int i = 0; i < n; ++i)
        {
            double s = 0.0; int c = 0;
            for (int k = juce::jmax (0, i - half); k <= juce::jmin (n - 1, i + half); ++k)
                if (t.present[(size_t) k]) { s += t.midi[(size_t) k]; ++c; }
            centre[(size_t) i] = c > 0 ? (float) (s / c) : t.midi[(size_t) i];
        }

        std::vector<int> cuts { 0 };
        // Semitone changes held for >= minLen frames
        int curSemi = (int) std::lround (centre[0]);
        int runStart = 0;
        for (int i = 1; i < n; ++i)
        {
            const int s = (int) std::lround (centre[(size_t) i]);
            if (s != curSemi)
            {
                // confirm the new semitone persists
                int k = i;
                while (k < n && (int) std::lround (centre[(size_t) k]) == s) ++k;
                if (k - i >= minLen && i - runStart >= minLen)
                {
                    cuts.push_back (i);
                    runStart = i;
                }
                curSemi = s;
            }
        }

        // Re-attacks: only at a global spectral onset, and only if this note's own level
        // rises there (vibrato tremolo doesn't produce onsets).
        for (int i = 4; i < n - minLen; ++i)
        {
            const int f = t.start + i;
            bool nearOnset = false;
            for (int k = f - 2; k <= f + 2; ++k)
                if (k >= 0 && k < (int) onsets.size() && onsets[(size_t) k]) nearOnset = true;
            if (! nearOnset) continue;

            float before = 0.0f, after = 0.0f;
            for (int k = i - 4; k < i - 1; ++k) before = juce::jmax (before, t.amp[(size_t) k]);
            for (int k = i; k < juce::jmin (n, i + 4); ++k) after = juce::jmax (after, t.amp[(size_t) k]);
            if (after > 1.6f * before && i - cuts.back() >= minLen)
            {
                cuts.push_back (i);
                i += minLen;
            }
        }
        std::sort (cuts.begin(), cuts.end());
        cuts.push_back (n);

        for (size_t c = 0; c + 1 < cuts.size(); ++c)
        {
            const int a = cuts[c], b = cuts[c + 1];
            if (b - a < minLen) continue;
            Note note;
            note.id = nextId++;
            note.startFrame = t.start + a;
            note.endFrame = t.start + b;
            std::vector<float> ps;
            double ampSum = 0.0; int ampN = 0;
            for (int i = a; i < b; ++i)
            {
                note.pitchMidi.push_back (t.midi[(size_t) i]);
                note.amp.push_back (t.amp[(size_t) i]);
                if (t.present[(size_t) i]) { ps.push_back (centre[(size_t) i]); ampSum += t.amp[(size_t) i]; ++ampN; }
            }
            if (ps.empty()) continue;
            std::nth_element (ps.begin(), ps.begin() + (long) ps.size() / 2, ps.end());
            note.midi = ps[ps.size() / 2];
            note.level = ampN > 0 ? (float) (ampSum / ampN) : 0.0f;
            out.push_back (std::move (note));
        }
    }
    /** Peak-picks a spectral-flux onset function with an adaptive (median) threshold. */
    std::vector<bool> pickOnsets (const std::vector<float>& flux)
    {
        const int n = (int) flux.size();
        std::vector<bool> on ((size_t) n, false);
        if (n == 0) return on;
        const float maxFlux = *std::max_element (flux.begin(), flux.end());
        std::vector<float> win;
        for (int i = 1; i < n - 1; ++i)
        {
            win.clear();
            for (int k = juce::jmax (0, i - 15); k <= juce::jmin (n - 1, i + 15); ++k) win.push_back (flux[(size_t) k]);
            std::nth_element (win.begin(), win.begin() + (long) win.size() / 2, win.end());
            const float thresh = 1.8f * win[win.size() / 2] + 0.06f * maxFlux;
            bool isMax = true;
            for (int k = juce::jmax (0, i - 3); k <= juce::jmin (n - 1, i + 3); ++k)
                if (flux[(size_t) k] > flux[(size_t) i]) isMax = false;
            if (isMax && flux[(size_t) i] > thresh) on[(size_t) i] = true;
        }
        return on;
    }

    /** Note-level clean-up:
        - attack blips: very short notes that are much weaker than what's sounding around them
        - overtone ghosts: a note sitting on an integer harmonic of a louder concurrent note,
          too weak to be a real voice (a real doubling carries its own energy). */
    void pruneNotes (std::vector<Note>& notes, const AnalyzerSettings& settings)
    {
        constexpr int   shortLen   = 11;     // frames (110 ms)
        constexpr float shortRatio = 0.45f;
        constexpr float ghostTol   = 0.35f;  // semitones from an exact harmonic
        const float ghostK = 0.9f * juce::jmap (settings.sensitivity, 0.0f, 1.0f, 1.3f, 0.7f);

        auto overlap = [] (const Note& a, const Note& b)
        {
            return juce::jmax (0, juce::jmin (a.endFrame, b.endFrame) - juce::jmax (a.startFrame, b.startFrame));
        };

        // Mean level of `n` over the frames it shares with `other`
        auto levelDuring = [] (const Note& n, const Note& other)
        {
            const int a = juce::jmax (n.startFrame, other.startFrame), b = juce::jmin (n.endFrame, other.endFrame);
            double s = 0.0; int c = 0;
            for (int f = a; f < b; ++f) { s += n.amp[(size_t) (f - n.startFrame)]; ++c; }
            return c > 0 ? (float) (s / c) : 0.0f;
        };

        std::vector<bool> drop (notes.size(), false);

        // Strongest first, so ghosts are judged against real notes.
        std::vector<size_t> order (notes.size());
        std::iota (order.begin(), order.end(), 0);
        std::sort (order.begin(), order.end(), [&] (size_t a, size_t b) { return notes[a].level > notes[b].level; });

        for (size_t oi = 0; oi < order.size(); ++oi)
        {
            const size_t i = order[oi];
            auto& n = notes[i];
            const int len = n.endFrame - n.startFrame;

            for (size_t oj = 0; oj < oi && ! drop[i]; ++oj)
            {
                const size_t j = order[oj];
                if (drop[j]) continue;
                const auto& l = notes[j];
                const int ov = overlap (n, l);
                if (ov < len / 2) continue;

                const float ln = levelDuring (n, l), ll = levelDuring (l, n);

                // Short weak blip next to a sustained, louder note
                if (len < shortLen && ln < shortRatio * ll)
                    drop[i] = true;

                // Overtone ghost of a lower, louder note
                if (l.midi < n.midi - 0.5f)
                {
                    const float h = std::exp2 ((n.midi - l.midi) / 12.0f);
                    const int hr = (int) std::lround (h);
                    if (hr >= 2 && hr <= 16 && std::abs (12.0f * std::log2 (h / (float) hr)) < ghostTol)
                        if (ln < ghostK * ll / std::sqrt ((float) hr))
                            drop[i] = true;
                }
            }
        }

        std::vector<Note> kept;
        for (size_t i = 0; i < notes.size(); ++i)
            if (! drop[i]) kept.push_back (std::move (notes[i]));
        notes = std::move (kept);
    }
} // namespace

//==============================================================================
Analysis analyse (const float* mono, int numSamples, double sampleRate, const AnalyzerSettings& settings,
                  const std::function<bool()>& shouldCancel, const std::function<void (float)>& progress)
{
    Analysis result;
    result.sampleRate = sampleRate;
    result.hopSize = juce::roundToInt (sampleRate * 0.01);
    result.numFrames = numSamples / result.hopSize + 1;
    if (numSamples <= 0) return result;

    FrameAnalyzer fa (sampleRate, settings);

    // Pass 1: frame peaks + silence gate relative to the loudest frame of the clip
    std::vector<std::vector<FrameF0>> frames ((size_t) result.numFrames);
    std::vector<float> frameMax ((size_t) result.numFrames, 0.0f);
    std::vector<float> flux ((size_t) result.numFrames, 0.0f);
    float clipMax = 0.0f;
    for (int f = 0; f < result.numFrames; ++f)
    {
        frameMax[(size_t) f] = fa.computePeaks (mono, numSamples, f * result.hopSize);
        flux[(size_t) f] = fa.getLastFlux();
        clipMax = juce::jmax (clipMax, frameMax[(size_t) f]);
        if (shouldCancel && (f & 63) == 0 && shouldCancel()) return result;
    }
    const float gate = clipMax * 0.006f; // -44 dB below the loudest frame
    const auto onsets = pickOnsets (flux);

    for (int f = 0; f < result.numFrames; ++f)
    {
        if (frameMax[(size_t) f] < gate) continue;
        fa.computePeaks (mono, numSamples, f * result.hopSize);
        fa.estimate (frames[(size_t) f]);
        if ((f & 31) == 0)
        {
            if (shouldCancel && shouldCancel()) return result;
            if (progress) progress (0.9f * (float) f / (float) result.numFrames);
        }
    }

    // Pass 2: tracking
    constexpr int maxGap = 4;
    constexpr int minLen = 6;
    constexpr float linkTol = 0.55f; // semitones between consecutive frames
    std::vector<Track> tracks;
    std::vector<Track> finished;

    for (int f = 0; f < result.numFrames; ++f)
    {
        auto& dets = frames[(size_t) f];
        std::sort (dets.begin(), dets.end(), [] (const FrameF0& a, const FrameF0& b) { return a.salience > b.salience; });
        std::vector<bool> claimed (tracks.size(), false);

        for (const auto& d : dets)
        {
            int best = -1;
            float bestDist = linkTol;
            for (size_t t = 0; t < tracks.size(); ++t)
            {
                if (claimed[t]) continue;
                const float dist = std::abs (tracks[t].lastMidi - d.midi);
                if (dist < bestDist) { bestDist = dist; best = (int) t; }
            }
            if (best >= 0)
            {
                claimed[(size_t) best] = true;
                appendToTrack (tracks[(size_t) best], f, d);
            }
            else
            {
                Track t;
                t.start = f;
                t.lastMidi = d.midi;
                appendToTrack (t, f, d);
                tracks.push_back (std::move (t));
                claimed.push_back (true);
            }
        }

        for (size_t t = 0; t < tracks.size();)
        {
            if (f - tracks[t].lastFrame > maxGap)
            {
                // trim trailing held frames
                auto& tr = tracks[t];
                while (! tr.present.empty() && ! tr.present.back())
                {
                    tr.present.pop_back(); tr.midi.pop_back(); tr.amp.pop_back(); tr.sal.pop_back();
                }
                finished.push_back (std::move (tr));
                tracks.erase (tracks.begin() + (long) t);
            }
            else ++t;
        }
    }
    for (auto& t : tracks)
    {
        while (! t.present.empty() && ! t.present.back())
        {
            t.present.pop_back(); t.midi.pop_back(); t.amp.pop_back(); t.sal.pop_back();
        }
        finished.push_back (std::move (t));
    }

    int nextId = 1;
    std::vector<Note> notes;
    for (const auto& t : finished)
        segmentTrack (t, minLen, onsets, notes, nextId);

    pruneNotes (notes, settings);
    result.notes = std::move (notes);

    std::sort (result.notes.begin(), result.notes.end(),
               [] (const Note& a, const Note& b) { return a.startFrame != b.startFrame ? a.startFrame < b.startFrame : a.midi < b.midi; });
    if (progress) progress (1.0f);
    return result;
}
} // namespace Poly
