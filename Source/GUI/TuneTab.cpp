#include "TuneTab.h"
#include "HarmonyEngineColours.h"
#include <cmath>

//==============================================================================
namespace
{
    constexpr int toolbarHeight  = 34;
    constexpr int splitterHeight = 10;
    constexpr int cardPad        = 12;
    constexpr int cardTitleH     = 16;

    const juce::String minusSign = juce::String::charToString ((juce::juce_wchar) 0x2212);

    void styleSegment (juce::Button& b, const char* segment)
    {
        b.getProperties().set ("segment", segment);
    }

    void styleChip (juce::Button& b, const char* role)
    {
        b.getProperties().set ("chip", true);
        b.getProperties().set ("pillRole", role);
    }
}

//==============================================================================
// SCALE KEYBOARD (12 pitch-class mask)
//==============================================================================
TuneTab::PianoMask::PianoKeyButton::PianoKeyButton (juce::String noteName, bool isBlackKeyToDraw)
    : note (std::move (noteName)), blackKey (isBlackKeyToDraw)
{
    setTooltip ("Toggle " + note + " in or out of the active scale");
}

void TuneTab::PianoMask::PianoKeyButton::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;

    const auto bounds = getLocalBounds().toFloat().reduced (blackKey ? 0.0f : 0.5f, 0.0f);
    const bool active = getToggleState();
    const bool hover  = isMouseOver();
    const float r = 4.0f;

    juce::Path key;
    key.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(), r, r,
                             false, false, true, true);

    juce::Colour fill;
    if (blackKey)
        fill = active ? pianoBlackKeyGleam.interpolatedWith (mintAccent, 0.55f) : pianoBlackKey;
    else
        fill = active ? (isLightChassis ? pianoWhiteKey : pianoWhiteKey.interpolatedWith (panelLight, 0.22f))
                      : pianoWhiteKey.withMultipliedAlpha (isLightChassis ? 0.55f : 0.22f);
    if (hover)
        fill = fill.brighter (0.08f);

    g.setColour (fill);
    g.fillPath (key);
    g.setColour (blackKey ? background.withAlpha (0.9f) : hairline().withMultipliedAlpha (2.0f));
    g.strokePath (key, juce::PathStrokeType (1.0f));

    if (! blackKey)
    {
        // In-scale marker: small accent bar along the bottom of the key.
        if (active)
        {
            g.setColour (mintAccent);
            g.fillRoundedRectangle (bounds.withTop (bounds.getBottom() - 3.0f).reduced (bounds.getWidth() * 0.22f, 0.0f), 1.5f);
        }
        g.setColour (active ? juce::Colour (0xff15171c) : textSecondary);
        g.setFont (HarmonyEngineUI::font (10.5f, true));
        g.drawText (note, getLocalBounds().removeFromBottom (18), juce::Justification::centred);
    }
}

TuneTab::PianoMask::PianoMask (juce::AudioProcessorValueTreeState& apvts)
{
    static const std::array<juce::String, 12> noteNames { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    static const std::array<bool, 12> isBlack           { false, true, false, true, false, false, true, false, true, false, true, false };

    for (int i = 0; i < 12; ++i)
    {
        keyButtons[(size_t) i] = std::make_unique<PianoKeyButton> (noteNames[(size_t) i], isBlack[(size_t) i]);
        addAndMakeVisible (*keyButtons[(size_t) i]);
    }

    for (int i = 0; i < 12; ++i)
        if (isBlack[(size_t) i])
            keyButtons[(size_t) i]->toFront (false);

    for (int i = 0; i < 12; ++i)
    {
        auto parameterID = "tune_scaleDegree_" + juce::String (i);
        keyAttachments[(size_t) i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
            (apvts, parameterID, *keyButtons[(size_t) i]);
    }
}

void TuneTab::PianoMask::resized()
{
    auto bounds = getLocalBounds();
    constexpr int numWhiteKeys = 7;

    const float whiteKeyWidth  = (float) bounds.getWidth() / (float) numWhiteKeys;
    const float blackKeyWidth  = whiteKeyWidth * 0.58f;
    const float blackKeyHeight = (float) bounds.getHeight() * 0.58f;

    static const std::array<int, 12> pitchClassToWhiteIndex { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

    for (int i = 0; i < 12; ++i)
    {
        auto whiteIndex = pitchClassToWhiteIndex[(size_t) i];
        if (whiteIndex >= 0)
        {
            juce::Rectangle<float> r (whiteKeyWidth * (float) whiteIndex, 0.0f,
                                      whiteKeyWidth, (float) bounds.getHeight());
            keyButtons[(size_t) i]->setBounds (r.reduced (1.0f, 0.0f).toNearestInt());
        }
    }

    static const std::array<int, 5> blackAfterWhiteIndex { 0, 1, 3, 4, 5 };
    static const std::array<int, 5> blackPitchClass       { 1, 3, 6, 8, 10 };

    for (int k = 0; k < 5; ++k)
    {
        auto centreX = whiteKeyWidth * (float) (blackAfterWhiteIndex[(size_t) k] + 1);
        juce::Rectangle<float> r (centreX - blackKeyWidth * 0.5f, 0.0f, blackKeyWidth, blackKeyHeight);
        keyButtons[(size_t) blackPitchClass[(size_t) k]]->setBounds (r.toNearestInt());
    }
}

//==============================================================================
// LIVE TUNER READOUT (shown in the editor footer)
//==============================================================================
TuneTab::NoteTriangle::NoteTriangle (const PitchTraceComponent& pitchSourceToPoll)
    : pitchSource (pitchSourceToPoll)
{
    setOpaque (false);
    setTooltip ("Live tuner: the note being sung and how far NovaTone is moving it (cents)");
    startTimerHz (30);
}

TuneTab::NoteTriangle::~NoteTriangle() { stopTimer(); }

juce::String TuneTab::NoteTriangle::noteNameForMidi (int midiNote)
{
    static const std::array<const char*, 12> names { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    int pitchClass = midiNote % 12;
    if (pitchClass < 0)
        pitchClass += 12;
    const int oct = (midiNote / 12) - 1;
    return juce::String (names[(size_t) pitchClass]) + juce::String (oct);
}

void TuneTab::NoteTriangle::timerCallback()
{
    const auto latest = pitchSource.getLatestPitch();
    const bool hadSignal = hasSignal;
    const float before = smoothedCorrectionCents;

    if (latest.isVoiced && latest.correctedPitchHz > 0.0f)
    {
        const float midi = 12.0f * std::log2 (latest.correctedPitchHz / 440.0f) + 69.0f;
        smoothedMidiNote = !hasSignal ? midi : (smoothedMidiNote + 0.35f * (midi - smoothedMidiNote));

        if (latest.uncorrectedPitchHz > 0.0f)
        {
            const float rawCents = juce::jlimit (-100.0f, 100.0f,
                1200.0f * std::log2 (latest.correctedPitchHz / latest.uncorrectedPitchHz));
            smoothedCorrectionCents = !hasSignal ? rawCents : (smoothedCorrectionCents + 0.25f * (rawCents - smoothedCorrectionCents));
        }
        hasSignal = true;
    }
    else
    {
        hasSignal = false;
    }

    if (hasSignal || hadSignal || std::abs (before - smoothedCorrectionCents) > 0.05f)
        repaint();
}

void TuneTab::NoteTriangle::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;

    auto b = getLocalBounds().toFloat().reduced (2.0f, 6.0f);
    g.setColour (panel);
    g.fillRoundedRectangle (b, 8.0f);
    g.setColour (hairline());
    g.drawRoundedRectangle (b.reduced (0.5f), 8.0f, 1.0f);

    auto inner = b.reduced (12.0f, 0.0f);

    // Note name
    const auto noteStr = hasSignal ? noteNameForMidi ((int) std::lround (smoothedMidiNote)) : juce::String ("--");
    g.setFont (HarmonyEngineUI::font (16.0f, true));
    g.setColour (hasSignal ? cyanBright : textSecondary.withAlpha (0.6f));
    g.drawText (noteStr, inner.removeFromLeft (40.0f), juce::Justification::centredLeft);

    // Cents readout
    const int cents = hasSignal ? juce::roundToInt (smoothedCorrectionCents) : 0;
    g.setFont (HarmonyEngineUI::font (11.5f, false));
    g.setColour (textSecondary);
    g.drawText (hasSignal ? ((cents > 0 ? "+" : (cents < 0 ? minusSign : juce::String())) + juce::String (std::abs (cents)) + " c")
                          : juce::String ("idle"),
                inner.removeFromRight (40.0f), juce::Justification::centredRight);

    // Correction meter: centre tick + bar towards the correction amount.
    auto track = inner.reduced (8.0f, 0.0f).withSizeKeepingCentre (inner.getWidth() - 16.0f, 4.0f);
    g.setColour (panelLight.brighter (isLightChassis ? 0.0f : 0.10f));
    g.fillRoundedRectangle (track, 2.0f);

    const float midX = track.getCentreX();
    if (hasSignal && std::abs (smoothedCorrectionCents) > 1.0f)
    {
        const float norm = juce::jlimit (-1.0f, 1.0f, smoothedCorrectionCents / 50.0f);
        const float tipX = midX + norm * (track.getWidth() * 0.5f);
        g.setColour (std::abs (smoothedCorrectionCents) < 12.0f ? mintAccent : accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (midX, tipX), track.getY(),
                                                        std::abs (tipX - midX), track.getHeight()), 2.0f);
    }
    g.setColour (textPrimary.withAlpha (0.7f));
    g.fillRect (juce::Rectangle<float> (1.5f, 10.0f).withCentre ({ midX, track.getCentreY() }));
}

//==============================================================================
// WORKSTATION MAIN VIEW
//==============================================================================
TuneTab::TuneTab (juce::AudioProcessorValueTreeState& apvts, PitchTraceQueue& queue)
    : ModuleTab       (apvts, "tune", "Pitch & time workstation", {}),
      apvtsRef        (apvts),
      scaleSelector   (apvts),
      pitchTrace      (queue),
      noteTriangle    (pitchTrace),
      pianoMask       (apvts),
      centerKnob      (apvts, "tune_center",      "Center",      "Pitch pull",      HarmonyEngineColours::accent,     "accent"),
      sensitivityKnob (apvts, "tune_sensitivity", "Sensitivity", "Note split",      HarmonyEngineColours::mintAccent, "mint"),
      variationKnob   (apvts, "tune_variation",   "Variation",   "Vibrato & drift", HarmonyEngineColours::cyanAccent, "cyan"),
      transitionKnob  (apvts, "tune_transition",  "Transition",  "Glide speed",     HarmonyEngineColours::cyanAccent, "cyan"),
      formantKnob     (apvts, "alter_formant",    "Formant",     "Throat",          HarmonyEngineColours::accent,     "accent"),
      vibratoKnob     (apvts, "tune_vibrato",     "Vibrato",     "Sustain cap",     HarmonyEngineColours::mintAccent, "mint"),
      smoothAttachment    (apvts, "tune_smooth",    smoothButton),
      lookaheadAttachment (apvts, "tune_lookahead", lookaheadButton)
{
    addAndMakeVisible (scaleSelector);
    addAndMakeVisible (pitchTrace);
    addAndMakeVisible (noteTriangle);
    addAndMakeVisible (pianoMask);

    centerKnob.setTooltipText ("Center: pulls the average pitch of every note toward the target scale note (100% = dead-center pitch).");
    sensitivityKnob.setTooltipText ("Sensitivity: how eagerly NovaTone splits phrases into separate notes and locks onto quiet syllables.");
    variationKnob.setTooltipText ("Variation: scales vibrato and pitch expression inside each note (0% = flat line, 100% = natural expression).");
    transitionKnob.setTooltipText ("Transition: how fast pitch glides from one note to the next (100% = instant snap).");
    formantKnob.setTooltipText ("Formant: shifts vocal throat character deeper (-) or brighter (+) while keeping pitch intact.");
    vibratoKnob.setTooltipText ("Vibrato: caps the maximum vibrato swing on sustained notes.");

    for (auto* k : { &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
        addAndMakeVisible (k);

    auto& laf = HarmonyEngineLookAndFeel::shared();

    //--------------------------------------------------------------------------
    // Toolbar: tools (segmented), mono/poly (segmented), status chips
    for (auto* tb : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton,
                      &monoModeButton, &polyModeButton, &midiCleanButton, &snapScaleButton, &snapGridButton, &independentButton, &autoScrollButton })
    {
        tb->setLookAndFeel (&laf);
        addAndMakeVisible (tb);
    }

    styleSegment (editToolButton, "first");
    styleSegment (cutToolButton, "middle");
    styleSegment (advancedToolButton, "middle");
    styleSegment (drawToolButton, "last");
    for (auto* tb : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton })
        tb->getProperties().set ("pillRole", "accent");

    styleSegment (monoModeButton, "first");
    styleSegment (polyModeButton, "last");
    monoModeButton.getProperties().set ("pillRole", "cyan");
    polyModeButton.getProperties().set ("pillRole", "mint");

    styleChip (snapScaleButton,  "cyan");
    styleChip (snapGridButton,   "cyan");
    snapGridButton.setTooltip ("Grid: snap slices and note edges to your DAW's 1/16 grid");
    snapGridButton.onStateChange = [this] { pitchTrace.setSnapToHostGrid (snapGridButton.getToggleState()); };
    styleChip (independentButton, "mint");
    independentButton.setTooltip ("Independent: Play / Stop run the clip on NovaTone's own playhead, ignoring FL Studio's transport and song position");
    independentButton.onStateChange = [this] { pitchTrace.setIndependentPlay (independentButton.getToggleState()); };
    styleChip (autoScrollButton, "cyan");
    styleChip (midiCleanButton,  "mint");
    midiCleanButton.setTooltip ("MIDI clean (Ctrl+M): cleans pick/fret/drum noise, bridges dropouts and stitches fragmented notes for accurate instrument MIDI.");
    snapScaleButton.setTooltip ("Snap edited notes to the active key & scale");
    autoScrollButton.setTooltip ("Follow: keep the playhead in view while audio comes in");

    editToolButton.setToggleState (true, juce::dontSendNotification);
    editToolButton.setTooltip     ("Move (Ctrl+T): drag notes up/down by semitones, Alt+drag for cents, right-click to snap to scale.");
    cutToolButton.setTooltip      ("Slice (Ctrl+S): click inside a note to split it; Ctrl+click to glue neighbouring notes.");
    advancedToolButton.setTooltip ("Shape (Ctrl+E): drag inside a note to shape its formant, variation, gain and ramps.");
    drawToolButton.setTooltip     ("Draw (Ctrl+F): paint a freehand pitch curve over the notes.");

    editToolButton.onClick     = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::StandardEdit); };
    cutToolButton.onClick      = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::CutAndGlue); };
    advancedToolButton.onClick = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::AdvancedEdit); };
    drawToolButton.onClick     = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::FreehandDraw); };

    monoModeButton.setTooltip ("Mono: single-voice detection for solo vocals (Ctrl+P to switch)");
    polyModeButton.setTooltip ("Poly: multi-voice detection for chords, choirs and strings (Ctrl+P to switch)");
    monoModeButton.setRadioGroupId (0x4e56);
    polyModeButton.setRadioGroupId (0x4e56);

    if (apvts.getParameter ("tune_polyphonic") != nullptr)
        polyModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "tune_polyphonic", polyModeButton);
    if (apvts.getParameter ("tune_midiClean") != nullptr)
        midiCleanAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "tune_midiClean", midiCleanButton);
    midiCleanButton.onStateChange = [this]
    {
        pitchTrace.setMidiCleanMode (midiCleanButton.getToggleState());
    };
    polyModeButton.onStateChange = [this]
    {
        const bool poly = polyModeButton.getToggleState();
        monoModeButton.setToggleState (! poly, juce::dontSendNotification);
        if (pitchTrace.isPolyphonicMode() != poly)
        {
            pitchTrace.setPolyphonicMode (poly);
            updateClipControls();
            layoutBody (getBodyBounds());
            repaint();
        }
    };
    monoModeButton.onClick = [this]
    {
        if (polyModeButton.getToggleState())
            polyModeButton.setToggleState (false, juce::sendNotificationSync);
        monoModeButton.setToggleState (true, juce::dontSendNotification);
    };
    monoModeButton.setToggleState (! polyModeButton.getToggleState(), juce::dontSendNotification);

    if (apvts.getParameter ("tune_snapScale") != nullptr)
        snapScaleAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "tune_snapScale", snapScaleButton);
    else
        snapScaleButton.setToggleState (true, juce::dontSendNotification);

    if (apvts.getParameter ("tune_autoScroll") != nullptr)
        autoScrollAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, "tune_autoScroll", autoScrollButton);
    else
        autoScrollButton.setToggleState (true, juce::dontSendNotification);

    snapScaleButton.onStateChange  = [this] { pitchTrace.setSnapToScale (snapScaleButton.getToggleState()); };
    autoScrollButton.onStateChange = [this] { pitchTrace.setAutoScroll (autoScrollButton.getToggleState()); };

    //--------------------------------------------------------------------------
    // Toolbar actions
    undoButton.getProperties().set ("icon", "undo");
    redoButton.getProperties().set ("icon", "redo");
    styleSegment (undoButton, "first");
    styleSegment (redoButton, "last");
    dockViewButton.getProperties().set ("icon", "dock");
    snapAllButton.getProperties().set ("accentButton", true);
    autoKeyButton.getProperties().set ("ghost", true);

    for (auto* btn : { &undoButton, &redoButton, &autoKeyButton, &levelerButton, &dockViewButton, &snapAllButton, &fileMenuButton })
    {
        btn->setLookAndFeel (&laf);
        addAndMakeVisible (btn);
    }

    undoButton.setTooltip ("Undo (Ctrl+Z)");
    undoButton.onClick = [this] { pitchTrace.undo(); };

    redoButton.setTooltip ("Redo (Ctrl+Y)");
    redoButton.onClick = [this] { pitchTrace.redo(); };

    autoKeyButton.setTooltip ("Detect key & scale from the notes on the piano roll (Ctrl+K)");
    autoKeyButton.onClick = [this] { pitchTrace.autoDetectKeyAndApply(); };

    levelerButton.setTooltip ("Level (Ctrl+L): balance quiet and loud notes toward the phrase average");
    levelerButton.onClick = [this] { pitchTrace.levelAllNotesVolume (0.72f); };

    dockViewButton.setTooltip ("Show / hide the panels under the piano roll (Ctrl+B)");
    dockViewButton.onClick = [this] { cycleDockLayoutPreset(); };

    snapAllButton.setTooltip ("Snap every note on the piano roll to the active key & scale");
    snapAllButton.onClick = [this] { pitchTrace.snapAllNotesToScale(); };

    fileMenuButton.setTooltip ("Load audio, export MIDI or WAV, clear the piano roll");
    fileMenuButton.onClick = [this] { showFileMenu(); };

    // Poly clip controls (visible in Poly mode)
    styleChip (recordButton, "rec");
    recordButton.setClickingTogglesState (false);
    recordButton.setTooltip ("Record a clip: arm, then press play in your DAW. Press again to stop and analyse. Then drag notes to edit chords.");
    recordButton.setLookAndFeel (&laf);
    addChildComponent (recordButton);
    recordButton.onClick = [this]
    {
        if (clipSession == nullptr) return;
        if (clipSession->getStatus() == Poly::ClipSession::Status::recording) clipSession->stopRecording();
        else clipSession->startRecording();
        updateClipControls();
    };
    clipPlayButton.getProperties().set ("icon", "play");
    clipStopButton.getProperties().set ("icon", "stop");
    clipToStartButton.getProperties().set ("icon", "toStart");
    clipPlayButton.setTooltip ("Play the edited clip here (your DAW's transport also plays it in place)");
    clipStopButton.setTooltip ("Stop the clip preview");
    clipToStartButton.setTooltip ("Back to the start of the clip");
    for (auto* b : { &clipPlayButton, &clipStopButton, &clipToStartButton })
    {
        b->setLookAndFeel (&laf);
        addChildComponent (b);
    }
    clipPlayButton.onClick = [this] { pitchTrace.setPreviewPlaying (true); };
    clipStopButton.onClick = [this] { pitchTrace.setPreviewPlaying (false); };
    clipToStartButton.onClick = [this] { pitchTrace.seekClipToStart(); };

    //--------------------------------------------------------------------------
    // Selected-note inspector
    inspectorNoteLabel.setFont (HarmonyEngineUI::font (14.0f, true));
    inspectorNoteLabel.setColour (juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    inspectorNoteLabel.setJustificationType (juce::Justification::centredLeft);
    inspectorNoteLabel.setBorderSize ({});
    addAndMakeVisible (inspectorNoteLabel);

    semiDownButton.setButtonText (minusSign);
    semiUpButton.setButtonText ("+");
    styleSegment (semiDownButton, "first");
    styleSegment (semiUpButton, "last");
    resetNoteButton.getProperties().set ("ghost", true);
    for (auto* b : { &semiDownButton, &semiUpButton, &resetNoteButton })
    {
        b->setLookAndFeel (&laf);
        addAndMakeVisible (b);
    }
    semiDownButton.setTooltip ("Transpose the selected note down one step (Down arrow)");
    semiUpButton.setTooltip ("Transpose the selected note up one step (Up arrow)");
    resetNoteButton.setTooltip ("Restore the selected note's original pitch, formant, variation and gain");

    semiDownButton.onClick  = [this] { pitchTrace.transposeSelectedNote (-1); };
    semiUpButton.onClick    = [this] { pitchTrace.transposeSelectedNote (1); };
    resetNoteButton.onClick = [this] { pitchTrace.resetSelectedNote(); };

    auto setupInspectorSlider = [&] (juce::Slider& s, const juce::String& caption, double minV, double maxV, double defV,
                                     const juce::String& suffix, const juce::String& tip, const char* role)
    {
        s.setSliderStyle (juce::Slider::LinearBar);
        s.setSliderSnapsToMousePosition (false);
        s.setMouseDragSensitivity (300);
        s.setRange (minV, maxV, 0.1);
        s.setValue (defV, juce::dontSendNotification);
        s.setDoubleClickReturnValue (true, defV);
        s.setTextValueSuffix (suffix);
        s.setNumDecimalPlacesToDisplay (1);
        s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        s.getProperties().set ("field", true);
        s.getProperties().set ("caption", caption);
        s.getProperties().set ("ringRole", role);
        s.setLookAndFeel (&laf);
        s.setTooltip (tip + " (double-click to reset)");
        addAndMakeVisible (s);
    };

    setupInspectorSlider (noteCentsSlider,   "Cents",     -50.0, 50.0,  0.0,   " c",  "Fine pitch offset for the selected note, in cents", "cyan");
    setupInspectorSlider (noteFormantSlider, "Formant",   -12.0, 12.0,  0.0,   " st", "Formant shift for the selected note, in semitones", "accent");
    setupInspectorSlider (noteVarSlider,     "Variation",   0.0, 200.0, 100.0, " %",  "Vibrato & pitch variation for the selected note", "cyan");
    setupInspectorSlider (noteGainSlider,    "Gain",      -18.0, 12.0,  0.0,   " dB", "Volume trim for the selected note", "mint");
    setupInspectorSlider (noteSibSlider,     "Breath",    -18.0, 12.0,  0.0,   " dB", "Breath & sibilance balance (>3.8 kHz) for the selected note", "mint");
    noteVarSlider.setNumDecimalPlacesToDisplay (0);

    noteCentsSlider.onValueChange = [this]
    {
        if (!updatingInspector)
            pitchTrace.setSelectedNoteFineCents ((float) noteCentsSlider.getValue());
    };
    noteFormantSlider.onValueChange = [this]
    {
        if (!updatingInspector)
            pitchTrace.setSelectedNoteFormant ((float) noteFormantSlider.getValue());
    };
    noteVarSlider.onValueChange = [this]
    {
        if (!updatingInspector)
            pitchTrace.setSelectedNoteVariation ((float) (noteVarSlider.getValue() * 0.01));
    };
    noteGainSlider.onValueChange = [this]
    {
        if (!updatingInspector)
            pitchTrace.setSelectedNoteGainDb ((float) noteGainSlider.getValue());
    };
    noteSibSlider.onValueChange = [this]
    {
        if (!updatingInspector)
            pitchTrace.setSelectedNoteSibilantDb ((float) noteSibSlider.getValue());
    };

    pitchTrace.onNoteSelectionChanged = [this] { refreshSelectedNoteInspector(); };
    pitchTrace.onToolModeChanged      = [this] (PitchTraceComponent::ToolMode m) { setActiveToolMode (m); };
    pitchTrace.onCycleDockShortcut    = [this] { cycleDockLayoutPreset(); };
    pitchTrace.onTogglePolyphonicShortcut = [this]
    {
        polyModeButton.setToggleState (!polyModeButton.getToggleState(), juce::sendNotificationSync);
    };
    pitchTrace.onToggleMidiCleanShortcut = [this]
    {
        midiCleanButton.setToggleState (!midiCleanButton.getToggleState(), juce::sendNotificationSync);
    };
    pitchTrace.onAutoKeyDetected = [this] (int root, int mode)
    {
        setParamNormalised ("tune_root", (float) root);
        setParamNormalised ("tune_mode", (float) mode);
    };
    refreshSelectedNoteInspector();

    //--------------------------------------------------------------------------
    // Style card: one-click styles, humanize, auto-smooth, lookahead
    styleSegment (styleNaturalButton, "first");
    styleSegment (stylePopButton, "middle");
    styleSegment (styleHardButton, "last");
    for (auto* sb : { &styleNaturalButton, &stylePopButton, &styleHardButton })
    {
        sb->setLookAndFeel (&laf);
        addAndMakeVisible (sb);
    }
    styleNaturalButton.setTooltip ("Natural: soft, transparent tuning (Center 58%, Variation 95%, Transition 26%, Sensitivity 52%, Humanize 50%)");
    stylePopButton.setTooltip     ("Pop: tight, polished modern pop (Center 92%, Variation 55%, Transition 78%, Sensitivity 72%, Humanize 18%)");
    styleHardButton.setTooltip    ("Hard tune: instant hard snap (Center 100%, Variation 0%, Transition 100%, Sensitivity 85%, Humanize 0%)");

    styleNaturalButton.onClick = [this] { applyQuickVocalStyle (0.58f, 0.52f, 0.95f, 0.26f, 45.0f, 0.50f); };
    stylePopButton.onClick     = [this] { applyQuickVocalStyle (0.92f, 0.72f, 0.55f, 0.78f, 18.0f, 0.18f); };
    styleHardButton.onClick    = [this] { applyQuickVocalStyle (1.00f, 0.85f, 0.00f, 1.00f, 0.0f,  0.00f); };

    humanizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    humanizeSlider.setRange (0.0, 1.0, 0.01);
    humanizeSlider.setDoubleClickReturnValue (true, 0.50);
    humanizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    humanizeSlider.setColour (juce::Slider::textBoxTextColourId, HarmonyEngineColours::textPrimary);
    humanizeSlider.getProperties().set ("ringRole", "mint");
    humanizeSlider.setLookAndFeel (&laf);
    humanizeSlider.setTooltip ("Humanize: 0% = off, 50% = standard natural humanize, 100% = extreme (steeper leaps, deeper scoops, organic vibrato bloom). Double-click for 50%.");
    addAndMakeVisible (humanizeSlider);
    if (apvts.getParameter ("tune_humanizeAmount") != nullptr)
        humanizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "tune_humanizeAmount", humanizeSlider);

    pitchTrace.onToggleHumanizeShortcut = [this]
    {
        const float cur = (float) humanizeSlider.getValue();
        const float next = (cur < 0.25f) ? 0.50f : ((cur < 0.75f) ? 1.00f : 0.00f);
        setParamNormalised ("tune_humanizeAmount", next);
    };

    smoothButton.setButtonText ("Auto-smooth");
    styleChip (smoothButton, "mint");
    smoothButton.setLookAndFeel (&laf);
    smoothButton.setTooltip ("Auto-smooth: hands-free natural lookahead tuning (locks the manual controls)");
    addAndMakeVisible (smoothButton);

    lookaheadButton.setButtonText ("Lookahead");
    styleChip (lookaheadButton, "cyan");
    lookaheadButton.setLookAndFeel (&laf);
    lookaheadButton.setTooltip ("Lookahead: inspects incoming pitch 60 ms ahead for zero-lag note transitions");
    addAndMakeVisible (lookaheadButton);

    smoothButton.onStateChange = [this] { updateControlStates(); };
    updateControlStates();

    pitchTrace.setFrequencyRange (80.0f, 1000.0f);

    // The attachments restored these toggles before their handlers existed: sync the roll now.
    pitchTrace.setSnapToScale (snapScaleButton.getToggleState());
    pitchTrace.setAutoScroll (autoScrollButton.getToggleState());
    pitchTrace.setMidiCleanMode (midiCleanButton.getToggleState());
    pitchTrace.setPolyphonicMode (polyModeButton.getToggleState());
    monoModeButton.setToggleState (! polyModeButton.getToggleState(), juce::dontSendNotification);
}

TuneTab::~TuneTab()
{
    for (auto* tb : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton, &monoModeButton, &polyModeButton,
                      &midiCleanButton, &snapScaleButton, &snapGridButton, &independentButton, &autoScrollButton, &smoothButton, &lookaheadButton, &recordButton })
        tb->setLookAndFeel (nullptr);
    pitchTrace.onClipStatusChanged = nullptr;

    for (auto* btn : { &undoButton, &redoButton, &autoKeyButton, &levelerButton, &dockViewButton, &snapAllButton, &fileMenuButton,
                       &clipPlayButton, &clipStopButton, &clipToStartButton, &semiDownButton, &semiUpButton, &resetNoteButton,
                       &styleNaturalButton, &stylePopButton, &styleHardButton })
        btn->setLookAndFeel (nullptr);

    for (auto* s : { &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &humanizeSlider })
        s->setLookAndFeel (nullptr);
}

void TuneTab::setClipSession (Poly::ClipSession* session)
{
    clipSession = session;
    pitchTrace.setClipSession (session);
    pitchTrace.onClipStatusChanged = [this] { updateClipControls(); };
    updateClipControls();
    layoutBody (getBodyBounds());
}

void TuneTab::updateClipControls()
{
    const bool poly = polyModeButton.getToggleState() && clipSession != nullptr;
    recordButton.setVisible (poly);
    for (auto* b : { &clipPlayButton, &clipStopButton, &clipToStartButton })
    {
        b->setVisible (true);
        b->setEnabled (pitchTrace.hasPreviewClip());
    }
    if (clipSession == nullptr) return;
    const auto st = clipSession->getStatus();
    recordButton.setToggleState (st == Poly::ClipSession::Status::recording, juce::dontSendNotification);
    recordButton.setButtonText (st == Poly::ClipSession::Status::recording ? "Stop" : "Rec");
}

void TuneTab::refreshTheme()
{
    inspectorNoteLabel.setColour (juce::Label::textColourId, HarmonyEngineColours::textPrimary);
    for (auto* k : { &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
        k->refreshThemeColours();
    for (auto* s : { &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &humanizeSlider })
        s->setColour (juce::Slider::textBoxTextColourId, HarmonyEngineColours::textPrimary);
    pitchTrace.repaint();
    pianoMask.repaint();
    noteTriangle.repaint();
    repaint();
}

void TuneTab::setParamNormalised (const juce::String& paramId, float plainValue)
{
    if (auto* p = apvtsRef.getParameter (paramId))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void TuneTab::applyQuickVocalStyle (float center, float sensitivity, float variation, float transition, float vibratoCents, float humanize)
{
    setParamNormalised ("tune_smooth",         0.0f);
    setParamNormalised ("tune_humanizeAmount", humanize);
    setParamNormalised ("tune_center",         center);
    setParamNormalised ("tune_sensitivity",    sensitivity);
    setParamNormalised ("tune_variation",      variation);
    setParamNormalised ("tune_transition",     transition);
    setParamNormalised ("tune_flexAmount",     transition);
    setParamNormalised ("tune_vibrato",        vibratoCents);
}

void TuneTab::refreshSelectedNoteInspector()
{
    const juce::ScopedValueSetter<bool> guard (updatingInspector, true);
    const auto* sel = pitchTrace.getSelectedNote();
    const bool hasVoicedNote = (sel != nullptr && !sel->isSibilant);

    for (auto* c : std::initializer_list<juce::Component*> {
            &semiDownButton, &semiUpButton, &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &resetNoteButton })
        c->setEnabled (hasVoicedNote);

    if (hasVoicedNote)
    {
        inspectorNoteLabel.setText (PitchTraceComponent::midiNoteName (sel->targetMidi), juce::dontSendNotification);
        inspectorNoteLabel.setColour (juce::Label::textColourId, HarmonyEngineColours::textPrimary);
        noteCentsSlider.setValue   (sel->fineCents,               juce::dontSendNotification);
        noteFormantSlider.setValue (sel->formantSemitones,        juce::dontSendNotification);
        noteVarSlider.setValue     (sel->noteVariation * 100.0f,  juce::dontSendNotification);
        noteGainSlider.setValue    (sel->gainDb,                  juce::dontSendNotification);
        noteSibSlider.setValue     (sel->sibilantDb,              juce::dontSendNotification);
    }
    else
    {
        inspectorNoteLabel.setText ("None", juce::dontSendNotification);
        inspectorNoteLabel.setColour (juce::Label::textColourId, HarmonyEngineColours::textSecondary);
        noteCentsSlider.setValue   (0.0,   juce::dontSendNotification);
        noteFormantSlider.setValue (0.0,   juce::dontSendNotification);
        noteVarSlider.setValue     (100.0, juce::dontSendNotification);
        noteGainSlider.setValue    (0.0,   juce::dontSendNotification);
        noteSibSlider.setValue     (0.0,   juce::dontSendNotification);
    }
    repaint (inspectorPanelArea);
}

void TuneTab::setActiveToolMode (PitchTraceComponent::ToolMode mode)
{
    editToolButton.setToggleState     (mode == PitchTraceComponent::ToolMode::StandardEdit, juce::dontSendNotification);
    cutToolButton.setToggleState      (mode == PitchTraceComponent::ToolMode::CutAndGlue,   juce::dontSendNotification);
    advancedToolButton.setToggleState (mode == PitchTraceComponent::ToolMode::AdvancedEdit, juce::dontSendNotification);
    drawToolButton.setToggleState     (mode == PitchTraceComponent::ToolMode::FreehandDraw, juce::dontSendNotification);
    pitchTrace.setToolMode (mode);
}

bool TuneTab::keyPressed (const juce::KeyPress& key)
{
    return pitchTrace.keyPressed (key);
}

void TuneTab::updateControlStates()
{
    const bool smooth = smoothButton.getToggleState();

    for (auto* c : std::initializer_list<juce::Component*> {
            &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &vibratoKnob, &humanizeSlider, &lookaheadButton })
    {
        c->setEnabled (!smooth);
        c->setAlpha (smooth ? 0.45f : 1.0f);
    }
    repaint();
}

void TuneTab::showFileMenu()
{
    juce::PopupMenu menu;
    menu.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    const bool poly = pitchTrace.isClipMode();
    menu.addItem (1, poly ? "Load audio into poly clip..." : "Load audio...");
    menu.addSeparator();
    menu.addItem (2, "Export MIDI...");
    menu.addItem (3, poly ? "Export edited clip (WAV)..." : "Export WAV...", true, false);
    menu.addSeparator();
    menu.addItem (4, poly ? "Clear poly clip" : "Clear piano roll");

    juce::Component::SafePointer<TuneTab> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileMenuButton).withMinimumWidth (180),
        [safeThis] (int result)
        {
            if (safeThis == nullptr) return;
            auto& pt = safeThis->pitchTrace;
            switch (result)
            {
                case 1: pt.promptLoadAudioFile();  break;
                case 2: pt.promptExportMidiFile(); break;
                case 3: pt.promptExportWavFile();  break;
                case 4: pt.clearTimeline();        break;
                default: break;
            }
        });
}

//==============================================================================
// Lower-area visibility
//==============================================================================
void TuneTab::setInspectorVisibility (bool visible)
{
    for (auto* c : std::initializer_list<juce::Component*> {
            &inspectorNoteLabel, &semiDownButton, &semiUpButton,
            &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &resetNoteButton })
        c->setVisible (visible);
}

void TuneTab::setDockVisibility (bool visible)
{
    for (auto* c : std::initializer_list<juce::Component*> {
            &scaleSelector, &autoKeyButton, &pianoMask,
            &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob,
            &styleNaturalButton, &stylePopButton, &styleHardButton, &humanizeSlider, &smoothButton, &lookaheadButton })
        c->setVisible (visible);
}

void TuneTab::cycleDockLayoutPreset()
{
    // Everything -> controls only -> piano roll only -> everything
    if (showInspector && showDock)        { showInspector = false; }
    else if (! showInspector && showDock) { showDock = false; }
    else                                  { showInspector = true; showDock = true; }

    layoutBody (getBodyBounds());
    repaint();
}

//==============================================================================
// Splitter: drag down to hide the lower panels, up to bring them back;
// double-click toggles a full-height piano roll.
//==============================================================================
void TuneTab::mouseMove (const juce::MouseEvent& e)
{
    const bool over = splitterBarArea.expanded (0, 2).contains (e.position.toInt());
    if (over != splitterHovered)
    {
        splitterHovered = over;
        repaint (splitterBarArea.expanded (0, 4));
    }
    setMouseCursor (over ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void TuneTab::mouseExit (const juce::MouseEvent&)
{
    if (splitterHovered)
    {
        splitterHovered = false;
        repaint (splitterBarArea.expanded (0, 4));
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TuneTab::mouseDown (const juce::MouseEvent& e)
{
    draggingSplitter = splitterBarArea.expanded (0, 2).contains (e.position.toInt());
    dragStartMouseY  = e.position.y;
}

void TuneTab::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingSplitter)
        return;

    const float dy = e.position.y - dragStartMouseY;
    const bool anyShown = showInspector || showDock;

    if (anyShown && dy > 60.0f)
    {
        showInspector = showDock = false;
        dragStartMouseY = e.position.y;
        layoutBody (getBodyBounds());
        repaint();
    }
    else if (! anyShown && dy < -60.0f)
    {
        showInspector = showDock = true;
        dragStartMouseY = e.position.y;
        layoutBody (getBodyBounds());
        repaint();
    }
}

void TuneTab::mouseUp (const juce::MouseEvent&)
{
    draggingSplitter = false;
}

void TuneTab::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! splitterBarArea.expanded (0, 3).contains (e.position.toInt()))
        return;

    const bool anyShown = showInspector || showDock;
    showInspector = showDock = ! anyShown;
    layoutBody (getBodyBounds());
    repaint();
}

//==============================================================================
// Key / scale / range selector (lives in the Scale card)
//==============================================================================
TuneTab::ScaleSelector::ScaleSelector (juce::AudioProcessorValueTreeState& apvts)
{
    auto setUp = [&] (juce::ComboBox& box, const juce::String& paramId, const juce::String& caption, const juce::String& tooltip,
                      std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment,
                      juce::StringArray displayNames = {})
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramId)))
            box.addItemList (displayNames.size() == choice->choices.size() ? displayNames : choice->choices, 1);
        box.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
        box.setJustificationType (juce::Justification::centredLeft);
        box.getProperties().set ("caption", caption);
        box.setTooltip (tooltip);
        addAndMakeVisible (box);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, paramId, box);
    };

    setUp (rootBox,  "tune_root",       "Key",   "Key: root note of the song", rootAttachment);
    setUp (modeBox,  "tune_mode",       {}, "Scale: major, minor, or a custom 12-key scale", modeAttachment);
    setUp (rangeBox, "tune_vocalRange", {}, "Vocal range lock: full, soprano, alto, tenor or bass", rangeAttachment,
           { "Full range", "Soprano", "Alto", "Tenor", "Bass" });
}

TuneTab::ScaleSelector::~ScaleSelector()
{
    rangeBox.setLookAndFeel (nullptr);
    rootBox.setLookAndFeel (nullptr);
    modeBox.setLookAndFeel (nullptr);
}

void TuneTab::ScaleSelector::paint (juce::Graphics&) {}

void TuneTab::ScaleSelector::resized()
{
    auto r = getLocalBounds();
    const int gap = 6;
    const int w = r.getWidth() - 2 * gap;
    rootBox.setBounds  (r.removeFromLeft (juce::roundToInt ((float) w * 0.30f)));
    r.removeFromLeft (gap);
    modeBox.setBounds  (r.removeFromLeft (juce::roundToInt ((float) w * 0.31f)));
    r.removeFromLeft (gap);
    rangeBox.setBounds (r);
}

//==============================================================================
// Painting
//==============================================================================
void TuneTab::paintBody (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;

    // Selected-note inspector strip
    if (! inspectorPanelArea.isEmpty())
    {
        HarmonyEngineUI::drawLiftedPanel (g, inspectorPanelArea, 8.0f);
        g.setFont (HarmonyEngineUI::caption (10.5f));
        g.setColour (textSecondary);
        g.drawText ("NOTE", noteLabelArea, juce::Justification::centredLeft);
    }

    // Dock cards
    if (! scaleCardArea.isEmpty())
    {
        HarmonyEngineUI::drawLiftedPanel (g, scaleCardArea, 10.0f);
        HarmonyEngineUI::drawLiftedPanel (g, knobCardArea, 10.0f);
        HarmonyEngineUI::drawLiftedPanel (g, styleCardArea, 10.0f);
        HarmonyEngineUI::drawCardTitle (g, scaleTitleArea, "Scale");
        HarmonyEngineUI::drawCardTitle (g, knobTitleArea, smoothButton.getToggleState() ? "Correction  (auto-smooth on)" : "Correction");
        HarmonyEngineUI::drawCardTitle (g, styleTitleArea, "Style");

        g.setFont (HarmonyEngineUI::font (12.0f, false));
        g.setColour (textSecondary);
        g.drawText ("Humanize", humanizeLabelArea, juce::Justification::centredLeft);
    }

    // Splitter grip (only visible while hovered or dragged, so the layout stays quiet)
    if (! splitterBarArea.isEmpty())
    {
        const bool active = splitterHovered || draggingSplitter;
        const bool collapsed = ! (showInspector || showDock);
        const auto grip = juce::Rectangle<float> (36.0f, 4.0f).withCentre (splitterBarArea.toFloat().getCentre());
        g.setColour ((active ? textPrimary.withAlpha (0.45f) : hairline().withMultipliedAlpha (collapsed ? 2.5f : 1.6f)));
        g.fillRoundedRectangle (grip, 2.0f);
    }
}

//==============================================================================
// Layout
//==============================================================================
void TuneTab::layoutBody (juce::Rectangle<int> bounds)
{
    //--------------------------------------------------------------------------
    // 1. Toolbar
    auto tb = bounds.removeFromTop (toolbarHeight);
    {
        // Widths shrink together on narrow windows so nothing gets clipped.
        const bool clipCtl = recordButton.isVisible();
        const int fixedW    = 4 * 34 + (10 + 12 + 12 + 6 + 6 + 12) + (6 + 10 + 6) + 8 + 3 * 34 + 3 * 4 + 8 + (clipCtl ? 4 : 0) + 6 + 6;
        const int flexibleW = 4 * 62 + 2 * 58 + 66 + 74 + 96 + 76 + 62 + 66 + 66 + 66 + (clipCtl ? 66 : 0);
        const float k = juce::jlimit (0.70f, 1.0f, (float) (tb.getWidth() - fixedW) / (float) flexibleW);
        auto w = [k] (int px) { return juce::roundToInt ((float) px * k); };

        header.setBounds (tb.removeFromLeft (34));
        tb.removeFromLeft (10);
        for (auto* b : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton })
            b->setBounds (tb.removeFromLeft (w (62)));
        tb.removeFromLeft (12);
        monoModeButton.setBounds (tb.removeFromLeft (w (58)));
        polyModeButton.setBounds (tb.removeFromLeft (w (58)));
        tb.removeFromLeft (12);
        if (recordButton.isVisible())
        {
            recordButton.setBounds (tb.removeFromLeft (w (66)));
            tb.removeFromLeft (4);
        }
        for (auto* b : { &clipPlayButton, &clipStopButton, &clipToStartButton })
        {
            b->setBounds (tb.removeFromLeft (34));
            tb.removeFromLeft (4);
        }
        independentButton.setBounds (tb.removeFromLeft (w (66)));
        tb.removeFromLeft (8);
        snapScaleButton.setBounds (tb.removeFromLeft (w (66)));
        tb.removeFromLeft (6);
        snapGridButton.setBounds (tb.removeFromLeft (w (66)));
        tb.removeFromLeft (6);
        autoScrollButton.setBounds (tb.removeFromLeft (w (74)));
        tb.removeFromLeft (6);
        midiCleanButton.setBounds (tb.removeFromLeft (w (96)));
        tb.removeFromLeft (12);
        undoButton.setBounds (tb.removeFromLeft (34));
        redoButton.setBounds (tb.removeFromLeft (34));

        fileMenuButton.setBounds (tb.removeFromRight (w (66)));
        tb.removeFromRight (6);
        dockViewButton.setBounds (tb.removeFromRight (34));
        tb.removeFromRight (10);
        levelerButton.setBounds (tb.removeFromRight (w (62)));
        tb.removeFromRight (6);
        snapAllButton.setBounds (tb.removeFromRight (w (76)));
    }
    bounds.removeFromTop (10);

    //--------------------------------------------------------------------------
    // 2. Lower panels (bottom-up)
    const bool roomForDock = bounds.getHeight() >= 200 + dockHeight + inspectorHeight;
    const bool dockOn = showDock && roomForDock;
    const bool inspOn = showInspector && bounds.getHeight() >= 200 + inspectorHeight;

    scaleCardArea = knobCardArea = styleCardArea = {};
    inspectorPanelArea = {};

    setDockVisibility (dockOn);
    if (dockOn)
    {
        auto dock = bounds.removeFromBottom (dockHeight);
        bounds.removeFromBottom (8);

        const float side = juce::jlimit (0.86f, 1.0f, (float) dock.getWidth() / 1016.0f);
        scaleCardArea = dock.removeFromLeft (juce::roundToInt (296.0f * side));
        dock.removeFromLeft (8);
        styleCardArea = dock.removeFromRight (juce::roundToInt (268.0f * side));
        dock.removeFromRight (8);
        knobCardArea = dock;

        // Scale card: title + Detect, key/scale/range, 12-key mask
        {
            auto c = scaleCardArea.reduced (cardPad);
            auto titleRow = c.removeFromTop (cardTitleH + 4);
            autoKeyButton.setBounds (titleRow.removeFromRight (64).withSizeKeepingCentre (64, 22));
            scaleTitleArea = titleRow;
            c.removeFromTop (6);
            scaleSelector.setBounds (c.removeFromTop (28));
            c.removeFromTop (8);
            pianoMask.setBounds (c);
        }

        // Correction card: six knobs
        {
            auto c = knobCardArea.reduced (cardPad);
            knobTitleArea = c.removeFromTop (cardTitleH + 4);
            c.removeFromTop (4);
            const int colW = c.getWidth() / 6;
            for (auto* k : { &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
                k->setBounds (c.removeFromLeft (colW).reduced (2, 0));
        }

        // Style card: styles, humanize, chips
        {
            auto c = styleCardArea.reduced (cardPad);
            styleTitleArea = c.removeFromTop (cardTitleH + 4);
            c.removeFromTop (6);
            auto styles = c.removeFromTop (28);
            const int sw = styles.getWidth() / 3;
            styleNaturalButton.setBounds (styles.removeFromLeft (sw));
            stylePopButton.setBounds (styles.removeFromLeft (sw));
            styleHardButton.setBounds (styles);
            c.removeFromTop (10);
            auto hum = c.removeFromTop (24);
            humanizeLabelArea = hum.removeFromLeft (64);
            humanizeSlider.setBounds (hum);
            c.removeFromTop (10);
            auto chips = c.removeFromTop (28);
            const int cw = (chips.getWidth() - 6) / 2;
            smoothButton.setBounds (chips.removeFromLeft (cw));
            chips.removeFromLeft (6);
            lookaheadButton.setBounds (chips);
        }
    }

    setInspectorVisibility (inspOn);
    if (inspOn)
    {
        inspectorPanelArea = bounds.removeFromBottom (inspectorHeight);
        auto ir = inspectorPanelArea.reduced (12, 6);

        noteLabelArea = ir.removeFromLeft (38);
        inspectorNoteLabel.setBounds (ir.removeFromLeft (52));
        semiDownButton.setBounds (ir.removeFromLeft (28));
        semiUpButton.setBounds (ir.removeFromLeft (28));
        ir.removeFromLeft (16);

        resetNoteButton.setBounds (ir.removeFromRight (60));
        ir.removeFromRight (12);

        const int gap = 6;
        const int fieldW = (ir.getWidth() - 4 * gap) / 5;
        for (auto* sl : { &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider })
        {
            sl->setBounds (ir.removeFromLeft (fieldW));
            ir.removeFromLeft (gap);
        }
    }

    //--------------------------------------------------------------------------
    // 3. Splitter + piano roll (everything that's left)
    splitterBarArea = bounds.removeFromBottom (splitterHeight);
    pitchTrace.setBounds (bounds);
    bypassVeilArea = bounds;
}
