#pragma once

#include <JuceHeader.h>
#include <array>
#include <memory>

#include "ModuleTabs.h"
#include "PitchTraceComponent.h"
#include "PitchTraceQueue.h"
#include "../Poly/PolyClipSession.h"

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

    /** Connects the poly clip (record / load / edit / playback) used in Poly mode. */
    void setClipSession (Poly::ClipSession* session);

    /** The live tuner readout; the editor shows it in its footer. */
    juce::Component& getTunerComponent() noexcept { return noteTriangle; }

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

    private:
        juce::ComboBox rangeBox, rootBox, modeBox;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rangeAttachment, rootAttachment, modeAttachment;

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
                         public juce::SettableTooltipClient,
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

    void updateControlStates();
    void setActiveToolMode (PitchTraceComponent::ToolMode mode);
    void applyQuickVocalStyle (float center, float sensitivity, float variation, float transition, float vibratoCents, float humanize);
    void setParamNormalised (const juce::String& paramId, float plainValue);
    void refreshSelectedNoteInspector();
    void cycleDockLayoutPreset();
    void setInspectorVisibility (bool visible);
    void setDockVisibility (bool visible);

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

    // Toolbar
    juce::ToggleButton editToolButton     { "Move" };
    juce::ToggleButton cutToolButton      { "Slice" };
    juce::ToggleButton advancedToolButton { "Shape" };
    juce::ToggleButton drawToolButton     { "Draw" };
    juce::ToggleButton monoModeButton     { "Mono" };
    juce::ToggleButton polyModeButton     { "Poly" };
    juce::ToggleButton midiCleanButton    { "MIDI clean" };
    juce::ToggleButton snapScaleButton    { "Snap" };
    juce::ToggleButton autoScrollButton   { "Follow" };
    juce::TextButton   undoButton;
    juce::TextButton   redoButton;
    juce::TextButton   autoKeyButton      { "Detect" };
    juce::TextButton   levelerButton      { "Level" };
    juce::TextButton   dockViewButton;
    juce::TextButton   snapAllButton      { "Snap all" };
    juce::TextButton   fileMenuButton     { "File" };
    juce::ToggleButton recordButton       { "Rec" };
    juce::TextButton   clipPlayButton;
    juce::TextButton   clipStopButton;
    juce::TextButton   clipToStartButton;
    juce::ToggleButton snapGridButton     { "Grid" };
    juce::ToggleButton independentButton  { "Indep." };
    Poly::ClipSession* clipSession = nullptr;
    void updateClipControls();

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> polyModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> midiCleanAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> snapScaleAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoScrollAttachment;

    // Selected-Note Quick Inspector Controls:
    juce::Label      inspectorNoteLabel;
    juce::TextButton semiDownButton;
    juce::TextButton semiUpButton;
    juce::Slider     noteCentsSlider;
    juce::Slider     noteFormantSlider;
    juce::Slider     noteVarSlider;
    juce::Slider     noteGainSlider;
    juce::Slider     noteSibSlider;
    juce::TextButton resetNoteButton { "Reset" };
    bool             updatingInspector = false;

    // 1-Click Vocal Style Preset Buttons, Humanize & Lookahead Controls:
    juce::TextButton   styleNaturalButton { "Natural" };
    juce::TextButton   stylePopButton     { "Pop" };
    juce::TextButton   styleHardButton    { "Hard tune" };
    juce::Slider       humanizeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> humanizeAttachment;
    juce::ToggleButton smoothButton;
    juce::AudioProcessorValueTreeState::ButtonAttachment smoothAttachment;
    juce::ToggleButton lookaheadButton;
    juce::AudioProcessorValueTreeState::ButtonAttachment lookaheadAttachment;

    // Lower area visibility (toggled by the dock button, the splitter, or Ctrl+B):
    static constexpr int inspectorHeight = 38;
    static constexpr int dockHeight      = 156;
    bool  showInspector     = true;
    bool  showDock          = true;
    float dragStartMouseY   = 0.0f;
    bool  draggingSplitter  = false;
    bool  splitterHovered   = false;

    void showFileMenu();

    juce::Rectangle<int> splitterBarArea;     ///< Drag / double-click handle between piano roll and lower area
    juce::Rectangle<int> inspectorPanelArea;
    juce::Rectangle<int> scaleCardArea, knobCardArea, styleCardArea;
    juce::Rectangle<int> scaleTitleArea, knobTitleArea, styleTitleArea;
    juce::Rectangle<int> humanizeLabelArea;
    juce::Rectangle<int> noteLabelArea;

    juce::Rectangle<int> centsLabelArea, formantLabelArea, varLabelArea, gainLabelArea, sibLabelArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TuneTab)
};