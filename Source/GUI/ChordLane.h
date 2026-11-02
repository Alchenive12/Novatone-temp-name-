#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>
#include "../Harmony/MidiChordTrack.h"

/**
    Harmony tab, "MIDI file" chord source.

    Compact (under the top cards): the chords on the bar grid with the lead's notes under them (green = on
    a chord tone, orange = not). Drag to shift the chords against the song; Auto-align finds the best shift.

    Expanded (fills the tab): a chord editor that moves like the Tune piano roll - wheel scrolls pitch,
    Shift+wheel scrolls time, Ctrl+wheel zooms time, Alt+wheel zooms pitch, drag empty space to pan.
    Write mode: pick a chord (chips for the song's key, "More..." for any chord) and drag it onto the
    timeline, or arm it and click; drag chords to move, drag their edges to resize, right-click to change
    or delete. Snap: bar / half bar / beat / half beat / quarter beat. Ctrl+Z / Ctrl+Y undo / redo.
*/
class ChordLane : public juce::Component,
                  public juce::DragAndDropContainer,
                  public juce::DragAndDropTarget,
                  private juce::Timer
{
public:
    struct Hooks
    {
        std::function<bool (const juce::File&, juce::String&)> load;
        std::function<void()> clear;
        std::function<std::shared_ptr<const MidiChordTrack>()> track;
        std::function<void (std::shared_ptr<const MidiChordTrack>)> publish;   ///< edited chords -> the engine
        std::function<double()> positionPpq;   ///< song position the chords are read at (before the offset)
        std::function<bool()> moving;          ///< song (or a Mono take) is playing
        std::function<std::vector<MidiChordTrack::LeadNote>()> leadNotes;
    };

    explicit ChordLane (juce::AudioProcessorValueTreeState& state);
    ~ChordLane() override;

    Hooks hooks;
    std::function<void()> onExpandedChanged;
    bool isExpanded() const noexcept { return expanded; }
    void setExpanded (bool shouldExpand);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    /** Chord chips (write mode): click to arm, drag onto the timeline. */
    struct Palette : public juce::Component
    {
        explicit Palette (ChordLane& o) : owner (o) {}
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        struct Chip { int root, quality; juce::Rectangle<int> r; bool more = false; };
        std::vector<Chip> chips;
        void rebuild();
        int hit (juce::Point<int>) const;
        ChordLane& owner;
        int pressed = -1;
    };

    void timerCallback() override;
    double offset() const;
    void setOffset (double beats);
    void nudge (double sign, const juce::ModifierKeys&);
    void autoAlign();
    void importFile();
    void refreshLead (bool force);
    float xFor (double ppq) const;
    double ppqFor (float x) const;
    float yFor (float midi) const;
    double snapBeats() const;
    double snap (double ppq) const { const double q = snapBeats(); return std::round (ppq / q) * q; }

    // write mode
    std::shared_ptr<MidiChordTrack> editableCopy() const;
    void commit (std::shared_ptr<MidiChordTrack> edited);
    void placeChord (int root, int quality, double songStart);
    int  segmentAtX (float x) const;
    void showChordMenu (int segIndex);
    void chooseChord (std::function<void (int root, int quality)> then);
    void deleteSelected();
    int keyRoot() const;
    bool keyMinor() const;

    juce::AudioProcessorValueTreeState& apvts;
    juce::TextButton importButton { "Import MIDI..." }, clearButton, leftButton, rightButton, alignButton { "Auto-align" },
                     expandButton, writeButton { "Write" };
    juce::ComboBox snapBox;
    Palette palette { *this };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> laneArea, chordRow, rollArea, keysArea, infoArea;

    std::shared_ptr<const MidiChordTrack> track;
    std::vector<std::shared_ptr<const MidiChordTrack>> undoStack, redoStack;
    std::vector<MidiChordTrack::LeadNote> lead;
    float leadLo = 55.0f, leadHi = 72.0f;
    float fitNow = -1.0f;
    double fitOffset = 1.0e9;
    size_t fitLeadCount = 0;
    juce::String message;

    bool expanded = false, writeMode = false;
    double viewStart = 0.0, viewBeats = 16.0, pos = 0.0;
    float viewLo = 48.0f, viewHi = 76.0f;       // expanded piano roll pitch range
    bool following = true, pitchViewSet = false;

    int armedRoot = 0, armedQuality = 0;
    bool armed = false;
    int selected = -1;                          // segment index (write mode)
    static constexpr double noDrop = -1.0e9;
    double dropPreview = noDrop;                // song ppq of a chip being dragged over the lane

    enum class Drag { none, offset, pan, move, resizeLeft, resizeRight, draw };
    Drag drag = Drag::none;
    double dragStartOffset = 0.0, dragStartViewStart = 0.0, dragAnchor = 0.0;
    float  dragStartX = 0.0f, dragStartY = 0.0f, dragStartLo = 0.0f;
    MidiChordTrack::Segment dragSeg;            // the chord being moved / resized / drawn (file time)
    std::shared_ptr<MidiChordTrack> dragBase;   // the chords without it
    int leadTick = 0;
};
