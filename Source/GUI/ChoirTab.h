#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>
#include "ModuleTabs.h"
#include "../Harmony/HarmonyEngine.h"
#include "../Harmony/HarmonyExporter.h"
#include "../Choir/ChoirSession.h"

/**
    Choir tab: record the Harmony tab's voices as parts, then mix AI singers over them while the song plays.
    Each strip = one singer on one part: voice menu (swap = the helper converts only that part, cached after),
    level, pan, mute, solo. Choir level / 3 kHz dip / Balance for the whole choir.
*/
class ChoirTab : public juce::Component, private juce::Timer
{
public:
    ChoirTab (juce::AudioProcessorValueTreeState& apvts, const HarmonyEngine& engine, HarmonyExporter& recorder, ChoirSession& choir);
    ~ChoirTab() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshTheme() { repaint(); }

private:
    struct Row : public juce::Component
    {
        Row (ChoirTab& owner, int index);
        void resized() override;
        void paint (juce::Graphics&) override;
        void update();
        ChoirTab& owner; const int index;
        juce::ComboBox voiceBox;
        juce::Slider level, pan;
        juce::TextButton mute { "M" }, solo { "S" }, remove { juce::CharPointer_UTF8 ("\xc3\x97") };
        juce::String status;
    };

    void timerCallback() override;
    void rebuildRows();
    void toggleRecord();
    void openChooser();
    juce::String recordText() const;

    juce::AudioProcessorValueTreeState& apvts;
    const HarmonyEngine& engine;
    HarmonyExporter& recorder;
    ChoirSession& choir;

    juce::TextButton recordButton, openButton { "Open choir..." }, balanceButton { "Balance" }, addButton { "Add singer" };
    juce::ToggleButton onButton { "Choir on" }, dipButton { "Make room for the lead" };
    juce::Slider busSlider;
    juce::ComboBox addPart;
    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<Row>> rows;
    juce::Rectangle<int> topBar;
    std::unique_ptr<juce::FileChooser> chooser;
    HarmonyExporter::State lastRec = HarmonyExporter::State::idle;
    std::vector<int> armParts, armRegs;
    juce::File lastFolder;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoirTab)
};
