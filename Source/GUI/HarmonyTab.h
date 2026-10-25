#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>
#include "ModuleTabs.h"
#include "../Harmony/HarmonyEngine.h"
#include "../Harmony/HarmonyExporter.h"

/**
    Harmony tab: chord-aware harmony voices generated from the lead vocal, following the chords
    of the beat/instrumental routed into NovaTone's sidechain input.

    Top: detected chord + sidechain status + chromagram | live keyboard (lead, voices, chord tones)
    | mix (harmony level, lead level, humanize, chord source).
    Bottom: four voice cards (on/off, mode, level, pan, formant, current note).
*/
class HarmonyTab : public juce::Component, private juce::Timer
{
public:
    HarmonyTab (juce::AudioProcessorValueTreeState& apvts, const HarmonyEngine& engine,
                HarmonyExporter& exporter, std::function<bool()> sidechainConnected);
    ~HarmonyTab() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshTheme();

private:
    void timerCallback() override;
    void paintChordCard (juce::Graphics&, juce::Rectangle<int>);
    void paintKeyboardCard (juce::Graphics&, juce::Rectangle<int>);

    struct VoiceCard : public juce::Component
    {
        VoiceCard (juce::AudioProcessorValueTreeState& apvts, int index);
        ~VoiceCard() override;
        void paint (juce::Graphics&) override;
        void resized() override;

        int index;
        int currentNote = -1;
        juce::ToggleButton power;
        juce::AudioProcessorValueTreeState::ButtonAttachment powerAttachment;
        ChoiceSelector mode;
        RotaryKnob level, pan, formant;
    };

    /** Button you can drag out of the plugin: drops the exported WAV into the DAW. */
    struct FileDragButton : public juce::TextButton
    {
        std::function<juce::File()> getFile;
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (e.getDistanceFromDragStart() < 6 || dragging || getFile == nullptr) return;
            const auto f = getFile();
            if (! f.existsAsFile()) return;
            dragging = true;
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this, [this] { dragging = false; });
        }
        void mouseUp (const juce::MouseEvent& e) override { dragging = false; juce::TextButton::mouseUp (e); }
        bool dragging = false;
    };

    void updateExportControls();
    void paintExportBar (juce::Graphics&);

    juce::AudioProcessorValueTreeState& apvts;
    const HarmonyEngine& engine;
    HarmonyExporter& exporter;
    juce::TextButton exportButton, saveAsButton, showButton;
    FileDragButton dragButton;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> exportBar, exportStatus;
    HarmonyExporter::State lastExportState = HarmonyExporter::State::idle;
    std::function<bool()> sidechainConnected;

    std::array<std::unique_ptr<VoiceCard>, HarmonyEngine::numVoices> voiceCards;
    RotaryKnob mixKnob, leadKnob, humanizeKnob;
    ChoiceSelector sourceSelector;

    juce::Rectangle<int> chordCard, keyboardCard, mixCard, helpArea;
    int   chord = -1;
    float leadMidi = 0.0f;
    bool  sideActive = false, sideConnected = false;
    std::array<float, 12> chroma {};
    std::array<int, HarmonyEngine::numVoices> voiceNotes {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HarmonyTab)
};
