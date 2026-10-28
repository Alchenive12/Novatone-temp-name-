#include "BasicPitch.h"
#include "BasicPitchWeights.h"
#include <JuceHeader.h>
#include <cmath>

namespace Poly
{
namespace BasicPitchDetail
{
namespace W = BasicPitchWeights;
constexpr int bins = 36, taps = 256, octaves = 9, cqtBins = 309, featBins = 264, notes = 88;

/** Valid 1-D correlation: out[k * outStride + t] for C kernels of 256 taps, stride s, first T frames. */
static void corr (const std::vector<float>& x, const float* k, int C, int s, int T, float* out)
{
    for (int c = 0; c < C; ++c)
    {
        const float* kc = k + c * taps;
        for (int t = 0; t < T; ++t)
        {
            const float* xs = x.data() + (size_t) t * (size_t) s;
            float acc = 0.0f;
            for (int i = 0; i < taps; ++i) acc += xs[i] * kc[i];
            out[c * T + t] = acc;
        }
    }
}

static std::vector<float> reflectPad (const std::vector<float>& x, int p)
{
    const int n = (int) x.size();
    std::vector<float> y ((size_t) (n + 2 * p));
    for (int i = 0; i < n + 2 * p; ++i)
    {
        int j = i - p;
        if (j < 0) j = -j;
        if (j >= n) j = 2 * (n - 1) - j;
        y[(size_t) i] = x[(size_t) j];
    }
    return y;
}

/** Stacked harmonic CQT features (8 x 172 x 264), exactly as the stock ONNX front end. */
static void features (const float* audio, std::vector<float>& feat)
{
    // per-octave CQT, highest octave first (stride 256), then low-pass + decimate by 2
    std::vector<float> mag ((size_t) frames * cqtBins);                 // (172, 309), bins low -> high
    std::vector<float> cur (audio, audio + windowSamples), re ((size_t) bins * frames), im ((size_t) bins * frames);
    for (int o = 0; o < octaves; ++o)
    {
        const auto xp = reflectPad (cur, 128);
        corr (xp, W::cqtRe, bins, 256 >> o, frames, re.data());
        corr (xp, W::cqtIm, bins, 256 >> o, frames, im.data());
        const int binOffset = (octaves - 1 - o) * bins - (octaves * bins - cqtBins);   // concat low..high, keep last 309
        for (int b = 0; b < bins; ++b)
        {
            const int ob = binOffset + b;
            if (ob < 0) continue;
            const float sc = W::binScale[ob];
            for (int t = 0; t < frames; ++t)
            {
                const float r = re[(size_t) (b * frames + t)] * sc, i = im[(size_t) (b * frames + t)] * sc;
                mag[(size_t) (t * cqtBins + ob)] = std::sqrt (r * r + i * i);
            }
        }
        if (o + 1 < octaves)
        {
            std::vector<float> zp (cur.size() + 254, 0.0f);
            std::copy (cur.begin(), cur.end(), zp.begin() + 127);
            const int n = ((int) zp.size() - taps) / 2 + 1;
            std::vector<float> next ((size_t) n);
            corr (zp, W::lowpass, 1, 2, n, next.data());
            cur.swap (next);
        }
    }
    // 10 log10(power), min/max normalise over the window, batch norm
    float lo = 1e30f, hi = -1e30f;
    for (auto& v : mag) { v = 10.0f * std::log10 (v * v + 1e-10f); lo = std::min (lo, v); hi = std::max (hi, v); }
    const float range = hi - lo;
    for (auto& v : mag) v = (range != 0.0f ? (v - lo) / range : v - lo) * 2.480741024017334f - 0.8769183158874512f;
    // harmonic stacking: sub-harmonic, 1..7 (shifts in CQT bins), cropped to 264 bins
    static constexpr int shifts[8] = { -36, 0, 36, 57, 72, 84, 93, 101 };
    feat.assign ((size_t) 8 * frames * featBins, 0.0f);
    for (int h = 0; h < 8; ++h)
        for (int t = 0; t < frames; ++t)
            for (int f = 0; f < featBins; ++f)
            {
                const int src = f + shifts[h];
                if (src >= 0 && src < cqtBins) feat[((size_t) h * frames + (size_t) t) * featBins + (size_t) f] = mag[(size_t) (t * cqtBins + src)];
            }
}

/** 2-D conv, 'same'-style explicit pads, x (Ci, T, Fi) -> y (Co, T, Fo); time stride 1, freq stride sf. */
static void conv2d (const std::vector<float>& x, int Ci, int Fi, const float* Wt, const float* B, int Co, int kt, int kf,
                    int padT, int padL, int sf, int Fo, std::vector<float>& y, bool relu)
{
    const int T = frames;
    y.assign ((size_t) Co * T * Fo, 0.0f);
    for (int o = 0; o < Co; ++o)
    {
        float* yo = y.data() + (size_t) o * T * Fo;
        for (int i = 0; i < T * Fo; ++i) yo[i] = B[o];
        for (int c = 0; c < Ci; ++c)
        {
            const float* xc = x.data() + (size_t) c * T * Fi;
            for (int i = 0; i < kt; ++i)
                for (int j = 0; j < kf; ++j)
                {
                    const float w = Wt[((o * Ci + c) * kt + i) * kf + j];
                    if (w == 0.0f) continue;
                    for (int t = 0; t < T; ++t)
                    {
                        const int ti = t + i - padT;
                        if (ti < 0 || ti >= T) continue;
                        const float* xr = xc + (size_t) ti * Fi;
                        float* yr = yo + (size_t) t * Fo;
                        // input freq index = f * sf + j - padL, valid range
                        const int f0 = std::max (0, (padL - j + sf - 1) / sf), f1 = std::min (Fo, (Fi - 1 - j + padL) / sf + 1);
                        if (sf == 1) { const float* xs = xr + j - padL; for (int f = f0; f < f1; ++f) yr[f] += w * xs[f]; }
                        else for (int f = f0; f < f1; ++f) yr[f] += w * xr[f * sf + j - padL];
                    }
                }
        }
        if (relu) for (int i = 0; i < T * Fo; ++i) yo[i] = std::max (0.0f, yo[i]);
    }
}

static inline float sigm (float z) { return 1.0f / (1.0f + std::exp (-z)); }

void runWindow (const float* audio22k, float* note, float* onset, float* contour)
{
    std::vector<float> feat, on1, c1, cont, n1, nt, cat, on;
    features (audio22k, feat);
    conv2d (feat, 8, featBins, W::onset1W, W::onset1B, 32, 5, 5, 2, 1, 3, notes, on1, true);
    conv2d (feat, 8, featBins, W::cont1W, W::cont1B, 8, 3, 39, 1, 19, 1, featBins, c1, true);
    conv2d (c1, 8, featBins, W::cont2W, W::cont2B, 1, 5, 5, 2, 2, 1, featBins, cont, false);
    for (auto& v : cont) v = sigm (v);
    conv2d (cont, 1, featBins, W::note1W, W::note1B, 32, 7, 7, 3, 2, 3, notes, n1, true);
    conv2d (n1, 32, notes, W::note2W, W::note2B, 1, 7, 3, 3, 1, 1, notes, nt, false);
    for (auto& v : nt) v = sigm (v);
    cat.resize ((size_t) 33 * frames * notes);
    std::copy (nt.begin(), nt.end(), cat.begin());
    std::copy (on1.begin(), on1.end(), cat.begin() + (long) nt.size());
    conv2d (cat, 33, notes, W::onset2W, W::onset2B, 1, 3, 3, 1, 1, 1, notes, on, false);
    for (int i = 0; i < frames * notes; ++i) { note[i] = nt[(size_t) i]; onset[i] = sigm (on[(size_t) i]); }
    std::copy (cont.begin(), cont.end(), contour);
}
} // namespace BasicPitchDetail

PitchPrior basicPitchPrior (const float* mono, int numSamples, double sampleRate, std::function<bool()> shouldExit, PitchPrior* onsetsOut)
{
    using namespace BasicPitchDetail;
    PitchPrior prior;
    if (numSamples <= 0 || sampleRate <= 0) return prior;
    // resample to 22050 Hz
    std::vector<float> a;
    if (std::abs (sampleRate - 22050.0) < 0.5) a.assign (mono, mono + numSamples);
    else
    {
        const double ratio = sampleRate / 22050.0;
        a.resize ((size_t) std::floor (numSamples / ratio));
        juce::WindowedSincInterpolator interp;
        std::vector<float> in (mono, mono + numSamples);
        in.resize (in.size() + 512, 0.0f);
        interp.process (ratio, in.data(), a.data(), (int) a.size());
    }
    // windows like basic_pitch.inference: pad half an overlap in front, hop = window - overlap, trim 15 frames per side
    const int ov = overlapFrames * hop, step = windowSamples - ov, pad = ov / 2, keep = frames - overlapFrames;
    const int n = (int) a.size(), total = n / hop + 1;
    std::vector<float> x ((size_t) (pad + n + windowSamples), 0.0f);
    std::copy (a.begin(), a.end(), x.begin() + pad);
    prior.note.assign ((size_t) total * notes, 0.0f);
    std::vector<float> on ((size_t) total * notes, 0.0f);
    std::vector<float> nb ((size_t) frames * notes), ob ((size_t) frames * notes), cb ((size_t) frames * featBins);
    int outF = 0;
    for (int s = 0; s < n + pad && outF < total; s += step)
    {
        if (shouldExit && shouldExit()) return {};
        runWindow (x.data() + s, nb.data(), ob.data(), cb.data());
        for (int f = overlapFrames / 2; f < overlapFrames / 2 + keep && outF < total; ++f, ++outF)
            for (int m = 0; m < notes; ++m)
            {
                prior.note[(size_t) (outF * notes + m)] = nb[(size_t) (f * notes + m)];
                on[(size_t) (outF * notes + m)] = ob[(size_t) (f * notes + m)];
            }
    }
    prior.frames = outF;
    prior.fps = 22050.0f / (float) hop;
    prior.note.resize ((size_t) outF * notes);
    if (onsetsOut != nullptr) { onsetsOut->frames = outF; onsetsOut->fps = prior.fps; on.resize ((size_t) outF * notes); onsetsOut->note = std::move (on); }
    return prior;
}
} // namespace Poly
