#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>

#include "ModuleTabs.h"
#include "PitchTraceComponent.h"
#include "PitchTraceQueue.h"

//==============================================================================
/**
    NovaTone Single-Window Vocal Pitch & Time Workstation View.

    Supports live 1-click theme switching across all 5 Alchenive themes:
      - NovaTone Studio (Cyan & Amber)
      - Harmony Engine  (Ivory & Gold)
      - The Drum Master (Noir & Seafoam Mint/Crimson)
      - The Conductor   (Concert Hall Espresso & Champagne Brass)
      - AmpPire Valve   (Obsidian Tolex & Valve Amber)

    Includes an interactive Draggable Horizontal Splitter Bar (plus double-click,
    a dedicated top-of-knob-dock drag handle, and 1-click toolbar presets) to
    adjust or collapse the Quick Inspector Bar and Lower Control Dock at a
    60 Hz throttled relayout rate with zero audio crackles.
*/
class TuneTab : public ModuleTab
{
public:
    explicit TuneTab (juce::AudioProcessorValueTreeState& apvts, PitchTraceQueue& queue);
    ~TuneTab() override;

    void refreshTheme();

    // Interactive splitter bar mouse overrides for resizing/collapsing the lower bars:
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    void paintBody (juce::Graphics& g) override;
    void layoutBody (juce::Rectangle<int> body) override;

    class ScaleSelector : public juce::Component
    {
    public:
        explicit ScaleSelector (juce::AudioProcessorValueTreeState& apvts);
        ~ScaleSelector() override;

        void paint (juce::Graphics& g) override;
        void resized() override;

        static constexpr int preferredWidth = 382;

    private:
        juce::ComboBox rangeBox, rootBox, modeBox;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rangeAttachment, rootAttachment, modeAttachment;
        juce::Rectangle<int> rangeLabelArea, keyLabelArea, scaleLabelArea;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleSelector)
    };

    class PianoMask : public juce::Component
    {
    public:
        explicit PianoMask (juce::AudioProcessorValueTreeState& apvts);
        void resized() override;

    private:
        class PianoKeyButton : public juce::ToggleButton
        {
        public:
            PianoKeyButton (juce::String noteName, bool isBlackKeyToDraw);
            void paint (juce::Graphics& g) override;

        private:
            juce::String note;
            bool blackKey;
        };

        std::array<std::unique_ptr<PianoKeyButton>, 12> keyButtons;
        std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 12> keyAttachments;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoMask)
    };

    class NoteTriangle : public juce::Component,
                         private juce::Timer
    {
    public:
        explicit NoteTriangle (const PitchTraceComponent& pitchSourceToPoll);
        ~NoteTriangle() override;

        void paint (juce::Graphics& g) override;

    private:
        void timerCallback() override;
        static juce::String noteNameForMidi (int midiNote);

        const PitchTraceComponent& pitchSource;

        bool  hasSignal        = false;
        float smoothedMidiNote = 60.0f;
        float breathPhase      = 0.0f;
        float smoothedCorrectionCents = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteTriangle)
    };

    enum class SplitterDragMode
    {
        None = 0,
        ResizeLowerDock,     ///< Main splitter right below Piano Roll (or top of Knob Dock): scales/collapses the lower dock.
        ResizeInspectorOnly  ///< Divider right below Inspector Bar: resizes/collapses only the Inspector Bar.
    };

    void updateControlStates();
    void setActiveToolMode (PitchTraceComponent::ToolMode mode);
    void applyQuickVocalStyle (float center, float sensitivity, float variation, float transition, float vibratoCents, float humanize);
    void setParamNormalised (const juce::String& paramId, float plainValue);
    void refreshSelectedNoteInspector();
    void cycleDockLayoutPreset();
    void setInspectorVisibility (bool visible);
    void setMiddleRowVisibility (bool visible);
    void setKnobRowVisibility (bool visible);

    juce::AudioProcessorValueTreeState& apvtsRef;

    ScaleSelector       scaleSelector;
    PitchTraceComponent pitchTrace;
    NoteTriangle        noteTriangle;
    PianoMask           pianoMask;

    // 6 Semantic Color-Coded Global Macro Knobs:
    RotaryKnob centerKnob;
    RotaryKnob sensitivityKnob;
    RotaryKnob variationKnob;
    RotaryKnob transitionKnob;
    RotaryKnob formantKnob;
    RotaryKnob vibratoKnob;

    // Top Editor Toolbar Buttons:
    juce::ToggleButton editToolButton     { "1. MOVE" };
    juce::ToggleButton cutToolButton      { "2. SLICE" };
    juce::ToggleButton advancedToolButton { "3. SHAPER" };
    juce::ToggleButton drawToolButton     { "4. DRAW" };
    juce::ToggleButton polyModeButton     { "MONO (NEWTONE)" };
    juce::ToggleButton midiCleanButton    { "MIDI CLEAN" };
    juce::ToggleButton snapScaleButton    { "SNAP SCALE" };
    juce::ToggleButton autoScrollButton   { "AUTO-SCROLL" };
    juce::TextButton   undoButton         { "UNDO" };
    juce::TextButton   redoButton         { "REDO" };
    juce::TextButton   autoKeyButton      { "AUTO-KEY" };
    juce::TextButton   levelerButton      { "LEVELER" };
    juce::TextButton   dockViewButton     { "BAR: FULL" };
    juce::TextButton   snapAllButton      { "SNAP ALL" };
    juce::TextButton   loadWavButton      { "LOAD AUDIO" };
    juce::TextButton   exportMidiButton   { "EXPORT MIDI" };
    juce::TextButton   clearButton        { "CLEAR" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> polyModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> midiCleanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> snapScaleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoScrollAttachment;

    // Selected-Note Quick Inspector Controls:
    juce::Label      inspectorNoteLabel;
    juce::TextButton semiDownButton { "-1 SEMI" };
    juce::TextButton semiUpButton   { "+1 SEMI" };
    juce::Slider     noteCentsSlider;
    juce::Slider     noteFormantSlider;
    juce::Slider     noteVarSlider;
    juce::Slider     noteGainSlider;
    juce::Slider     noteSibSlider;
    juce::TextButton resetNoteButton { "RESET NOTE" };
    bool             updatingInspector = false;

    // 1-Click Vocal Style Preset Buttons, Humanize & Lookahead Controls:
    juce::TextButton   styleNaturalButton { "NATURAL VOCAL" };
    juce::TextButton   stylePopButton     { "MODERN POP" };
    juce::TextButton   styleHardButton    { "HARD TUNE" };
    juce::Slider       humanizeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> humanizeAttachment;
    juce::Rectangle<int> humanizeLabelArea;
    juce::ToggleButton smoothButton;
    juce::AudioProcessorValueTreeState::ButtonAttachment smoothAttachment;
    juce::ToggleButton lookaheadButton;
    juce::AudioProcessorValueTreeState::ButtonAttachment lookaheadAttachment;

    // Adjustable Lower Bar & Dock Height State:
    static constexpr int defaultLowerDockHeight = 280;
    static constexpr int defaultInspectorHeight = 40;
    int  lowerDockHeight       = defaultLowerDockHeight; ///< Total height below main splitter (0..340 px)
    int  savedLowerDockHeight  = defaultLowerDockHeight; ///< Restored on double-click when collapsed
    int  userInspectorHeight   = defaultInspectorHeight; ///< Independent Quick Inspector Bar height (0 or 26..64 px)
    int  dragStartDockHeight   = defaultLowerDockHeight;
    int  dragStartInspHeight   = defaultInspectorHeight;
    float dragStartMouseY      = 0.0f;
    juce::uint32 lastDragLayoutMs = 0;                   ///< 60 Hz frame-rate limiter for smooth drag relayout
    SplitterDragMode activeSplitterDrag = SplitterDragMode::None;
    bool mainSplitterHovered   = false;
    bool inspSplitterHovered   = false;
    bool knobSplitterHovered   = false;

    juce::Rectangle<int> toolbarPanelArea;
    juce::Rectangle<int> splitterBarArea;          ///< Main draggable bar right below the 2D Piano Roll
    juce::Rectangle<int> inspectorBottomSplitArea; ///< Secondary draggable divider right above the 12-key Piano Roll
    juce::Rectangle<int> knobTopSplitArea;         ///< Draggable divider right above the 6-knob dock
    juce::Rectangle<int> inspectorPanelArea;
    juce::Rectangle<int> pianoPanelArea;
    juce::Rectangle<int> macroPanelArea;
    juce::Rectangle<int> knobPanelArea;

    juce::Rectangle<int> centsLabelArea, formantLabelArea, varLabelArea, gainLabelArea, sibLabelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneTab)
};