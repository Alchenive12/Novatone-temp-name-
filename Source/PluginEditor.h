// ============================================================================
// Source/PluginEditor.h
// ============================================================================
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>
#include "PluginProcessor.h"
#if __has_include("GUI/HarmonyEngineColours.h")
 #include "GUI/HarmonyEngineColours.h"
 #include "GUI/TuneTab.h"
 #include "GUI/ModuleTabs.h"
#else
 #include "HarmonyEngineColours.h"
 #include "TuneTab.h"
 #include "ModuleTabs.h"
#endif

//==============================================================================
/** Global top bar: NOVATONE brand, 5-Theme Selector, Preset list, SAVE / SAVE AS. */
class PresetBar final : public juce::Component,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    PresetBar(PresetManager& manager, juce::AudioProcessorValueTreeState& apvts,
              std::function<void()> onThemeChangedCallback = {});
    ~PresetBar() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void applyCurrentThemeFromState(bool notify = true);

private:
    void refresh();
    void save();
    void saveAs();

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refresh(); }

    PresetManager& presets;
    juce::AudioProcessorValueTreeState& state;
    std::function<void()> onThemeChanged;

    juce::ComboBox themeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> themeAttachment;
    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save" }, saveAsButton { "Save as" };
    juce::Rectangle<int> brandArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBar)
};

//==============================================================================
/** Bottom strip: INPUT and OUTPUT gain with stereo peak meters. */
class MasterBar final : public juce::Component, private juce::Timer
{
public:
    MasterBar(juce::AudioProcessorValueTreeState& apvts,
        NovaToneAudioProcessor::PeakMeter& inputMeter, NovaToneAudioProcessor::PeakMeter& outputMeter);
    ~MasterBar() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Shows a component (the live tuner) in the middle of the footer. */
    void setCentreComponent(juce::Component* c);

private:
    struct Meter
    {
        float levelDb[2] { -100.0f, -100.0f };
        float holdDb[2] { -100.0f, -100.0f };
        double holdUntilMs[2] { 0.0, 0.0 };
        juce::Rectangle<int> area;
    };

    void timerCallback() override;
    void updateMeter(Meter& meter, NovaToneAudioProcessor::PeakMeter& source, float dt, double nowMs);
    void paintMeter(juce::Graphics& g, const Meter& meter);
    void paintKnobCaption(juce::Graphics& g, const juce::Slider& knob, const juce::String& caption, bool captionLeft);

    NovaToneAudioProcessor::PeakMeter& inputSource;
    NovaToneAudioProcessor::PeakMeter& outputSource;

    juce::Slider inputKnob, outputKnob;
    juce::AudioProcessorValueTreeState::SliderAttachment inputAttachment, outputAttachment;
    Meter inputMeter, outputMeter;
    juce::Component* centreComponent = nullptr;
    double lastTickMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterBar)
};

//==============================================================================
/**
    Standalone "NovaTone" Single-Window AudioProcessorEditor.
    Eliminates HarmonyEngine's 8-tab channel-strip layout in favor of an
    expansive, resizable, single-window vocal pitch & time workstation.
*/
class NovaToneAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit NovaToneAudioProcessorEditor(NovaToneAudioProcessor&);
    ~NovaToneAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    NovaToneAudioProcessor& audioProcessor;

    PresetBar presetBar;
    TuneTab   workstationView;
    MasterBar masterBar;
    bool      ownsDefaultLookAndFeel = false;
    juce::TooltipWindow tooltipWindow { this, 500 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NovaToneAudioProcessorEditor)
};

using HarmonyEngineAudioProcessorEditor = NovaToneAudioProcessorEditor;