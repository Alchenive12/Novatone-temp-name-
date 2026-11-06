// A/B: same vocal +N semitones through TD-PSOLA (current) and Signalsmith (Natural).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "PsolaShifter.h"
#include "SignalsmithShifter.h"
#include "MPMDetector.h"

static void writeWav(const juce::File& f, const juce::AudioBuffer<float>& b, double sr) {
    f.deleteFile(); juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(f), sr, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}

int main(int argc, char** argv) {
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(argv[1])));
    const double sr = r->sampleRate; const int n = (int) r->lengthInSamples;
    const float semis = argc > 3 ? std::stof(argv[3]) : 1.0f;
    juce::AudioBuffer<float> in((int) r->numChannels, n); r->read(&in, 0, n, 0, true, true);
    juce::AudioBuffer<float> mono(1, n); mono.clear();
    for (int c = 0; c < in.getNumChannels(); ++c) mono.addFrom(0, 0, in, c, 0, n, 1.0f / in.getNumChannels());
    const int B = 256, W = 2048;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) B, 1 };
    PsolaShifter ps; ps.prepare(spec, W / 2 + B); ps.setPitchSemitones(semis);
    SignalsmithShifter ss; ss.prepare(spec); ss.setPitchSemitones(semis); ss.setFormantSemitones(0.0f);
    MPMDetector det; det.prepare(sr, W);
    juce::AudioBuffer<float> a(1, n), b(1, n); a.makeCopyOf(mono); b.makeCopyOf(mono);
    for (int s = 0; s + B <= n; s += B) {
        float hz = s >= W ? det.processFrame(mono.getReadPointer(0, s + B - W)).pitchHz : 0.0f;
        ps.setPeriodHint(hz);
        juce::dsp::AudioBlock<float> ba(a), bb(b);
        auto sa = ba.getSubBlock((size_t) s, B); auto sb = bb.getSubBlock((size_t) s, B);
        ps.process(juce::dsp::ProcessContextReplacing<float>(sa));
        ss.process(juce::dsp::ProcessContextReplacing<float>(sb));
    }
    // line both up with the dry input
    auto shift = [n](juce::AudioBuffer<float>& x, int lat) { auto* p = x.getWritePointer(0); std::memmove(p, p + lat, sizeof(float) * (size_t)(n - lat)); std::fill(p + n - lat, p + n, 0.0f); };
    shift(a, ps.getLatencySamples()); shift(b, ss.getLatencySamples());
    juce::File out(argv[2]);
    writeWav(out.getChildFile("A_current_psola.wav"), a, sr);
    writeWav(out.getChildFile("B_natural_signalsmith.wav"), b, sr);
    writeWav(out.getChildFile("0_dry.wav"), mono, sr);
    printf("psola lat %d, signalsmith lat %d\n", ps.getLatencySamples(), ss.getLatencySamples());
}
