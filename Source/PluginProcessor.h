#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>

#include "GUI/PitchTraceQueue.h"
#include "DSP/TuneEngine.h"
#include "DSP/TuneMacro.h"
#include "DSP/AlterEngine.h"
#include "DSP/ModuleBypass.h"
#include "PresetManager.h"

/**
    Standalone "NovaTone" Vocal Pitch, Time & Formant AudioProcessor.

    Completely separate plugin identity from HarmonyEngine:
      - Dedicated to NewTone-style interactive vocal pitch & time editing,
        global Center / Sensitivity / Variation / Transition / Formant macros,
        and per-note Formant / Variation / Gain / Ramp overrides.
      - Streamlined low-latency signal path:
          Input Gain -> TuneEngine (MPM + Quantizer + VibratoTracker + SmoothPlanner + TD-PSOLA)
                     -> AlterEngine (Signalsmith Formant & Pitch Shifter, auto-engaged when Formant/Pitch != 0)
                     -> Output Gain.
*/
class NovaToneAudioProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorValueTreeState::Listener,
                                     private juce::AsyncUpdater
{
public:
    enum ScaleMode { majorScale = 0, minorScale, customScale };

    /** 12-bit pitch-class mask (bit 0 = C) of a major / natural minor scale on root. */
    static uint16_t scaleMaskFor(int root, int mode) noexcept;

    NovaToneAudioProcessor();
    ~NovaToneAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    PitchTraceQueue& getPitchTraceQueue() noexcept { return pitchQueue; }

    /** Linear peak per channel (L, R) since the GUI last took it (exchange with 0). */
    using PeakMeter = std::array<std::atomic<float>, 2>;
    PeakMeter& getInputPeakMeter() noexcept { return inputPeak; }
    PeakMeter& getOutputPeakMeter() noexcept { return outputPeak; }

    juce::AudioProcessorValueTreeState apvts;

    PresetManager& getPresetManager() noexcept { return presetManager; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void pushParametersToEngines() noexcept;
    uint16_t packScaleDegreeMask() const noexcept;

    void parameterChanged(const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    bool lookaheadRequested() const noexcept;
    bool smoothRequested() const noexcept;
    void applySyncMacro();
    int totalLatencyFor(bool lookahead) const noexcept;
    int currentRoot() const noexcept;
    int currentMode() const noexcept;
    uint16_t currentDegreeMask() const noexcept;
    void setParameterFromMessageThread(const juce::String& id, float plainValue);

    void applyStateGuarded(const juce::ValueTree& newState);
    void resetToDefaultsGuarded();

    std::atomic<bool> applyingScale { false };
    std::atomic<bool> scaleChangeRequested { false };
    std::atomic<bool> keyEdited { false };
    std::atomic<bool> applyingMacro { false };
    std::atomic<bool> syncRequested { false };
    std::atomic<bool> knobTouched { false };
    static constexpr std::array<const char*, 11> syncListenerIds {
        "tune_lookahead", "tune_smooth", "tune_sync", "tune_character",
        "tune_flexAmount", "tune_humanizeAmount", "tune_vibrato",
        "tune_center", "tune_variation", "tune_transition", "tune_sensitivity"
    };

    TuneEngine   tuneEngine;
    AlterEngine  alterEngine;

    juce::dsp::Gain<float> inputGain, outputGain;
    PeakMeter inputPeak {}, outputPeak {};
    static void publishPeaks(const juce::dsp::AudioBlock<float>& block, PeakMeter& meter) noexcept;

    enum Module { tuneModule = 0, alterModule, numModules };
    std::array<ModuleBypass, numModules> bypass;
    std::atomic<float>* tuneBypassParam  = nullptr;
    std::atomic<float>* alterBypassParam = nullptr;

    PitchTraceQueue pitchQueue;

    // Global NewTone Parameters:
    std::atomic<float>* tuneFlexAmountParam     = nullptr;
    std::atomic<float>* tuneHumanizeAmountParam = nullptr;
    std::atomic<float>* tuneHumanizeButtonParam = nullptr;
    std::atomic<float>* tunePolyphonicParam     = nullptr;
    std::atomic<float>* tuneMidiCleanParam      = nullptr;
    std::atomic<float>* tuneVibratoParam        = nullptr;
    std::atomic<float>* tuneCenterParam         = nullptr;
    std::atomic<float>* tuneVariationParam      = nullptr;
    std::atomic<float>* tuneTransitionParam     = nullptr;
    std::atomic<float>* tuneSensitivityParam    = nullptr;
    std::atomic<float>* tuneSnapScaleParam      = nullptr;
    std::atomic<float>* tuneAutoScrollParam     = nullptr;
    std::atomic<float>* tuneLookaheadParam      = nullptr;
    std::atomic<float>* tuneSyncParam           = nullptr;
    std::atomic<float>* tuneSmoothParam         = nullptr;
    std::atomic<float>* tuneCharacterParam      = nullptr;
    std::array<std::atomic<float>*, 12> tuneScaleDegreeParams{};
    std::atomic<float>* tuneRootParam = nullptr;
    std::atomic<float>* tuneModeParam = nullptr;
    std::atomic<float>* tuneVocalRangeParam = nullptr;

    std::atomic<float>* alterPitchParam   = nullptr;
    std::atomic<float>* alterFormantParam = nullptr;
    std::atomic<float>* alterMixParam     = nullptr;
    std::atomic<float>* alterSoloWetParam = nullptr;

    std::atomic<float>* inputGainParam  = nullptr;
    std::atomic<float>* outputGainParam = nullptr;

    std::array<bool, 128> heldMidiNotes {};
    std::array<bool, 128> activeOutputMidiNotes {};
    int lastActiveMidiNote = -1;
    int lastPublishedHopIndex = 0;

    PresetManager presetManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovaToneAudioProcessor)
};

// Backwards-compatible alias so shared editor helpers compile seamlessly
using VocalStripAudioProcessor = NovaToneAudioProcessor;