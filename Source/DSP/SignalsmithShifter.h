#pragma once
#include <juce_dsp/juce_dsp.h>
#include "signalsmith-stretch.h"
#include <vector>

/** Fully wet pitch/formant shifter. Callers that blend dry audio must delay
    the dry path by getLatencySamples() (see AlterEngine). */
class SignalsmithShifter {
public:
    SignalsmithShifter() = default;

    void prepare(const juce::dsp::ProcessSpec& spec);
    void reset();
    void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept;

    /** Input-to-output delay of the stretcher, in samples. Valid after prepare(). */
    int getLatencySamples() const noexcept;

    /** How far back (in input samples) the frame currently being transposed is
        centred. Valid after prepare(). */
    int getAnalysisLatencySamples() const noexcept { return stretch.inputLatency(); }

    void setPitchSemitones(float semitones);

    /** Formant shift relative to the original voice: 0 keeps the singer's
        formants in place whatever the pitch shift. */
    void setFormantSemitones(float semitones);

private:
    signalsmith::stretch::SignalsmithStretch<float> stretch;

    float pitchSemitones = 0.0f;
    float formantSemitones = 0.0f;

    juce::AudioBuffer<float> inputCopyBuffer; // stretch must not read and write the same buffer
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
};

