// Full NovaTone mono correction (TuneEngine) on a vocal, three knob settings.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "TuneEngine.h"

static void writeWav(const juce::File& f, const juce::AudioBuffer<float>& b, double sr) {
    f.deleteFile(); juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(f), sr, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}
// median |cents off nearest semitone| over clearly voiced frames
static float centsErr(const juce::AudioBuffer<float>& x, double sr) {
    MPMDetector d; d.prepare(sr, 2048); std::vector<float> e;
    for (int s = 0; s + 2048 <= x.getNumSamples(); s += 512) {
        auto r = d.processFrame(x.getReadPointer(0, s));
        if (r.pitchHz > 70 && r.clarity > 0.9f) { float m = 69 + 12 * std::log2(r.pitchHz / 440); e.push_back(std::abs(m - std::round(m)) * 100); }
    }
    std::sort(e.begin(), e.end()); return e.empty() ? -1 : e[e.size() / 2];
}
struct Setting { const char* name; bool lookahead, smooth; float center, variation, humanize; bool sib = true; };
int main(int, char** argv) {
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(argv[1])));
    const double sr = r->sampleRate; const int n = (int) r->lengthInSamples;
    juce::AudioBuffer<float> in((int) r->numChannels, n); r->read(&in, 0, n, 0, true, true);
    juce::AudioBuffer<float> mono(1, n); mono.clear();
    for (int c = 0; c < in.getNumChannels(); ++c) mono.addFrom(0, 0, in, c, 0, n, 1.0f / in.getNumChannels());
    printf("dry: median %.1f cents off\n", centsErr(mono, sr));
    Setting set[] = { {"E2_center75", true, true, 0.75f, 1.0f, 1.0f}, {"E3_center60", true, true, 0.60f, 1.0f, 1.0f}, {"E4_center75_nophase", true, true, 0.75f, 1.0f, 1.0f, false} };
    const int B = 512; juce::File out(argv[2]);
    for (auto& s : set) {
        TuneEngine te; te.prepare({ sr, (juce::uint32) B, 1 });
        te.setScale(0x0FFF); te.setVoicedSibBlend(s.sib); te.setLookaheadEnabled(s.lookahead); te.setNaturalOnly(s.smooth);
        te.setFlexAmount(0.65f); te.setTransitionAmount(0.65f); te.setSensitivityAmount(0.65f); te.setVibratoCents(0);
        te.setCenterAmount(s.center); te.setVariationAmount(s.variation);
        te.setHumanizeAmount(s.humanize); te.setHumanizeEnabled(s.humanize > 0.005f);
        juce::AudioBuffer<float> y; y.makeCopyOf(mono);
        juce::dsp::AudioBlock<float> blk(y);
        for (int p = 0; p + B <= n; p += B) { auto sb = blk.getSubBlock((size_t) p, B); te.process(juce::dsp::ProcessContextReplacing<float>(sb)); }
        const int lat = te.getLatencySamples(); auto* q = y.getWritePointer(0);
        std::memmove(q, q + lat, sizeof(float) * (size_t)(n - lat)); std::fill(q + n - lat, q + n, 0.0f);
        writeWav(out.getChildFile(juce::String(s.name) + ".wav"), y, sr);
        printf("%s: median %.1f cents off (latency %d)\n", s.name, centsErr(y, sr), lat);
    }
}
