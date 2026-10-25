#pragma once
#include <juce_dsp/juce_dsp.h>
#include "SignalsmithShifter.h"

class AlterEngine {
public:
    AlterEngine() = default;

    void prepare(const juce::dsp::ProcessSpec& spec)
    {
        shifter.prepare(spec);
        mixer.prepare(spec);
        mixer.setMixingRule(juce::dsp::DryWetMixingRule::linear);
        mixer.setWetLatency(static_cast<float> (shifter.getLatencySamples())); // dry path delayed to match
    }

    void reset()
    {
        shifter.reset();
        mixer.reset();
    }

    void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        mixer.setWetMixProportion(soloWet ? 1.0f : mix);
        mixer.pushDrySamples(block);
        shifter.process(context);
        mixer.mixWetSamples(block);
    }

    int getLatencySamples() const noexcept { return shifter.getLatencySamples(); }

    void setPitch(float v) { shifter.setPitchSemitones(v); }
    void setFormant(float v) { shifter.setFormantSemitones(v); }
    void setMix(float v) { mix = juce::jlimit(0.0f, 1.0f, v); }
    void setSoloWet(bool v) { soloWet = v; }

private:
    SignalsmithShifter shifter;

    // Max dry delay. presetDefault latency is ~0.12 s, i.e. 23040 samples at 192 kHz.
    juce::dsp::DryWetMixer<float> mixer { 1 << 15 };

    float mix = 1.0f;
    bool  soloWet = false;
};

