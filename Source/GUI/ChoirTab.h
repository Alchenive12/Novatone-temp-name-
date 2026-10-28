#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>
#include "ModuleTabs.h"
#include "../Harmony/HarmonyEngine.h"
#include "../Harmony/HarmonyExporter.h"
#include "../Choir/ChoirSession.h"

/**
    Choir tab: record the Harmony tab's voices as parts ("takes"), then mix AI singers over them while the song plays.
    Takes live in groups (one group = one song's choir); all takes of the open group play together.
    Each strip = one singer on one part: voice menu with names and recommended ranges (✓ = fits the part,
    ● = ready to play now), < > to step through the voices that fit, level, pan, mute, solo.
    Group tools: prepare every fitting voice (so trying them is instant), export the group mix, send it to the Tune tab.
*/
class ChoirTab : public juce::Component, private juce::Timer
{
public:
    ChoirTab (juce::AudioProcessorValueTreeState& apvts, const HarmonyEngine& engine, HarmonyExporter& recorder, ChoirSession& choir);
    ~ChoirTab() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshTheme() { repaint(); }

    /** Editor hook: load the rendered group (song position in samples at `rate`) into the Tune tab. */
    std::function<void (const juce::File&, juce::int64 start, double rate)> onSendToTune;

private:
    struct Row : public juce::Component
    {
        Row (ChoirTab& owner, int index);
        void resized() override;
        void paint (juce::Graphics&) override;
        void update();
        void fillVoices();
        void step (int dir);
        ChoirTab& owner; const int index;
        juce::ComboBox voiceBox;
        juce::Slider level, pan;
        juce::TextButton prev { "<" }, next { ">" }, mute { "M" }, solo { "S" }, remove { juce::CharPointer_UTF8 ("\xc3\x97") };
        juce::String status, readyKey;
    };

    struct TakeHeader : public juce::Component
    {
        TakeHeader (ChoirTab& owner, int take);
        void resized() override;
        void paint (juce::Graphics&) override;
        ChoirTab& owner; const int take;
        juce::ComboBox moveBox, partBox;
        juce::TextButton addButton { "+ Singer" };
    };

    void timerCallback() override;
    void rebuildRows();
    void refreshGroups();
    void toggleRecord();
    void openChooser();
    void exportGroup();
    void sendToTune();
    juce::String recordText() const;

    juce::AudioProcessorValueTreeState& apvts;
    const HarmonyEngine& engine;
    HarmonyExporter& recorder;
    ChoirSession& choir;

    juce::TextButton recordButton, newGroupButton { "New group" }, openButton { "Open..." }, balanceButton { "Balance" },
                     prepareButton { "Prepare all fitting voices" }, exportButton { "Export choir" }, tuneButton { "Send group to Tune" };
    juce::ToggleButton onButton { "Choir on" }, dipButton { "Make room for the lead" };
    juce::ComboBox groupBox;
    juce::Slider busSlider;
    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<TakeHeader>> headers;
    juce::Rectangle<int> topBar;
    std::unique_ptr<juce::FileChooser> chooser;
    HarmonyExporter::State lastRec = HarmonyExporter::State::idle;
    std::vector<int> armParts, armRegs, armLo, armHi;
    juce::File lastFolder;
    int lastTakes = -1;
    juce::String note;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoirTab)
};
