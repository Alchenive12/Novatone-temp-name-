#include "SignalsmithShifter.h"
#include <cmath>

void SignalsmithShifter::prepare(const juce::dsp::ProcessSpec& spec) {
    stretch.presetDefault((int)spec.numChannels, spec.sampleRate);
    stretch.reset();

    inputCopyBuffer.setSize((int)spec.numChannels, (int)spec.maximumBlockSize);
    inputPointers.resize(spec.numChannels, nullptr);
    outputPointers.resize(spec.numChannels, nullptr);
}

void SignalsmithShifter::reset() {
    stretch.reset();
}

int SignalsmithShifter::getLatencySamples() const noexcept {
    return stretch.inputLatency() + stretch.outputLatency();
}

void SignalsmithShifter::setPitchSemitones(float semitones) {
    pitchSemitones = semitones;
}

void SignalsmithShifter::setFormantSemitones(float semitones) {
    formantSemitones = semitones;
}

void SignalsmithShifter::process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept {
    auto& block = context.getOutputBlock();
    const int numChannels = (int)block.getNumChannels();
    const int numSamples = (int)block.getNumSamples();

    if (numSamples == 0) return;

    stretch.setTransposeSemitones(pitchSemitones);

    // compensatePitch = true: the spectral envelope is measured and kept where
    // it was before the pitch shift, then moved by the formant amount alone.
    stretch.setFormantSemitones(formantSemitones, true);

    // Stretch reads input while it writes output, so the input must not alias
    // the output block: copy it aside first (in-place use tears the grains).
    for (int ch = 0; ch < numChannels; ++ch) {
        inputCopyBuffer.copyFrom(ch, 0, block.getChannelPointer(ch), numSamples);
        inputPointers[ch] = inputCopyBuffer.getReadPointer(ch);
        outputPointers[ch] = block.getChannelPointer(ch);
    }

    stretch.process(inputPointers.data(), numSamples, outputPointers.data(), numSamples);
}

