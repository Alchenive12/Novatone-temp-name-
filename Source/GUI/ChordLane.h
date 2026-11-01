#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include "../Harmony/MidiChordTrack.h"

/**
    Harmony tab, "MIDI file" chord source: import a MIDI file of chords and line it up with the lead vocal.

    The lane shows the file's chords (shifted by the offset) on the song's beat grid, with the lead's
    notes over them: green where the lead is on a chord tone, orange where it isn't. Drag the lane (or nudge)
    to move the chords; Auto-align finds the offset where the lead fits the chords best.
*/
class ChordLane : public juce::Component, private juce::Timer
{
public:
    struct Hooks
    {
        std::function<bool (const juce::File&, juce::String&)> load;
        std::function<void()> clear;
        std::function<std::shared_ptr<const MidiChordTrack>()> track;
        std::function<double()> positionPpq;   ///< song position the chords are read at (before the offset)
        std::function<bool()> moving;          ///< song (or a Mono take) is playing
        std::function<std::vector<MidiChordTrack::LeadNote>()> leadNotes;
    };

    explicit ChordLane (juce::AudioProcessorValueTreeState& state);
    ~ChordLane() override;

    Hooks hooks;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    double offset() const;
    void setOffset (double beats);
    void nudge (double sign, const juce::ModifierKeys&);
    void autoAlign();
    void importFile();
    void refreshLead (bool force);
    float xFor (double ppq) const;
    double ppqFor (float x) const;

    juce::AudioProcessorValueTreeState& apvts;
    juce::TextButton importButton { "Import MIDI..." }, clearButton, leftButton, rightButton, alignButton { "Auto-align" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> laneArea, infoArea;

    std::shared_ptr<const MidiChordTrack> track;
    std::vector<MidiChordTrack::LeadNote> lead;
    float leadLo = 55.0f, leadHi = 72.0f;
    float fitNow = -1.0f;            ///< share of the lead on chord tones at the current offset (-1 = unknown)
    double fitOffset = 1.0e9;        ///< offset fitNow was computed for
    size_t fitLeadCount = 0;
    juce::String message;
    double viewStart = 0.0, viewBeats = 16.0, pos = 0.0;
    bool following = true;
    double dragStartOffset = 0.0;
    float dragStartX = 0.0f;
    bool dragging = false;
    int leadTick = 0;
};
