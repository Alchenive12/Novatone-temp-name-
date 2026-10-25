#include "TuneTab.h"
#include "HarmonyEngineColours.h"
#include <cmath>

//==============================================================================
namespace
{
    constexpr int toolbarHeight   = 38;
    constexpr int splitterHeight  = 10;
    constexpr int baseMiddleH     = 76;
    constexpr int baseKnobH       = 152;
}

//==============================================================================
TuneTab::PianoMask::PianoKeyButton::PianoKeyButton (juce::String noteName, bool isBlackKeyToDraw)
    : note (std::move (noteName)), blackKey (isBlackKeyToDraw)
{
    setTooltip ("Toggle " + note + " in or out of the active scale");
}

void TuneTab::PianoMask::PianoKeyButton::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;

    auto bounds = getLocalBounds().toFloat();
    const bool active = getToggleState();

    if (!active)
    {
        g.setColour(blackKey ? backgroundDeep : panel);
        g.fillRoundedRectangle(bounds, 3.5f);

        g.setColour(knobOutline.withAlpha(0.45f));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 3.5f, 1.0f);

        g.setColour(textSecondary.withAlpha(0.45f));
        g.setFont(HarmonyEngineUI::font(9.5f, true));
        g.drawText(note, getLocalBounds().removeFromBottom(15), juce::Justification::centred);
        return;
    }

    const auto keyCol = blackKey ? pianoBlackKeyGleam.interpolatedWith(mintAccent, 0.38f)
                                 : pianoWhiteKey.interpolatedWith(mintAccent, 0.22f);
    g.setColour(keyCol);
    g.fillRoundedRectangle(bounds, 3.5f);

    g.setColour(mintAccent);
    g.drawRoundedRectangle(bounds.reduced(0.75f), 3.5f, 1.5f);

    const float ledRadius = 3.0f;
    const juce::Point<float> ledCentre(bounds.getCentreX(), bounds.getY() + ledRadius + 4.0f);
    g.setColour(mintAccent.brighter(0.35f));
    g.fillEllipse(ledCentre.x - ledRadius, ledCentre.y - ledRadius, ledRadius * 2.0f, ledRadius * 2.0f);

    g.setColour(blackKey ? juce::Colours::white : juce::Colour(0xff0b0d13));
    g.setFont(HarmonyEngineUI::font(9.5f, true));
    g.drawText(note, getLocalBounds().removeFromBottom(15), juce::Justification::centred);
}

//==============================================================================
TuneTab::PianoMask::PianoMask (juce::AudioProcessorValueTreeState& apvts)
{
    setBufferedToImage (true);
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
    const float blackKeyWidth  = whiteKeyWidth * 0.62f;
    const float blackKeyHeight = (float) bounds.getHeight() * 0.62f;

    static const std::array<int, 12> pitchClassToWhiteIndex { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

    for (int i = 0; i < 12; ++i)
    {
        auto whiteIndex = pitchClassToWhiteIndex[(size_t) i];
        if (whiteIndex >= 0)
        {
            juce::Rectangle<float> r (whiteKeyWidth * (float) whiteIndex, 0.0f,
                                       whiteKeyWidth, (float) bounds.getHeight());
            keyButtons[(size_t) i]->setBounds (r.reduced (1.5f, 0.0f).toNearestInt());
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
// LIVE DIGITAL PITCH & CENTS TUNER MONITOR
//==============================================================================
TuneTab::NoteTriangle::NoteTriangle (const PitchTraceComponent& pitchSourceToPoll)
    : pitchSource (pitchSourceToPoll)
{
    setOpaque (false);
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

    repaint();
}

void TuneTab::NoteTriangle::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;

    const auto b = getLocalBounds().toFloat();
    HarmonyEngineUI::drawInsetGlass (g, getLocalBounds(), 10.0f);

    auto inner = b.reduced (10.0f, 4.0f);
    if (inner.getHeight() >= 48.0f)
    {
        g.setFont (HarmonyEngineUI::font (9.0f, true, 0.16f));
        g.setColour (isLightChassis ? glow.withAlpha(0.8f) : textSecondary);
        g.drawText ("LIVE TUNER", inner.removeFromTop (13.0f), juce::Justification::centred);
    }

    const auto noteStr = hasSignal ? noteNameForMidi ((int) std::lround (smoothedMidiNote)) : juce::String ("--");
    const float noteFontH = juce::jlimit (15.0f, 22.0f, inner.getHeight() * 0.56f);
    g.setFont (HarmonyEngineUI::font (noteFontH, true, 0.06f));
    g.setColour (hasSignal ? cyanBright : juce::Colours::white.withAlpha (0.45f));
    g.drawText (noteStr, inner.removeFromTop (juce::jmin (28.0f, inner.getHeight() - 10.0f)), juce::Justification::centred);

    auto barTrack = inner.withSizeKeepingCentre (inner.getWidth() - 8.0f, 6.0f);
    g.setColour (graphGridLineBright);
    g.fillRoundedRectangle (barTrack, 3.0f);

    const float midX = barTrack.getCentreX();
    g.setColour (mintAccent);
    g.drawVerticalLine (juce::roundToInt (midX), barTrack.getY() - 2.0f, barTrack.getBottom() + 2.0f);

    if (hasSignal && std::abs (smoothedCorrectionCents) > 1.0f)
    {
        const float norm = juce::jlimit (-1.0f, 1.0f, smoothedCorrectionCents / 50.0f);
        const float tipX = midX + norm * (barTrack.getWidth() * 0.48f);
        const auto fill = juce::Rectangle<float> (juce::jmin (midX, tipX), barTrack.getY(),
                                                  std::abs (tipX - midX), barTrack.getHeight());
        g.setColour (std::abs (smoothedCorrectionCents) < 12.0f ? mintAccent : accent);
        g.fillRoundedRectangle (fill, 3.0f);
    }
}

//==============================================================================
// WORKSTATION MAIN VIEW
//==============================================================================
TuneTab::TuneTab (juce::AudioProcessorValueTreeState& apvts, PitchTraceQueue& queue)
    : ModuleTab       (apvts, "tune", "PITCH & TIME WORKSTATION", "DRAG NOTES OR USE 1-CLICK VOCAL STYLES"),
      apvtsRef        (apvts),
      scaleSelector   (apvts),
      pitchTrace      (queue),
      noteTriangle    (pitchTrace),
      pianoMask       (apvts),
      centerKnob      (apvts, "tune_center",      "CENTER",      "Pitch Pull",      HarmonyEngineColours::accent,     "accent"),
      sensitivityKnob (apvts, "tune_sensitivity", "SENSITIVITY", "Note Split/Lock", HarmonyEngineColours::mintAccent, "mint"),
      variationKnob   (apvts, "tune_variation",   "VARIATION",   "Vibrato & Drift", HarmonyEngineColours::cyanAccent, "cyan"),
      transitionKnob  (apvts, "tune_transition",  "TRANSITION",  "Glide Speed",     HarmonyEngineColours::cyanAccent, "cyan"),
      formantKnob     (apvts, "alter_formant",    "FORMANT",     "Vocal Throat",    HarmonyEngineColours::accent,     "accent"),
      vibratoKnob     (apvts, "tune_vibrato",     "VIBRATO",     "Sustained Cap",   HarmonyEngineColours::mintAccent, "mint"),
      smoothAttachment    (apvts, "tune_smooth",    smoothButton),
      lookaheadAttachment (apvts, "tune_lookahead", lookaheadButton)
{
    header.setTrailingComponent (&scaleSelector, ScaleSelector::preferredWidth, 28);
    addAndMakeVisible (pitchTrace);
    addAndMakeVisible (noteTriangle);
    addAndMakeVisible (pianoMask);

    centerKnob.setTooltipText ("CENTER (NewTone): Pulls the average pitch of every note toward the target scale note (100% = dead-center pitch).");
    sensitivityKnob.setTooltipText ("SENSITIVITY (NewTone): Controls how eagerly NovaTone splits vocal phrases into separate note blocks and locks onto quiet syllables.");
    variationKnob.setTooltipText ("VARIATION (NewTone): Scales vibrato and pitch expression inside each note (0% = robotic flat line, 100% = natural expression).");
    transitionKnob.setTooltipText ("TRANSITION (NewTone): Controls how fast pitch glides from one note block to the next (100% = instant snap).");
    formantKnob.setTooltipText ("FORMANT: Shifts vocal throat character deeper (-st) or brighter (+st) while keeping pitch intact.");
    vibratoKnob.setTooltipText ("VIBRATO: Caps maximum vibrato swing on sustained notes.");

    for (auto* k : { &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
        addAndMakeVisible (k);

    auto& laf = HarmonyEngineLookAndFeel::shared();

    // 1. Top Toolbar Buttons
    for (auto* tb : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton, &polyModeButton, &midiCleanButton, &snapScaleButton, &autoScrollButton })
    {
        tb->setLookAndFeel (&laf);
        addAndMakeVisible (tb);
    }
    editToolButton.getProperties().set ("pillRole", "accent");
    cutToolButton.getProperties().set ("pillRole", "accent");
    advancedToolButton.getProperties().set ("pillRole", "accent");
    drawToolButton.getProperties().set ("pillRole", "accent");
    polyModeButton.getProperties().set ("pillRole", "mint");
    midiCleanButton.getProperties().set ("pillRole", "mint");
    midiCleanButton.setTooltip ("MIDI CLEAN MODE (Ctrl+M): Cleans pick/fret/drum noise, bridges dropouts, stitches fragmented notes, and outputs ultra-accurate instrument MIDI.");

    editToolButton.setToggleState (true, juce::dontSendNotification);
    editToolButton.setTooltip ("1. MOVE / TUNE: Drag notes up/down for semitones, Alt+Drag for cents, Right-Click to snap to scale.");
    cutToolButton.setTooltip ("2. SLICE / GLUE: Click inside a note to split it in two; Ctrl+Click to glue adjacent notes.");
    advancedToolButton.setTooltip ("3. NOTE SHAPER: Drag directly inside a note block to shape per-note Formant, Variation, Gain, and Ramp.");
    drawToolButton.setTooltip ("4. DRAW CURVE: Paint custom freehand pitch contours directly onto vocal notes.");
    polyModeButton.setTooltip ("Toggle between Monophonic (NewTone solo vocal) and Polyphonic (Melodyne chord/choir/string) detection.");

    editToolButton.onClick     = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::StandardEdit); };
    cutToolButton.onClick      = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::CutAndGlue); };
    advancedToolButton.onClick = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::AdvancedEdit); };
    drawToolButton.onClick     = [this] { setActiveToolMode (PitchTraceComponent::ToolMode::FreehandDraw); };

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
        polyModeButton.setButtonText (poly ? "POLY (MELODYNE)" : "MONO (NEWTONE)");
        pitchTrace.setPolyphonicMode (poly);
    };

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

    snapAllButton.getProperties().set ("accentButton", true);
    for (auto* btn : { &undoButton, &redoButton, &autoKeyButton, &levelerButton, &dockViewButton, &snapAllButton, &loadWavButton, &exportMidiButton, &clearButton })
    {
        btn->setLookAndFeel (&laf);
        addAndMakeVisible (btn);
    }

    undoButton.setTooltip ("Undo last note edit (Ctrl+Z)");
    undoButton.onClick = [this] { pitchTrace.undo(); };

    redoButton.setTooltip ("Redo note edit (Ctrl+Y)");
    redoButton.onClick = [this] { pitchTrace.redo(); };

    autoKeyButton.setTooltip ("1-Click Auto-Key Detector (Ctrl+K): Detects vocal key & scale via Krumhansl-Kessler chromagram.");
    autoKeyButton.onClick = [this] { pitchTrace.autoDetectKeyAndApply(); };

    levelerButton.setTooltip ("1-Click Note Leveler (Ctrl+L): Balances quiet and loud vocal notes toward the phrase average.");
    levelerButton.onClick = [this] { pitchTrace.levelAllNotesVolume (0.72f); };

    dockViewButton.setTooltip ("Cycle Lower Bar & Dock height (Full -> Hide Inspector Bar -> Compact Dock -> Max Piano Roll), or drag the splitter bar directly below the Piano Roll.");
    dockViewButton.onClick = [this] { cycleDockLayoutPreset(); };

    snapAllButton.setTooltip ("1-Click Snap All: Snaps every vocal note in the timeline to the active Key & Scale.");
    snapAllButton.onClick = [this] { pitchTrace.snapAllNotesToScale(); };

    loadWavButton.setTooltip ("Load a vocal audio file (.wav, .aiff, .flac, .mp3) onto the Piano Roll.");
    loadWavButton.onClick = [this] { pitchTrace.promptLoadAudioFile(); };

    exportMidiButton.setTooltip ("Export the tuned vocal melody as a standard .mid file.");
    exportMidiButton.onClick = [this] { pitchTrace.promptExportMidiFile(); };

    clearButton.setTooltip ("Clear the Piano Roll timeline.");
    clearButton.onClick = [this] { pitchTrace.clearTimeline(); };

    // 2. Selected-Note Quick Inspector Bar
    inspectorNoteLabel.setFont (HarmonyEngineUI::font (11.0f, true, 0.08f));
    inspectorNoteLabel.setColour (juce::Label::textColourId, HarmonyEngineColours::accentBright);
    inspectorNoteLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (inspectorNoteLabel);

    for (auto* b : { &semiDownButton, &semiUpButton, &resetNoteButton })
    {
        b->setLookAndFeel (&laf);
        addAndMakeVisible (b);
    }
    semiDownButton.setTooltip ("Transpose selected note down 1 scale step / semitone (Down Arrow)");
    semiUpButton.setTooltip ("Transpose selected note up 1 scale step / semitone (Up Arrow)");
    resetNoteButton.setTooltip ("Restore selected note to its default pitch, formant, variation, and gain");

    semiDownButton.onClick  = [this] { pitchTrace.transposeSelectedNote (-1); };
    semiUpButton.onClick    = [this] { pitchTrace.transposeSelectedNote (1); };
    resetNoteButton.onClick = [this] { pitchTrace.resetSelectedNote(); };

    auto setupInspectorSlider = [&] (juce::Slider& s, double minV, double maxV, double defV, const juce::String& suffix, const juce::String& tip)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (minV, maxV, 0.1);
        s.setValue (defV, juce::dontSendNotification);
        s.setDoubleClickReturnValue (true, defV);
        s.setTextValueSuffix (suffix);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
        s.setLookAndFeel (&laf);
        s.setTooltip (tip + " (double-click to reset)");
        addAndMakeVisible (s);
    };

    setupInspectorSlider (noteCentsSlider,   -50.0, 50.0,  0.0,   " c",  "Fine pitch offset in cents for the selected note");
    setupInspectorSlider (noteFormantSlider, -12.0, 12.0,  0.0,   " st", "Per-note Formant throat shift in semitones");
    setupInspectorSlider (noteVarSlider,       0.0, 200.0, 100.0, "%",   "Per-note Vibrato & Pitch Variation scaling");
    setupInspectorSlider (noteGainSlider,    -18.0, 12.0,  0.0,   " dB", "Per-note Volume Trim in dB");
    setupInspectorSlider (noteSibSlider,     -18.0, 12.0,  0.0,   " dB", "Per-note Sibilant & Breath Balance (>3.8 kHz) in dB");

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

    // 3. 1-Click Vocal Style Buttons + Lookahead/Smooth
    for (auto* sb : { &styleNaturalButton, &stylePopButton, &styleHardButton })
    {
        sb->setLookAndFeel (&laf);
        addAndMakeVisible (sb);
    }
    styleNaturalButton.setTooltip ("1-Click Preset: Soft, transparent natural vocal tuning (Center 58%, Variation 95%, Transition 26%, Sensitivity 52%, Humanize 50%).");
    stylePopButton.setTooltip     ("1-Click Preset: Tight, polished modern pop vocal (Center 92%, Variation 55%, Transition 78%, Sensitivity 72%, Humanize 18%).");
    styleHardButton.setTooltip    ("1-Click Preset: Instant T-Pain / Hyperpop hard snap (Center 100%, Variation 0%, Transition 100%, Sensitivity 85%, Humanize 0%).");

    styleNaturalButton.onClick = [this] { applyQuickVocalStyle (0.58f, 0.52f, 0.95f, 0.26f, 45.0f, 0.50f); };
    stylePopButton.onClick     = [this] { applyQuickVocalStyle (0.92f, 0.72f, 0.55f, 0.78f, 18.0f, 0.18f); };
    styleHardButton.onClick    = [this] { applyQuickVocalStyle (1.00f, 0.85f, 0.00f, 1.00f, 0.0f,  0.00f); };

    humanizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    humanizeSlider.setRange (0.0, 1.0, 0.01);
    humanizeSlider.setDoubleClickReturnValue (true, 0.50);
    humanizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
    humanizeSlider.setLookAndFeel (&laf);
    humanizeSlider.setTooltip ("HUMANIZE SLIDER (0%..100%): 0% = Off, 50% = Standard Natural Humanize (matches previous ON button), 100% = Extreme Humanize (steeper leaps, deeper scoops & organic vibrato bloom). Double-click for 50%.");
    addAndMakeVisible (humanizeSlider);
    if (apvts.getParameter ("tune_humanizeAmount") != nullptr)
        humanizeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, "tune_humanizeAmount", humanizeSlider);

    pitchTrace.onToggleHumanizeShortcut = [this]
    {
        const float cur = (float) humanizeSlider.getValue();
        const float next = (cur < 0.25f) ? 0.50f : ((cur < 0.75f) ? 1.00f : 0.00f);
        setParamNormalised ("tune_humanizeAmount", next);
    };

    smoothButton.setButtonText ("AUTO-SMOOTH");
    smoothButton.setLookAndFeel (&laf);
    smoothButton.setTooltip ("Auto-Smooth: Hands-free natural lookahead tuning.");
    addAndMakeVisible (smoothButton);

    lookaheadButton.setButtonText ("60ms LOOKAHEAD");
    lookaheadButton.setLookAndFeel (&laf);
    lookaheadButton.setTooltip ("Lookahead: Inspects incoming pitch 60 ms ahead for zero-lag note transitions.");
    addAndMakeVisible (lookaheadButton);

    smoothButton.onStateChange = [this] { updateControlStates(); };
    updateControlStates();

    pitchTrace.setFrequencyRange (80.0f, 1000.0f);
}

TuneTab::~TuneTab()
{
    for (auto* tb : { &editToolButton, &cutToolButton, &advancedToolButton, &drawToolButton, &polyModeButton, &midiCleanButton,
                      &snapScaleButton, &autoScrollButton, &smoothButton, &lookaheadButton })
        tb->setLookAndFeel (nullptr);

    for (auto* btn : { &undoButton, &redoButton, &autoKeyButton, &levelerButton, &dockViewButton, &snapAllButton,
                       &loadWavButton, &exportMidiButton, &clearButton,
                       &semiDownButton, &semiUpButton, &resetNoteButton,
                       &styleNaturalButton, &stylePopButton, &styleHardButton })
        btn->setLookAndFeel (nullptr);

    for (auto* s : { &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &humanizeSlider })
        s->setLookAndFeel (nullptr);
}

void TuneTab::refreshTheme()
{
    inspectorNoteLabel.setColour (juce::Label::textColourId,
        HarmonyEngineColours::isLightChassis ? HarmonyEngineColours::accentDeep : HarmonyEngineColours::accentBright);
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
        inspectorNoteLabel.setText ("SELECTED NOTE:  " + PitchTraceComponent::midiNoteName (sel->targetMidi), juce::dontSendNotification);
        noteCentsSlider.setValue   (sel->fineCents,               juce::dontSendNotification);
        noteFormantSlider.setValue (sel->formantSemitones,        juce::dontSendNotification);
        noteVarSlider.setValue     (sel->noteVariation * 100.0f,  juce::dontSendNotification);
        noteGainSlider.setValue    (sel->gainDb,                  juce::dontSendNotification);
        noteSibSlider.setValue     (sel->sibilantDb,              juce::dontSendNotification);
    }
    else
    {
        inspectorNoteLabel.setText ("SELECTED NOTE:  None (Click any note)", juce::dontSendNotification);
        noteCentsSlider.setValue   (0.0,   juce::dontSendNotification);
        noteFormantSlider.setValue (0.0,   juce::dontSendNotification);
        noteVarSlider.setValue     (100.0, juce::dontSendNotification);
        noteGainSlider.setValue    (0.0,   juce::dontSendNotification);
        noteSibSlider.setValue     (0.0,   juce::dontSendNotification);
    }
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

void TuneTab::setInspectorVisibility (bool visible)
{
    for (auto* c : std::initializer_list<juce::Component*> {
            &inspectorNoteLabel, &semiDownButton, &semiUpButton,
            &noteCentsSlider, &noteFormantSlider, &noteVarSlider, &noteGainSlider, &noteSibSlider, &resetNoteButton })
        c->setVisible (visible);
}

void TuneTab::setMiddleRowVisibility (bool visible)
{
    for (auto* c : std::initializer_list<juce::Component*> {
            &pianoMask, &noteTriangle, &styleNaturalButton, &stylePopButton, &styleHardButton, &humanizeSlider, &smoothButton, &lookaheadButton })
        c->setVisible (visible);
}

void TuneTab::setKnobRowVisibility (bool visible)
{
    for (auto* c : std::initializer_list<juce::Component*> {
            &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
        c->setVisible (visible);
}

void TuneTab::cycleDockLayoutPreset()
{
    if (lowerDockHeight >= 255 && userInspectorHeight > 0)
    {
        userInspectorHeight = 0;
        lowerDockHeight     = 234;
    }
    else if (lowerDockHeight >= 200)
    {
        userInspectorHeight = 0;
        lowerDockHeight     = 172;
    }
    else if (lowerDockHeight > 0)
    {
        savedLowerDockHeight = defaultLowerDockHeight;
        lowerDockHeight      = 0;
    }
    else
    {
        userInspectorHeight = defaultInspectorHeight;
        lowerDockHeight     = defaultLowerDockHeight;
    }

    layoutBody (getBodyBounds());
    repaint();
}

//==============================================================================
// Interactive Splitter Dragging (Adjust Lower Bar & Knob Dock Height)
//==============================================================================
void TuneTab::mouseMove (const juce::MouseEvent& e)
{
    const auto pt = e.position.toInt();
    const bool overMain = splitterBarArea.expanded (0, 3).contains (pt);
    const bool overInsp = !inspectorBottomSplitArea.isEmpty() && inspectorBottomSplitArea.expanded (0, 3).contains (pt);
    const bool overKnob = !knobTopSplitArea.isEmpty() && knobTopSplitArea.expanded (0, 3).contains (pt);

    if (overMain != mainSplitterHovered || overInsp != inspSplitterHovered || overKnob != knobSplitterHovered)
    {
        mainSplitterHovered = overMain;
        inspSplitterHovered = overInsp;
        knobSplitterHovered = overKnob;
        repaint (splitterBarArea.expanded (0, 4));
        if (!inspectorBottomSplitArea.isEmpty())
            repaint (inspectorBottomSplitArea.expanded (0, 4));
        if (!knobTopSplitArea.isEmpty())
            repaint (knobTopSplitArea.expanded (0, 4));
    }

    if (overMain || overInsp || overKnob)
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TuneTab::mouseExit (const juce::MouseEvent&)
{
    if (mainSplitterHovered || inspSplitterHovered || knobSplitterHovered)
    {
        mainSplitterHovered = false;
        inspSplitterHovered = false;
        knobSplitterHovered = false;
        repaint (splitterBarArea.expanded (0, 4));
        if (!inspectorBottomSplitArea.isEmpty())
            repaint (inspectorBottomSplitArea.expanded (0, 4));
        if (!knobTopSplitArea.isEmpty())
            repaint (knobTopSplitArea.expanded (0, 4));
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TuneTab::mouseDown (const juce::MouseEvent& e)
{
    const auto pt = e.position.toInt();
    dragStartMouseY     = e.position.y;
    dragStartDockHeight = lowerDockHeight;
    dragStartInspHeight = userInspectorHeight;
    lastDragLayoutMs    = 0;

    if (splitterBarArea.expanded (0, 3).contains (pt)
        || (!knobTopSplitArea.isEmpty() && knobTopSplitArea.expanded (0, 3).contains (pt)))
        activeSplitterDrag = SplitterDragMode::ResizeLowerDock;
    else if (!inspectorBottomSplitArea.isEmpty() && inspectorBottomSplitArea.expanded (0, 3).contains (pt))
        activeSplitterDrag = SplitterDragMode::ResizeInspectorOnly;
    else
        activeSplitterDrag = SplitterDragMode::None;
}

void TuneTab::mouseDrag (const juce::MouseEvent& e)
{
    if (activeSplitterDrag == SplitterDragMode::None)
        return;

    // 60 Hz frame-rate cap on relayout prevents OS mouse-poll floods (500-1000 Hz) from
    // starving CPU/cache while audio is streaming.
    const juce::uint32 nowMs = juce::Time::getMillisecondCounter();
    if (nowMs - lastDragLayoutMs < 14)
        return;

    const int dy = juce::roundToInt (e.position.y - dragStartMouseY); // positive = dragged down

    if (activeSplitterDrag == SplitterDragMode::ResizeLowerDock)
    {
        const int maxDock = juce::jmin (340, getBodyBounds().getHeight() - toolbarHeight - 150);
        int nextDock = dragStartDockHeight - dy;

        if (nextDock < 36)
        {
            nextDock = 0; // Snap to full-screen Piano Roll
        }
        else
        {
            nextDock = juce::jlimit (56, maxDock, nextDock);
            if (nextDock >= 252 && userInspectorHeight == 0)
                userInspectorHeight = defaultInspectorHeight;
        }

        if (std::abs (nextDock - lowerDockHeight) >= 2 || (nextDock == 0 && lowerDockHeight != 0))
        {
            lastDragLayoutMs = nowMs;
            lowerDockHeight = nextDock;
            if (lowerDockHeight > 0)
                savedLowerDockHeight = lowerDockHeight;
            layoutBody (getBodyBounds());
            repaint();
        }
    }
    else if (activeSplitterDrag == SplitterDragMode::ResizeInspectorOnly)
    {
        int nextInsp = dragStartInspHeight - dy;
        if (nextInsp < 20)
            nextInsp = 0;
        else
            nextInsp = juce::jlimit (28, 64, nextInsp);

        if (std::abs (nextInsp - userInspectorHeight) >= 2 || (nextInsp == 0 && userInspectorHeight != 0))
        {
            lastDragLayoutMs = nowMs;
            const int delta = nextInsp - userInspectorHeight;
            userInspectorHeight = nextInsp;
            lowerDockHeight = juce::jlimit (160, 340, lowerDockHeight + delta);
            savedLowerDockHeight = lowerDockHeight;
            layoutBody (getBodyBounds());
            repaint();
        }
    }
}

void TuneTab::mouseUp (const juce::MouseEvent&)
{
    if (activeSplitterDrag != SplitterDragMode::None)
    {
        activeSplitterDrag = SplitterDragMode::None;
        layoutBody (getBodyBounds());
        repaint();
    }
}

void TuneTab::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto pt = e.position.toInt();
    if (splitterBarArea.expanded (0, 4).contains (pt)
        || (!knobTopSplitArea.isEmpty() && knobTopSplitArea.expanded (0, 4).contains (pt)))
    {
        if (lowerDockHeight > 0)
        {
            savedLowerDockHeight = lowerDockHeight;
            lowerDockHeight = 0;
        }
        else
        {
            lowerDockHeight = juce::jmax (172, savedLowerDockHeight);
            if (lowerDockHeight >= 252 && userInspectorHeight == 0)
                userInspectorHeight = defaultInspectorHeight;
        }
        layoutBody (getBodyBounds());
        repaint();
    }
    else if (!inspectorBottomSplitArea.isEmpty() && inspectorBottomSplitArea.expanded (0, 4).contains (pt))
    {
        if (userInspectorHeight > 0)
        {
            lowerDockHeight = juce::jmax (160, lowerDockHeight - (userInspectorHeight + 6));
            userInspectorHeight = 0;
        }
        else
        {
            userInspectorHeight = defaultInspectorHeight;
            lowerDockHeight = juce::jmax (defaultLowerDockHeight, lowerDockHeight + userInspectorHeight + 6);
        }
        layoutBody (getBodyBounds());
        repaint();
    }
}

//==============================================================================
TuneTab::ScaleSelector::ScaleSelector (juce::AudioProcessorValueTreeState& apvts)
{
    auto setUp = [&] (juce::ComboBox& box, const juce::String& paramId, const juce::String& tooltip,
                      std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>& attachment)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (paramId)))
            box.addItemList (choice->choices, 1);
        box.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
        box.setJustificationType (juce::Justification::centredLeft);
        box.setTooltip (tooltip);
        addAndMakeVisible (box);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, paramId, box);
    };

    setUp (rangeBox, "tune_vocalRange", "Vocal Range Lock: Full, Soprano, Alto, Tenor, or Bass/Baritone", rangeAttachment);
    setUp (rootBox,  "tune_root",       "Key: Root note of the song", rootAttachment);
    setUp (modeBox,  "tune_mode",       "Scale: Major, Minor, or Custom 12-key scale", modeAttachment);
}

TuneTab::ScaleSelector::~ScaleSelector()
{
    rangeBox.setLookAndFeel (nullptr);
    rootBox.setLookAndFeel (nullptr);
    modeBox.setLookAndFeel (nullptr);
}

void TuneTab::ScaleSelector::paint (juce::Graphics& g)
{
    g.setFont (HarmonyEngineUI::font (10.0f, true, 0.2f));
    g.setColour (HarmonyEngineColours::textSecondary);
    g.drawText ("RANGE", rangeLabelArea, juce::Justification::centredRight);
    g.drawText ("KEY",   keyLabelArea,   juce::Justification::centredRight);
    g.drawText ("SCALE", scaleLabelArea, juce::Justification::centredRight);
}

void TuneTab::ScaleSelector::resized()
{
    auto r = getLocalBounds();
    rangeLabelArea = r.removeFromLeft (44);
    r.removeFromLeft (4);
    rangeBox.setBounds (r.removeFromLeft (78));
    r.removeFromLeft (8);
    keyLabelArea = r.removeFromLeft (30);
    r.removeFromLeft (4);
    rootBox.setBounds (r.removeFromLeft (62));
    r.removeFromLeft (8);
    scaleLabelArea = r.removeFromLeft (42);
    r.removeFromLeft (4);
    modeBox.setBounds (r);
}

//==============================================================================
void TuneTab::paintBody (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    HarmonyEngineUI::drawLiftedPanel (g, toolbarPanelArea, 10.0f);

    if (!inspectorPanelArea.isEmpty())
    {
        HarmonyEngineUI::drawLiftedPanel (g, inspectorPanelArea, 10.0f);
        g.setFont (HarmonyEngineUI::font (9.0f, true, 0.12f));
        g.setColour (textSecondary);
        g.drawText ("CENTS",     centsLabelArea,   juce::Justification::centredRight);
        g.drawText ("FORMANT",   formantLabelArea, juce::Justification::centredRight);
        g.drawText ("VARIATION", varLabelArea,     juce::Justification::centredRight);
        g.drawText ("GAIN",      gainLabelArea,    juce::Justification::centredRight);
        g.drawText ("SIB",       sibLabelArea,     juce::Justification::centredRight);
    }

    if (!pianoPanelArea.isEmpty())
        HarmonyEngineUI::drawLiftedPanel (g, pianoPanelArea, 10.0f);

    if (!macroPanelArea.isEmpty())
    {
        HarmonyEngineUI::drawLiftedPanel (g, macroPanelArea, 10.0f);
        if (macroPanelArea.getHeight() >= 64)
        {
            auto styleHeader = macroPanelArea.reduced (12, 4).removeFromTop (13);
            g.setFont (HarmonyEngineUI::font (9.0f, true, 0.18f));
            g.setColour (isLightChassis ? accentDeep : cyanBright);
            g.drawText ("1-CLICK VOCAL STYLES   •   HUMANIZE SLIDER (50% = STD / 100% = EXTREME)", styleHeader, juce::Justification::centredLeft);
        }
        if (!humanizeLabelArea.isEmpty())
        {
            g.setFont (HarmonyEngineUI::font (9.5f, true, 0.10f));
            g.setColour (mintAccent);
            g.drawText ("HUMANIZE", humanizeLabelArea, juce::Justification::centredRight);
        }
    }

    if (!knobPanelArea.isEmpty())
        HarmonyEngineUI::drawLiftedPanel (g, knobPanelArea, 14.0f);

    // Draw Interactive Splitter Bar right below the Piano Roll
    if (!splitterBarArea.isEmpty())
    {
        const auto sb = splitterBarArea.toFloat();
        const bool active = mainSplitterHovered || (activeSplitterDrag == SplitterDragMode::ResizeLowerDock);

        g.setColour ((active ? cyanAccent : knobOutline).withAlpha (active ? 0.35f : 0.18f));
        g.fillRoundedRectangle (sb.reduced (2.0f, 1.5f), 4.0f);

        const auto grip = juce::Rectangle<float> (76.0f, 4.0f).withCentre (sb.getCentre());
        g.setColour (active ? cyanBright : textSecondary.withAlpha (0.65f));
        g.fillRoundedRectangle (grip, 2.0f);

        g.setFont (HarmonyEngineUI::font (8.2f, true, 0.10f));
        g.setColour (active ? cyanBright : textSecondary.withAlpha (0.70f));
        const juce::String hint = (lowerDockHeight == 0)
            ? "▲ DRAG UP OR DBL-CLICK TO RESTORE LOWER BARS"
            : "↕ DRAG TO ADJUST BAR HEIGHT  •  DBL-CLICK TO MAXIMIZE PIANO ROLL";
        g.drawText (hint, sb.reduced (12.0f, 0.0f), juce::Justification::centredRight, false);
    }

    // Draw subtle secondary grip between Inspector Bar and 12-Key Scale Piano Row
    if (!inspectorBottomSplitArea.isEmpty() && (inspSplitterHovered || activeSplitterDrag == SplitterDragMode::ResizeInspectorOnly))
    {
        const auto ib = inspectorBottomSplitArea.toFloat();
        const auto grip = juce::Rectangle<float> (54.0f, 3.0f).withCentre (ib.getCentre());
        g.setColour (accentBright.withAlpha (0.85f));
        g.fillRoundedRectangle (grip, 1.5f);
    }

    // Draw subtle grip on top edge of the Knob Dock when hovered
    if (!knobTopSplitArea.isEmpty() && (knobSplitterHovered || activeSplitterDrag == SplitterDragMode::ResizeLowerDock))
    {
        const auto kb = knobTopSplitArea.toFloat();
        const auto grip = juce::Rectangle<float> (64.0f, 3.0f).withCentre (kb.getCentre());
        g.setColour (cyanBright.withAlpha (0.85f));
        g.fillRoundedRectangle (grip, 1.5f);
    }
}

void TuneTab::layoutBody (juce::Rectangle<int> bounds)
{
    // 1. Top Editor Toolbar
    toolbarPanelArea = bounds.removeFromTop (toolbarHeight);
    {
        auto tb = toolbarPanelArea.reduced (6, 5);
        editToolButton.setBounds     (tb.removeFromLeft (68));
        tb.removeFromLeft (3);
        cutToolButton.setBounds      (tb.removeFromLeft (68));
        tb.removeFromLeft (3);
        advancedToolButton.setBounds (tb.removeFromLeft (74));
        tb.removeFromLeft (3);
        drawToolButton.setBounds     (tb.removeFromLeft (68));
        tb.removeFromLeft (5);
        polyModeButton.setBounds     (tb.removeFromLeft (100));
        tb.removeFromLeft (3);
        midiCleanButton.setBounds    (tb.removeFromLeft (78));
        tb.removeFromLeft (4);

        snapScaleButton.setBounds    (tb.removeFromLeft (76));
        tb.removeFromLeft (3);
        autoScrollButton.setBounds   (tb.removeFromLeft (80));
        tb.removeFromLeft (4);

        undoButton.setBounds         (tb.removeFromLeft (42));
        tb.removeFromLeft (3);
        redoButton.setBounds         (tb.removeFromLeft (42));
        tb.removeFromLeft (3);
        autoKeyButton.setBounds      (tb.removeFromLeft (64));
        tb.removeFromLeft (3);
        levelerButton.setBounds      (tb.removeFromLeft (60));

        clearButton.setBounds        (tb.removeFromRight (46));
        tb.removeFromRight (3);
        exportMidiButton.setBounds   (tb.removeFromRight (72));
        tb.removeFromRight (3);
        loadWavButton.setBounds      (tb.removeFromRight (72));
        tb.removeFromRight (3);
        snapAllButton.setBounds      (tb.removeFromRight (66));
        tb.removeFromRight (3);
        dockViewButton.setBounds     (tb.removeFromRight (74));
    }
    bounds.removeFromTop (4);

    const int maxAllowedDock = juce::jmax (0, bounds.getHeight() - splitterHeight - 160);
    const int effDockH = juce::jlimit (0, maxAllowedDock, lowerDockHeight);

    if (effDockH == 0)
        dockViewButton.setButtonText ("BAR: HIDDEN");
    else if (effDockH < 210)
        dockViewButton.setButtonText ("BAR: COMPACT");
    else if (userInspectorHeight == 0 || effDockH < 252)
        dockViewButton.setButtonText ("BAR: NO INSP");
    else
        dockViewButton.setButtonText ("BAR: FULL");

    inspectorPanelArea       = {};
    inspectorBottomSplitArea = {};
    knobTopSplitArea         = {};
    pianoPanelArea           = {};
    macroPanelArea           = {};
    knobPanelArea            = {};

    if (effDockH <= 0)
    {
        setInspectorVisibility (false);
        setMiddleRowVisibility (false);
        setKnobRowVisibility   (false);

        splitterBarArea = bounds.removeFromBottom (splitterHeight);
        pitchTrace.setBounds (bounds);
        return;
    }

    auto dockArea = bounds.removeFromBottom (effDockH);
    splitterBarArea = bounds.removeFromBottom (splitterHeight);

    int inspH = 0;
    int midH  = 0;
    int knobH = 0;

    if (effDockH >= 248 && userInspectorHeight >= 26)
    {
        inspH = juce::jlimit (26, 64, userInspectorHeight);
        const int rem = effDockH - inspH - 12;
        midH  = juce::jlimit (54, 92, juce::roundToInt (rem * (float) baseMiddleH / (float) (baseMiddleH + baseKnobH)));
        knobH = juce::jmax (96, rem - midH);
    }
    else if (effDockH >= 145)
    {
        inspH = 0;
        const int rem = effDockH - 6;
        midH  = juce::jlimit (50, 80, juce::roundToInt (rem * 0.34f));
        knobH = juce::jmax (88, rem - midH);
    }
    else
    {
        inspH = 0;
        midH  = juce::jlimit (50, 96, effDockH);
        knobH = 0;
    }

    // 2. Bottom Global Knobs Dock (if visible)
    if (knobH > 0)
    {
        setKnobRowVisibility (true);
        knobPanelArea = dockArea.removeFromBottom (knobH);
        const int vPad = knobH >= 130 ? 10 : 4;
        auto knobArea = knobPanelArea.reduced (16, vPad);
        const int numKnobs = 6;
        const int colW = knobArea.getWidth() / numKnobs;
        for (auto* k : { &centerKnob, &sensitivityKnob, &variationKnob, &transitionKnob, &formantKnob, &vibratoKnob })
            k->setBounds (knobArea.removeFromLeft (colW).reduced (4, 0));

        if (midH > 0 || inspH > 0)
            knobTopSplitArea = dockArea.removeFromBottom (6);
    }
    else
    {
        setKnobRowVisibility (false);
    }

    // 3. Middle Row: 12-Key Scale Keyboard (left) + 1-Click Style Presets (center) + Live Tuner (right)
    if (midH > 0)
    {
        setMiddleRowVisibility (true);
        auto midRow = (inspH > 0) ? dockArea.removeFromBottom (midH) : dockArea;

        auto triArea = midRow.removeFromRight (126);
        noteTriangle.setBounds (triArea);
        midRow.removeFromRight (8);

        pianoPanelArea = midRow.removeFromLeft (330);
        pianoMask.setBounds (pianoPanelArea.reduced (6, juce::jmax (3, midRow.getHeight() >= 68 ? 6 : 3)));
        midRow.removeFromLeft (8);

        macroPanelArea = midRow;
        auto mp = macroPanelArea.reduced (10, midRow.getHeight() >= 68 ? 6 : 4);
        if (macroPanelArea.getHeight() >= 64)
            mp.removeFromTop (14);

        const int rowH = juce::jmax (18, (mp.getHeight() - 4) / 2);
        auto styleRow = mp.removeFromTop (rowH);
        const int styleW = (styleRow.getWidth() - 12) / 3;
        styleNaturalButton.setBounds (styleRow.removeFromLeft (styleW));
        styleRow.removeFromLeft (6);
        stylePopButton.setBounds     (styleRow.removeFromLeft (styleW));
        styleRow.removeFromLeft (6);
        styleHardButton.setBounds    (styleRow);

        mp.removeFromTop (4);
        auto bottomRow = mp;
        const int pillW = juce::jmin (112, bottomRow.getWidth() / 4);
        lookaheadButton.setBounds (bottomRow.removeFromRight (pillW));
        bottomRow.removeFromRight (6);
        smoothButton.setBounds    (bottomRow.removeFromRight (pillW));
        bottomRow.removeFromRight (8);
        humanizeLabelArea         = bottomRow.removeFromLeft (62);
        humanizeSlider.setBounds  (bottomRow.reduced (4, 0));

        if (inspH > 0)
            inspectorBottomSplitArea = dockArea.removeFromBottom (6);
    }
    else
    {
        setMiddleRowVisibility (false);
    }

    // 4. Selected-Note Quick Inspector Bar (right below the Piano Roll, adjustable height)
    if (inspH > 0)
    {
        setInspectorVisibility (true);
        inspectorPanelArea = dockArea;
        const int vPad = juce::jmax (3, (inspectorPanelArea.getHeight() - 26) / 2);
        auto ir = inspectorPanelArea.reduced (10, vPad);
        inspectorNoteLabel.setBounds (ir.removeFromLeft (152));
        semiDownButton.setBounds     (ir.removeFromLeft (56));
        ir.removeFromLeft (4);
        semiUpButton.setBounds       (ir.removeFromLeft (56));
        ir.removeFromLeft (8);

        resetNoteButton.setBounds    (ir.removeFromRight (82));
        ir.removeFromRight (8);

        const int groupW = ir.getWidth() / 5;
        auto g1 = ir.removeFromLeft (groupW);
        centsLabelArea = g1.removeFromLeft (36);
        noteCentsSlider.setBounds (g1.reduced (2, 0));

        auto g2 = ir.removeFromLeft (groupW);
        formantLabelArea = g2.removeFromLeft (50);
        noteFormantSlider.setBounds (g2.reduced (2, 0));

        auto g3 = ir.removeFromLeft (groupW);
        varLabelArea = g3.removeFromLeft (54);
        noteVarSlider.setBounds (g3.reduced (2, 0));

        auto g4 = ir.removeFromLeft (groupW);
        gainLabelArea = g4.removeFromLeft (32);
        noteGainSlider.setBounds (g4.reduced (2, 0));

        auto g5 = ir;
        sibLabelArea = g5.removeFromLeft (26);
        noteSibSlider.setBounds (g5.reduced (2, 0));
    }
    else
    {
        setInspectorVisibility (false);
    }

    // 5. All remaining space is the 2D Piano Roll & Note Block Canvas
    pitchTrace.setBounds (bounds);
}