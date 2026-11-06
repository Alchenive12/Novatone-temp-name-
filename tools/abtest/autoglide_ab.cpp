// Auto Glide A/B: move one sung note +1 semitone, render with default ramps (0.18) vs Auto Glide ramps.
// Per-hop pitch curve follows PitchTraceComponent's ramp formula (simplified: no humanize drift).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "TuneEngine.h"
static void writeWav(const juce::File& f, const juce::AudioBuffer<float>& b, double sr) {
    f.deleteFile(); juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(f), sr, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}
struct Note { int s, e; float det; int target; float rin = 0.18f, rout = 0.18f; bool edited = false; };
int main(int, char** argv) {
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(argv[1])));
    const double sr = r->sampleRate; const int n = (int) r->lengthInSamples;
    juce::AudioBuffer<float> in((int) r->numChannels, n); r->read(&in, 0, n, 0, true, true);
    juce::AudioBuffer<float> mono(1, n); mono.clear();
    for (int c = 0; c < in.getNumChannels(); ++c) mono.addFrom(0, 0, in, c, 0, n, 1.0f / in.getNumChannels());
    const int H = 512, W = 2048, hops = n / H;
    std::vector<float> midi(hops, -1);
    MPMDetector d; d.prepare(sr, W);
    for (int h = 0; h < hops; ++h) { int s = h * H; if (s + W > n) break;
        auto res = d.processFrame(mono.getReadPointer(0, s)); if (res.pitchHz > 70 && res.clarity > 0.8f) midi[h] = 69 + 12 * std::log2(res.pitchHz / 440); }
    // segment: runs of voiced hops on the same nearest semitone, >= 6 hops
    std::vector<Note> notes; std::vector<int> owner(hops, -1);
    for (int h = 0; h < hops;) { if (midi[h] < 0) { ++h; continue; }
        int k = (int) std::lround(midi[h]), e = h; float sum = 0;
        while (e < hops && midi[e] >= 0 && std::abs(midi[e] - k) < 0.7f) sum += midi[e++];
        if (e - h >= 6) { notes.push_back({h, e, sum / (e - h), k}); for (int i = h; i < e; ++i) owner[i] = (int) notes.size() - 1; }
        h = std::max(e, h + 1); }
    // pick the longest note that touches both neighbours and move it +1
    int pick = -1, best = 0;
    for (int i = 1; i + 1 < (int) notes.size(); ++i)
        if (notes[i].s == notes[i-1].e && notes[i].e == notes[i+1].s && notes[i].e - notes[i].s > best) { best = notes[i].e - notes[i].s; pick = i; }
    if (pick < 0) { printf("no touching note found (%zu notes)\n", notes.size()); return 1; }
    printf("%zu notes; moved note %d (%.2fs-%.2fs) %d -> %d\n", notes.size(), pick, notes[pick].s*H/sr, notes[pick].e*H/sr, notes[pick].target, notes[pick].target+1);
    const float center = 0.75f, transitionScale = (1.0f - 0.65f) * 1.15f;
    for (int variant = 0; variant < 2; ++variant) {
        auto nv = notes; nv[pick].target += 1; nv[pick].edited = true;
        if (variant == 1) { // same rule as PitchTraceComponent::applyAutoGlide
            const double hopMs = 1000.0 * H / sr;
            for (int k = pick; k <= pick + 1; ++k) { auto& a = nv[k-1]; auto& b = nv[k];
                float jump = std::abs((float) b.target - (float) a.target); if (jump < 0.5f) continue;
                double half = 0.5 * (100.0 + 15.0 * (std::min(jump, 7.0f) - 1.0f)) / hopMs / ((1.0f - 0.65f) * 1.15f);
                a.rout = juce::jlimit(0.18f, 0.48f, (float)(half / (a.e - a.s))); b.rin = juce::jlimit(0.18f, 0.48f, (float)(half / (b.e - b.s))); } }
        auto pitchOf = [&](const Note& x) { return x.det + center * (x.target - x.det); };
        std::vector<float> curve(hops, -1);
        for (int i = 0; i < (int) nv.size(); ++i) { auto& x = nv[i]; int len = x.e - x.s;
            float rin = len * x.rin * transitionScale, rout = len * x.rout * transitionScale;
            for (int h = x.s; h < x.e; ++h) { float sh = pitchOf(x) + (midi[h] - x.det); // keep the sung movement
                int rel = h - x.s, fromEnd = x.e - 1 - h;
                if (rin >= 0.5f && rel < rin && x.s > 0 && owner[x.s-1] >= 0) { float ph = (rel + 0.5f) / rin, w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * ph), an = 0.5f * (pitchOf(nv[owner[x.s-1]]) + sh); sh = an + w * (sh - an); }
                if (rout >= 0.5f && fromEnd < rout && x.e < hops && owner[x.e] >= 0) { float ph = (fromEnd + 0.5f) / rout, w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * ph), an = 0.5f * (pitchOf(nv[owner[x.e]]) + sh); sh = an + w * (sh - an); }
                curve[h] = sh; } }
        TuneEngine te; te.prepare({ sr, (juce::uint32) H, 1 });
        te.setScale(0x0FFF); te.setLookaheadEnabled(true); te.setNaturalOnly(true);
        te.setFlexAmount(0.65f); te.setTransitionAmount(0.65f); te.setSensitivityAmount(0.65f);
        te.setCenterAmount(center); te.setVariationAmount(1.0f); te.setHumanizeAmount(1.0f); te.setHumanizeEnabled(true);
        juce::AudioBuffer<float> y; y.makeCopyOf(mono); juce::dsp::AudioBlock<float> blk(y);
        for (int h = 0; h < hops; ++h) {
            if (curve[h] >= 0) te.setLiveNoteOverride(true, (float) nv[owner[h]].target, 1.0f, 1.0f, 0.0f, nv[owner[h]].rin, 0.0f, true, curve[h], 1.0f, nv[owner[h]].rout, true);
            else te.setLiveNoteOverride(false, 60, 1, 1, 0, 0.18f);
            auto sb = blk.getSubBlock((size_t) h * H, H); te.process(juce::dsp::ProcessContextReplacing<float>(sb)); }
        const int lat = te.getLatencySamples(); auto* q = y.getWritePointer(0);
        std::memmove(q, q + lat, sizeof(float) * (size_t)(n - lat)); std::fill(q + n - lat, q + n, 0.0f);
        // trim to 1.5 s either side of the moved note so it's easy to hear
        int a = std::max(0, nv[pick].s * H - (int)(1.5 * sr)), b = std::min(n, nv[pick].e * H + (int)(1.5 * sr));
        juce::AudioBuffer<float> cut(1, b - a); cut.copyFrom(0, 0, y, 0, a, b - a);
        writeWav(juce::File(argv[2]).getChildFile(variant ? "G2_autoglide_on.wav" : "G1_autoglide_off.wav"), cut, sr);
        if (!variant) { juce::AudioBuffer<float> dc(1, b - a); dc.copyFrom(0, 0, mono, 0, a, b - a); writeWav(juce::File(argv[2]).getChildFile("G0_dry_snippet.wav"), dc, sr); }
        printf("variant %d: ramps prev-out %.2f  in %.2f  out %.2f  next-in %.2f\n", variant, nv[pick-1].rout, nv[pick].rin, nv[pick].rout, nv[pick+1].rin);
    }
}
