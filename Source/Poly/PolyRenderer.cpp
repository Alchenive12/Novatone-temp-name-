#include "PolyRenderer.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace Poly
{
namespace
{
    inline float midiToHz (float m) noexcept { return 440.0f * std::exp2 ((m - 69.0f) / 12.0f); }

    constexpr int maxHarmonics = 24;

    //==========================================================================
    /** Hann-windowed, 2x zero-padded magnitude spectra at analysis-frame centres, cached. */
    class PeakSpectra
    {
    public:
        PeakSpectra (const float* x, int n, double sr, int hop)
            : data (x), numSamples (n), sampleRate (sr), hopSize (hop)
        {
            winLen = juce::nextPowerOfTwo (juce::roundToInt (sr * 0.08));
            fftSize = winLen * 2;
            int order = 0; while ((1 << order) < fftSize) ++order;
            fft = std::make_unique<juce::dsp::FFT> (order);
            window.resize ((size_t) winLen);
            for (int i = 0; i < winLen; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) winLen);
            binHz = (float) (sr / fftSize);
            scale = 4.0f / (float) winLen;
        }

        float getBinHz() const noexcept { return binHz; }

        const std::vector<float>& frame (int f)
        {
            auto it = cache.find (f);
            if (it != cache.end()) return it->second;
            std::vector<float> buf ((size_t) fftSize * 2, 0.0f);
            const int start = f * hopSize - winLen / 2;
            for (int i = 0; i < winLen; ++i)
            {
                const int s = start + i;
                if (s >= 0 && s < numSamples) buf[(size_t) i] = data[s] * window[(size_t) i];
            }
            fft->performFrequencyOnlyForwardTransform (buf.data(), true);
            buf.resize ((size_t) fftSize / 2 + 1);
            for (auto& v : buf) v *= scale;
            return cache.emplace (f, std::move (buf)).first->second;
        }

        /** Strongest interpolated peak within +-tol of hz; returns false if none. */
        bool findPeak (int f, float hz, float& outHz, float& outAmp)
        {
            const auto& mag = frame (f);
            const float tol = juce::jmax (2.0f * binHz, hz * 0.025f);
            const int lo = juce::jmax (2, (int) std::floor ((hz - tol) / binHz));
            const int hi = juce::jmin ((int) mag.size() - 3, (int) std::ceil ((hz + tol) / binHz));
            int best = -1; float bestMag = 0.0f;
            for (int k = lo; k <= hi; ++k)
                if (mag[(size_t) k] > mag[(size_t) k - 1] && mag[(size_t) k] >= mag[(size_t) k + 1] && mag[(size_t) k] > bestMag)
                { bestMag = mag[(size_t) k]; best = k; }
            if (best < 0) return false;
            const float l1 = std::log (juce::jmax (1.0e-12f, mag[(size_t) best - 1]));
            const float l0 = std::log (juce::jmax (1.0e-12f, mag[(size_t) best]));
            const float r1 = std::log (juce::jmax (1.0e-12f, mag[(size_t) best + 1]));
            const float den = l1 - 2.0f * l0 + r1;
            const float d = den < -1.0e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (l1 - r1) / den) : 0.0f;
            outHz = ((float) best + d) * binHz;
            outAmp = std::exp (l0 - 0.25f * (l1 - r1) * d);
            return true;
        }

    private:
        const float* data;
        int numSamples;
        double sampleRate;
        int hopSize, winLen = 4096, fftSize = 8192;
        float binHz = 1.0f, scale = 1.0f;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window;
        std::map<int, std::vector<float>> cache;
    };

    //==========================================================================
    /** Partial table of one note over [f0, f1) analysis frames. */
    struct NotePartials
    {
        const Note* note = nullptr;
        int f0 = 0, f1 = 0;
        int numH = 0;
        std::vector<float> pitchHz;                 // per frame
        std::vector<std::array<float, maxHarmonics>> freq, amp, share;

        int frames() const noexcept { return f1 - f0; }
        bool covers (int f) const noexcept { return f >= f0 && f < f1; }
    };

    float notePitchHzAt (const Note& n, int f)
    {
        const int i = juce::jlimit (0, (int) n.pitchMidi.size() - 1, f - n.startFrame);
        return midiToHz (n.pitchMidi[(size_t) i]);
    }

    /** Frames past the detected end where the note is still audibly decaying (ringing strings,
        vocal releases); stops at -40 dB below the note's level or when the same pitch starts again. */
    int tailFrames (const Note& n, const Analysis& an, PeakSpectra& spec, double sr)
    {
        const float hz = midiToHz (n.midi);
        int nextSame = an.numFrames;
        for (auto& o : an.notes)
            if (&o != &n && o.startFrame >= n.endFrame - 2 && std::abs (o.midi - n.midi) < 0.5f)
                nextSame = juce::jmin (nextSame, o.startFrame);

        auto level = [&] (int f)
        {
            float sum = 0.0f;
            for (int m = 1; m <= 6 && hz * (float) m < (float) sr * 0.45f; ++m)
            {
                float pf, pa;
                if (spec.findPeak (f, hz * (float) m, pf, pa)) sum += pa;
            }
            return sum;
        };
        float ref = 0.0f;
        for (int f = juce::jmax (n.startFrame, n.endFrame - 8); f < n.endFrame; ++f) ref = juce::jmax (ref, level (f));
        int extra = 0;
        const int limit = juce::jmin (nextSame - 1, juce::jmin (an.numFrames, n.endFrame + 300));
        for (int f = n.endFrame; f < limit; ++f)
        {
            if (level (f) < ref * 0.01f) break;
            ++extra;
        }
        return extra;
    }

    NotePartials trackPartials (const Note& n, int margin, int numFrames, PeakSpectra& spec, double sr, int tail = 0)
    {
        NotePartials p;
        p.note = &n;
        p.f0 = juce::jmax (0, n.startFrame - margin);
        p.f1 = juce::jmin (numFrames, n.endFrame + tail + margin);
        const float lowestHz = midiToHz (n.midi) * 0.9f;
        p.numH = juce::jlimit (1, maxHarmonics, (int) std::floor (juce::jmin (9000.0f, (float) sr * 0.45f) / lowestHz));
        const int nf = p.frames();
        p.pitchHz.resize ((size_t) nf);
        p.freq.resize ((size_t) nf); p.amp.resize ((size_t) nf); p.share.resize ((size_t) nf);

        for (int i = 0; i < nf; ++i)
        {
            const int f = p.f0 + i;
            const float hz = notePitchHzAt (n, f);
            p.pitchHz[(size_t) i] = hz;
            for (int m = 1; m <= p.numH; ++m)
            {
                float fh = hz * (float) m, a = 0.0f;
                if (fh < (float) sr * 0.47f)
                {
                    float pf, pa;
                    if (spec.findPeak (f, fh, pf, pa)) { fh = pf; a = pa; }
                }
                p.freq[(size_t) i][(size_t) m - 1] = fh;
                p.amp[(size_t) i][(size_t) m - 1] = a;
                p.share[(size_t) i][(size_t) m - 1] = 1.0f;
            }
        }
        return p;
    }

    /** Is partial frequency `hz` also a harmonic of `other` at frame f? returns harmonic index or 0 */
    int sharedHarmonic (const NotePartials& other, int f, float hz, float binHz)
    {
        if (! other.covers (f)) return 0;
        const int i = f - other.f0;
        if (other.amp[(size_t) i][0] <= 0.0f && other.amp[(size_t) i][1] <= 0.0f) return 0; // other note silent here
        const float f0 = other.pitchHz[(size_t) i];
        const int k = (int) std::lround (hz / f0);
        if (k < 1 || k > other.numH) return 0;
        const float tol = juce::jmax (1.5f * binHz, hz * 0.02f);
        return std::abs (other.freq[(size_t) i][(size_t) k - 1] - hz) < tol ? k : 0;
    }

    /** Smooth envelope value of harmonic m at frame row i, ignoring partials flagged as shared. */
    float envelopeAt (const NotePartials& p, int i, int m, const std::array<bool, maxHarmonics>& shared)
    {
        if (! shared[(size_t) m - 1]) return p.amp[(size_t) i][(size_t) m - 1];
        int lo = m - 1, hi = m + 1;
        while (lo >= 1 && shared[(size_t) lo - 1]) --lo;
        while (hi <= p.numH && shared[(size_t) hi - 1]) ++hi;
        const bool hasLo = lo >= 1, hasHi = hi <= p.numH;
        if (! hasLo && ! hasHi)
        {
            // Every partial is shared: the spectrum can't tell us this note's envelope, so use
            // the level the analyser attributed to it (otherwise a faint note would claim
            // whole partials of a loud one).
            const auto& n = *p.note;
            const int f = juce::jlimit (n.startFrame, n.endFrame - 1, p.f0 + i);
            const float own = n.amp.empty() ? 0.0f : n.amp[(size_t) (f - n.startFrame)];
            return juce::jmin (p.amp[(size_t) i][(size_t) m - 1], own / std::sqrt ((float) m));
        }
        auto la = [&] (int h) { return std::log (juce::jmax (1.0e-9f, p.amp[(size_t) i][(size_t) h - 1])); };
        float v;
        if (hasLo && hasHi) { const float t = (float) (m - lo) / (float) (hi - lo); v = la (lo) + t * (la (hi) - la (lo)); }
        else v = hasLo ? la (lo) : la (hi);
        return juce::jmin (std::exp (v), p.amp[(size_t) i][(size_t) m - 1]);
    }

    void computeShares (std::vector<NotePartials>& all, float binHz)
    {
        // envelopes first (depends only on which partials are shared)
        std::vector<std::vector<std::array<float, maxHarmonics>>> env (all.size());
        for (size_t a = 0; a < all.size(); ++a)
        {
            auto& p = all[a];
            env[a].resize ((size_t) p.frames());
            for (int i = 0; i < p.frames(); ++i)
            {
                const int f = p.f0 + i;
                std::array<bool, maxHarmonics> shared {};
                for (int m = 1; m <= p.numH; ++m)
                    for (size_t b = 0; b < all.size() && ! shared[(size_t) m - 1]; ++b)
                        if (b != a && sharedHarmonic (all[b], f, p.freq[(size_t) i][(size_t) m - 1], binHz) > 0)
                            shared[(size_t) m - 1] = true;
                for (int m = 1; m <= p.numH; ++m)
                    env[a][(size_t) i][(size_t) m - 1] = envelopeAt (p, i, m, shared);
            }
        }
        for (size_t a = 0; a < all.size(); ++a)
        {
            auto& p = all[a];
            for (int i = 0; i < p.frames(); ++i)
            {
                const int f = p.f0 + i;
                for (int m = 1; m <= p.numH; ++m)
                {
                    const float mine = env[a][(size_t) i][(size_t) m - 1];
                    float others = 0.0f;
                    for (size_t b = 0; b < all.size(); ++b)
                    {
                        if (b == a) continue;
                        const int k = sharedHarmonic (all[b], f, p.freq[(size_t) i][(size_t) m - 1], binHz);
                        if (k > 0) others += env[b][(size_t) (f - all[b].f0)][(size_t) k - 1];
                    }
                    p.share[(size_t) i][(size_t) m - 1] = mine + others > 0.0f ? mine / (mine + others) : 1.0f;
                }
            }
        }
    }

    //==========================================================================
    /** Log-amplitude envelope over frequency from (freq, amp) points, for formant preservation. */
    float envelopeAtHz (const std::array<float, maxHarmonics>& fr, const std::array<float, maxHarmonics>& am, int n, float hz)
    {
        int lo = -1, hi = -1;
        for (int k = 0; k < n; ++k)
        {
            if (am[(size_t) k] <= 0.0f) continue;
            if (fr[(size_t) k] <= hz) lo = k;
            else { hi = k; break; }
        }
        if (lo < 0 && hi < 0) return 0.0f;
        if (lo < 0) return am[(size_t) hi];
        if (hi < 0)
        {
            // above the highest partial: roll off 12 dB/octave
            const float oct = std::log2 (hz / juce::jmax (1.0f, fr[(size_t) lo]));
            return am[(size_t) lo] * std::exp2 (-2.0f * oct);
        }
        const float t = std::log2 (hz / fr[(size_t) lo]) / std::log2 (fr[(size_t) hi] / fr[(size_t) lo]);
        return std::exp ((1.0f - t) * std::log (am[(size_t) lo]) + t * std::log (am[(size_t) hi]));
    }

    //==========================================================================
    void renderChannel (const float* x, int n, double sr, const Analysis& an,
                        const std::vector<NoteEdit>& edits, float* y)
    {
        const int hopA = an.hopSize;
        PeakSpectra spec (x, n, sr, hopA);
        const float binHz = spec.getBinHz();
        constexpr int margin = 3;

        // Edited notes, plus every note overlapping them in time (needed to split shared partials)
        std::vector<const Note*> edited;
        std::vector<const NoteEdit*> editOf;
        for (auto& e : edits)
            for (auto& note : an.notes)
                if (note.id == e.noteId && (std::abs (e.semitones) > 1.0e-4f || std::abs (e.gainDb) > 1.0e-3f || std::abs (e.formantSemitones) > 1.0e-4f))
                { edited.push_back (&note); editOf.push_back (&e); }
        if (edited.empty()) { std::copy (x, x + n, y); return; }

        std::vector<NotePartials> all;
        std::vector<int> editedIndex; // into `all`
        for (auto* e : edited)
        {
            editedIndex.push_back ((int) all.size());
            all.push_back (trackPartials (*e, margin, an.numFrames, spec, sr, tailFrames (*e, an, spec, sr)));
        }
        for (auto& note : an.notes)
        {
            if (std::find (edited.begin(), edited.end(), &note) != edited.end()) continue;
            bool overlaps = false;
            for (size_t e = 0; e < edited.size(); ++e)
                if (note.startFrame < all[(size_t) editedIndex[e]].f1 && note.endFrame > all[(size_t) editedIndex[e]].f0) overlaps = true;
            if (overlaps) all.push_back (trackPartials (note, margin, an.numFrames, spec, sr));
        }
        computeShares (all, binHz);

        //----------------------------------------------------------------------
        // Removal: STFT mask (sqrt-Hann, 75% overlap)
        const int N = juce::nextPowerOfTwo (juce::roundToInt (sr * 0.08));
        const int H = N / 4;
        int order = 0; while ((1 << order) < N) ++order;
        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) N);
        for (int i = 0; i < N; ++i)
            win[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) N));
        const float binHzM = (float) (sr / N);
        const int halfWinFrames = juce::roundToInt ((float) N * 0.5f / (float) hopA);

        // Pitch/formant edits are removed and resynthesised; gain-only edits just rescale the
        // note's own share of the spectrum, which keeps its original waveform (phase) intact.
        std::vector<bool> isShifted (edited.size());
        std::vector<float> editGain (edited.size());
        for (size_t e = 0; e < edited.size(); ++e)
        {
            isShifted[e] = std::abs (editOf[e]->semitones) > 1.0e-4f || std::abs (editOf[e]->formantSemitones) > 1.0e-4f;
            editGain[e] = juce::Decibels::decibelsToGain (editOf[e]->gainDb);
        }

        std::vector<float> residual ((size_t) n + (size_t) N, 0.0f);
        std::vector<float> buf ((size_t) N * 2);
        std::vector<float> mask ((size_t) N / 2 + 1);

        // Only process frames that touch edited regions; elsewhere residual == input (blended later)
        int regionLo = n, regionHi = 0;
        for (size_t e = 0; e < edited.size(); ++e)
        {
            regionLo = juce::jmin (regionLo, all[(size_t) editedIndex[e]].f0 * hopA);
            regionHi = juce::jmax (regionHi, all[(size_t) editedIndex[e]].f1 * hopA);
        }
        const int jLo = juce::jmax (0, (regionLo - N) / H), jHi = juce::jmin ((n + N) / H, (regionHi + N) / H + 1);

        for (int j = jLo; j <= jHi; ++j)
        {
            const int start = j * H - N / 2;
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const int s = start + i;
                if (s >= 0 && s < n) buf[(size_t) i] = x[s] * win[(size_t) i];
            }
            fft.performRealOnlyForwardTransform (buf.data(), true);

            std::fill (mask.begin(), mask.end(), 1.0f);
            const float fa = (float) (j * H) / (float) hopA;
            for (size_t e = 0; e < edited.size(); ++e)
            {
                const auto& p = all[(size_t) editedIndex[e]];
                if (fa < (float) p.f0 || fa > (float) (p.f1 - 1)) continue;
                const int i0 = juce::jlimit (0, p.frames() - 1, (int) std::floor (fa) - p.f0);
                const int i1 = juce::jmin (p.frames() - 1, i0 + 1);
                const float t = juce::jlimit (0.0f, 1.0f, fa - (float) (p.f0 + i0));
                for (int m = 1; m <= p.numH; ++m)
                {
                    const float a0 = p.amp[(size_t) i0][(size_t) m - 1], a1 = p.amp[(size_t) i1][(size_t) m - 1];
                    if (a0 <= 0.0f && a1 <= 0.0f) continue;
                    const float hz = p.freq[(size_t) i0][(size_t) m - 1] + t * (p.freq[(size_t) i1][(size_t) m - 1] - p.freq[(size_t) i0][(size_t) m - 1]);
                    float sh = p.share[(size_t) i0][(size_t) m - 1] + t * (p.share[(size_t) i1][(size_t) m - 1] - p.share[(size_t) i0][(size_t) m - 1]);
                    if (! isShifted[e]) sh *= (1.0f - editGain[e]); // gain-only: scale instead of remove
                    const float c = hz / binHzM;

                    // Widen the mask by how far this partial moves (vibrato, glides) inside the window
                    float spread = 0.0f;
                    for (int k = juce::jmax (0, i0 - halfWinFrames); k <= juce::jmin (p.frames() - 1, i1 + halfWinFrames); ++k)
                        if (p.amp[(size_t) k][(size_t) m - 1] > 0.0f)
                            spread = juce::jmax (spread, std::abs (p.freq[(size_t) k][(size_t) m - 1] - hz));
                    const float core = 1.6f + juce::jmin (6.0f, spread / binHzM);
                    const float edge = core + 1.4f;
                    for (int b = (int) std::floor (c - edge); b <= (int) std::ceil (c + edge); ++b)
                    {
                        if (b < 0 || b >= (int) mask.size()) continue;
                        const float d = std::abs ((float) b - c);
                        const float w = d <= core ? 1.0f : (d < edge ? 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (d - core) / 1.4f) : 0.0f);
                        mask[(size_t) b] *= (1.0f - sh * w);
                    }
                }
            }
            for (size_t b = 0; b < mask.size(); ++b)
            {
                buf[2 * b] *= mask[b];
                buf[2 * b + 1] *= mask[b];
            }
            fft.performRealOnlyInverseTransform (buf.data());
            for (int i = 0; i < N; ++i)
            {
                const int s = start + i;
                if (s >= 0 && s < n) residual[(size_t) s] += buf[(size_t) i] * win[(size_t) i] * 0.5f; // sqrt-Hann^2 at 75% overlap sums to 2
            }
        }

        //----------------------------------------------------------------------
        // Resynthesis of edited notes at their new pitch
        std::vector<float> synth ((size_t) n, 0.0f);
        for (size_t e = 0; e < edited.size(); ++e)
        {
            const auto& p = all[(size_t) editedIndex[e]];
            const auto& ed = *editOf[e];
            const float ratio = std::exp2 (ed.semitones / 12.0f);
            const float formantRatio = std::exp2 (ed.formantSemitones / 12.0f);
            const float gain = juce::Decibels::decibelsToGain (ed.gainDb);
            const bool shifted = isShifted[e];
            if (! shifted) continue; // gain-only edits were applied in the mask

            // Per frame: this note's own partial amplitudes (after sharing), and new freq/amp
            const int nf = p.frames();
            std::vector<std::array<float, maxHarmonics>> ownAmp ((size_t) nf), newF ((size_t) nf), newA ((size_t) nf);
            for (int i = 0; i < nf; ++i)
                for (int m = 1; m <= p.numH; ++m)
                    ownAmp[(size_t) i][(size_t) m - 1] = p.amp[(size_t) i][(size_t) m - 1] * p.share[(size_t) i][(size_t) m - 1];
            for (int i = 0; i < nf; ++i)
                for (int m = 1; m <= p.numH; ++m)
                {
                    const float f1 = p.freq[(size_t) i][(size_t) m - 1] * ratio;
                    newF[(size_t) i][(size_t) m - 1] = f1;
                    float a = ownAmp[(size_t) i][(size_t) m - 1];
                    if (shifted && a > 0.0f)
                        a = envelopeAtHz (p.freq[(size_t) i], ownAmp[(size_t) i], p.numH, f1 / formantRatio);
                    newA[(size_t) i][(size_t) m - 1] = f1 < (float) sr * 0.46f ? a * gain : 0.0f;
                }

            const int sStart = p.f0 * hopA, sEnd = juce::jmin (n, (p.f1 - 1) * hopA);
            for (int m = 1; m <= p.numH; ++m)
            {
                double phase = 0.0;
                for (int s = sStart; s < sEnd; ++s)
                {
                    const float fa = (float) s / (float) hopA - (float) p.f0;
                    const int i0 = juce::jlimit (0, nf - 1, (int) std::floor (fa));
                    const int i1 = juce::jmin (nf - 1, i0 + 1);
                    const float t = fa - (float) i0;
                    const float a = newA[(size_t) i0][(size_t) m - 1] + t * (newA[(size_t) i1][(size_t) m - 1] - newA[(size_t) i0][(size_t) m - 1]);
                    const float f = newF[(size_t) i0][(size_t) m - 1] + t * (newF[(size_t) i1][(size_t) m - 1] - newF[(size_t) i0][(size_t) m - 1]);
                    phase += juce::MathConstants<double>::twoPi * f / sr;
                    if (a > 0.0f) synth[(size_t) s] += a * (float) std::sin (phase);
                }
            }
        }

        //----------------------------------------------------------------------
        // Blend: processed inside edited regions (with 15 ms crossfades), original elsewhere
        std::vector<float> region ((size_t) n, 0.0f);
        const int fade = juce::roundToInt (sr * 0.015);
        for (size_t e = 0; e < edited.size(); ++e)
        {
            const auto& pe = all[(size_t) editedIndex[e]];
            const int a = juce::jmax (0, pe.f0 * hopA - fade), b = juce::jmin (n, pe.f1 * hopA + fade);
            for (int s = a; s < b; ++s)
            {
                const float r = juce::jmin (1.0f, juce::jmin ((float) (s - a) / (float) fade, (float) (b - s) / (float) fade));
                region[(size_t) s] = juce::jmax (region[(size_t) s], r);
            }
        }
        for (int s = 0; s < n; ++s)
        {
            const float r = region[(size_t) s];
            y[s] = r <= 0.0f ? x[s] : x[s] + r * ((residual[(size_t) s] + synth[(size_t) s]) - x[s]);
        }
    }
} // namespace

void render (const juce::AudioBuffer<float>& clip, double sampleRate, const Analysis& analysis,
             const std::vector<NoteEdit>& edits, juce::AudioBuffer<float>& out,
             const std::function<bool()>& shouldCancel, const std::function<void (float)>& progress)
{
    out.setSize (clip.getNumChannels(), clip.getNumSamples(), false, false, true);
    for (int ch = 0; ch < clip.getNumChannels(); ++ch)
    {
        if (shouldCancel && shouldCancel()) return;
        renderChannel (clip.getReadPointer (ch), clip.getNumSamples(), sampleRate, analysis, edits, out.getWritePointer (ch));
        if (progress) progress ((float) (ch + 1) / (float) clip.getNumChannels());
    }
}
} // namespace Poly
