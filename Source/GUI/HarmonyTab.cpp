#include "HarmonyTab.h"
#include "HarmonyEngineColours.h"
#include "../Choir/ChoirRegister.h"

namespace
{
    const char* pcName (int pc)
    {
        static const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return n[((pc % 12) + 12) % 12];
    }

    juce::String noteName (int midi)
    {
        return juce::String (pcName (midi)) + juce::String (midi / 12 - 1);
    }

    juce::String chordName (int chord)
    {
        if (chord < 0) return "-";
        return juce::String (pcName (chord / ChordTracker::numQualities)) + ChordTracker::qualitySuffix (chord % ChordTracker::numQualities);
    }

    const juce::Colour voiceColour (int i)
    {
        using namespace HarmonyEngineColours;
        switch (i % 4) { case 0: return accent; case 1: return mintAccent; case 2: return cyanAccent; default: return accentBright; }
    }
}

//==============================================================================
HarmonyTab::VoiceCard::VoiceCard (juce::AudioProcessorValueTreeState& s, int i, ChoirSession& c, const HarmonyEngine& e)
    : index (i), state (s), choir (c), engine (e),
      powerAttachment (s, "harm_v" + juce::String (i + 1) + "_on", power),
      mode (s, "harm_v" + juce::String (i + 1) + "_mode", "", "What this voice sings, relative to the lead"),
      level (s, "harm_v" + juce::String (i + 1) + "_level", "Level", {}, {}, "accent"),
      pan (s, "harm_v" + juce::String (i + 1) + "_pan", "Pan", {}, {}, "cyan"),
      formant (s, "harm_v" + juce::String (i + 1) + "_formant", "Formant", {}, {}, "mint")
{
    power.setButtonText ("Voice " + juce::String (i + 1));
    power.getProperties().set ("chip", true);
    power.getProperties().set ("pillRole", i % 4 == 1 ? "mint" : (i % 4 == 2 ? "cyan" : "accent"));
    removeButton.getProperties().set ("ghost", true);
    removeButton.setTooltip ("Remove this voice");
    removeButton.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    addChildComponent (removeButton);
    power.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    power.setTooltip ("Turn this harmony voice on or off");
    level.setTooltipText ("Voice level");
    pan.setTooltipText ("Stereo position");
    formant.setTooltipText ("Formant: 0 keeps your voice's character; lower = bigger/darker singer, higher = smaller/brighter");
    for (juce::Component* c : { (juce::Component*) &power, (juce::Component*) &mode, (juce::Component*) &level, (juce::Component*) &pan, (juce::Component*) &formant })
        addAndMakeVisible (c);

    // ---- Choirize: AI singer for this voice ----
    singer.setTooltip ("AI singer for this voice (Choirize). \xe2\x9c\x93 = range fits the part, \xe2\x97\x8f = ready (switching is instant). "
                       "Auto picks the best fit; Off keeps your own shifted voice.");
    singer.onChange = [this]
    {
        juce::String v = singer.getSelectedId() == 1 ? "auto" : singer.getSelectedId() == 2 ? "off" : juce::String();
        if (singer.getSelectedId() >= 100) v = ChoirSession::catalog()[(size_t) singer.getSelectedId() - 100].id;
        if (v.isEmpty()) return;
        state.state.setProperty ("choirSinger" + juce::String (index + 1), v, nullptr);
        if (choir.hasPart (index + 1)) choir.setCardSinger (index + 1, v);
        shownKey = {};
        refreshSinger();
    };
    doubleButton.setClickingTogglesState (true);
    doubleButton.setTooltip ("Double this voice with a second AI singer, panned to the other side");
    doubleButton.onClick = [this]
    {
        state.state.setProperty ("choirDouble" + juce::String (index + 1), doubleButton.getToggleState(), nullptr);
        doubleButton.getProperties().set ("accentButton", doubleButton.getToggleState()); doubleButton.repaint();
        if (choir.hasPart (index + 1)) choir.setCardDouble (index + 1, doubleButton.getToggleState());
        shownKey = {};
    };
    addAndMakeVisible (singer);
    addAndMakeVisible (doubleButton);
    refreshSinger();
}

/** Changes whenever the menu text would (ready marks, current singers, choirized or not). */
juce::String HarmonyTab::VoiceCard::singerKey() const
{
    const int part = index + 1;
    juce::String k (choir.latestTake());
    k << "|" << choir.cardVoice (part, 0) << "|" << choir.cardVoice (part, 1) << "|" << state.state.getProperty ("choirSinger" + juce::String (part)).toString()
      << "|" << (int) engine.getVoiceMode (index);
    if (const int i = choir.cardStrip (part, 0); i >= 0)
        for (auto& v : ChoirSession::catalog()) k << (choir.isReady (i, v.id) ? "1" : "0");
    return k;
}

void HarmonyTab::VoiceCard::refreshSinger()
{
    const auto key = singerKey();
    if (key == shownKey) return;
    shownKey = key;
    const int part = index + 1;
    const bool done = choir.hasPart (part);
    const int strip = choir.cardStrip (part, 0);
    singer.clear (juce::dontSendNotification);
    singer.addItem ("Auto (best fit)", 1);
    singer.addItem ("Off (your shifted voice)", 2);
    const int reg = choirRegisterFor (engine.getVoiceMode (index));
    int k = 0;
    for (auto& v : ChoirSession::catalog())
    {
        if (v.reg >= 0)
        {
            const bool fit = done ? choir.fits (choir.latestTake(), part, v) : v.reg == reg;
            auto text = ChoirSession::voiceLabel (v.id) + "   " + ChoirSession::rangeText (v.id);
            if (fit) text += juce::String (juce::CharPointer_UTF8 ("  \xe2\x9c\x93"));
            if (strip >= 0 && choir.isReady (strip, v.id)) text += juce::String (juce::CharPointer_UTF8 ("  \xe2\x97\x8f"));
            singer.addItem (text, 100 + k);
        }
        ++k;
    }
    juce::String cur = state.state.getProperty ("choirSinger" + juce::String (part), "auto").toString();
    if (done) cur = choir.cardVoice (part, 0).isEmpty() ? juce::String ("off") : choir.cardVoice (part, 0);
    int id = cur == "off" ? 2 : 1;
    k = 0;
    for (auto& v : ChoirSession::catalog()) { if (cur == v.id) id = 100 + k; ++k; }
    singer.setSelectedId (id, juce::dontSendNotification);
    doubleButton.setToggleState (done ? choir.cardVoice (part, 1).isNotEmpty() : (bool) state.state.getProperty ("choirDouble" + juce::String (part), false),
                                 juce::dontSendNotification);
    doubleButton.getProperties().set ("accentButton", doubleButton.getToggleState());
    repaint();
}

void HarmonyTab::VoiceCard::mouseUp (const juce::MouseEvent& e)
{
    // click a note block: AI singer -> your voice -> muted -> AI singer
    const auto* ns = choir.partNotes (index + 1);
    const auto len = choir.latestLength();
    if (ns == nullptr || ns->empty() || len <= 0 || ! notesArea.contains (e.getPosition()) || ! e.mouseWasClicked()) return;
    const double t = (double) (e.x - notesArea.getX()) / (double) notesArea.getWidth() * (double) len;
    for (int k = 0; k < (int) ns->size(); ++k)
        if (t >= (double) (*ns)[(size_t) k].start && t <= (double) (*ns)[(size_t) k].end) { choir.cycleNote (index + 1, k); repaint (notesArea); return; }
}

HarmonyTab::VoiceCard::~VoiceCard() { power.setLookAndFeel (nullptr); removeButton.setLookAndFeel (nullptr); }

void HarmonyTab::VoiceCard::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto b = getLocalBounds();
    HarmonyEngineUI::drawLiftedPanel (g, b, 10.0f);
    const bool on = power.getToggleState();

    // colour tab on the left edge
    g.setColour (voiceColour (index).withAlpha (on ? 0.9f : 0.25f));
    g.fillRoundedRectangle (b.toFloat().withWidth (3.0f).reduced (0.0f, 14.0f), 1.5f);

    // current note
    auto noteArea = b.reduced (14, 12).removeFromTop (28).removeFromRight (64);
    g.setFont (HarmonyEngineUI::font (17.0f, true));
    g.setColour (on && currentNote >= 0 ? voiceColour (index) : textSecondary.withAlpha (0.5f));
    g.drawText (on && currentNote >= 0 ? noteName (currentNote) : juce::String ("-"), noteArea, juce::Justification::centredRight);

    // choirized notes: AI singer (colour) / your voice (grey) / muted (outline); click to switch
    if (notesArea.isEmpty()) return;
    g.setColour (background.withAlpha (0.6f));
    g.fillRoundedRectangle (notesArea.toFloat(), 4.0f);
    const auto* ns = choir.partNotes (index + 1);
    const auto len = choir.latestLength();
    if (ns == nullptr || ns->empty() || len <= 0)
    {
        g.setFont (HarmonyEngineUI::font (10.5f, false));
        g.setColour (textSecondary.withAlpha (0.7f));
        g.drawText (choir.hasPart (index + 1) ? "No notes found" : "Choirize to see the notes", notesArea, juce::Justification::centred);
        return;
    }
    std::vector<int> ms; for (auto& n : *ns) ms.push_back (n.midi);   // range without stray octave jumps
    std::sort (ms.begin(), ms.end());
    int lo = ms[ms.size() / 10], hi = ms[ms.size() - 1 - ms.size() / 10];
    if (hi - lo < 4) { lo -= 2; hi += 2; }
    const auto a = notesArea.toFloat().reduced (2.0f, 3.0f);
    const float noteH = juce::jmax (2.0f, a.getHeight() / (float) juce::jmax (1, hi - lo + 1));
    for (auto& n : *ns)
    {
        const float x0 = a.getX() + a.getWidth() * (float) ((double) n.start / (double) len);
        const float x1 = a.getX() + a.getWidth() * (float) ((double) n.end / (double) len);
        const float y = a.getBottom() - noteH * (float) (juce::jlimit (lo, hi, n.midi) - lo + 1);
        juce::Rectangle<float> blk (x0, y, juce::jmax (2.0f, x1 - x0 - 1.0f), noteH);
        if (n.state == ChoirSession::noteAI)          { g.setColour (voiceColour (index)); g.fillRoundedRectangle (blk, 1.5f); }
        else if (n.state == ChoirSession::noteNormal) { g.setColour (textSecondary); g.fillRoundedRectangle (blk, 1.5f); }
        else                                          { g.setColour (textSecondary.withAlpha (0.5f)); g.drawRoundedRectangle (blk, 1.5f, 1.0f); }
    }
}

void HarmonyTab::VoiceCard::resized()
{
    auto r = getLocalBounds().reduced (14, 12);
    auto top = r.removeFromTop (28);
    power.setBounds (top.removeFromLeft (96));
    top.removeFromLeft (6);
    removeButton.setBounds (top.removeFromLeft (28));
    r.removeFromTop (10);
    mode.setBounds (r.removeFromTop (30));
    r.removeFromTop (6);
    auto srow = r.removeFromTop (26);
    doubleButton.setBounds (srow.removeFromRight (36));
    srow.removeFromRight (4);
    singer.setBounds (srow);
    notesArea = r.removeFromBottom (36);
    r.removeFromBottom (6);
    r.removeFromTop (6);
    r = r.withHeight (juce::jmin (r.getHeight(), 104));
    const int w = r.getWidth() / 3;
    level.setBounds (r.removeFromLeft (w));
    pan.setBounds (r.removeFromLeft (w));
    formant.setBounds (r);
}

//==============================================================================
HarmonyTab::HarmonyTab (juce::AudioProcessorValueTreeState& s, const HarmonyEngine& e, HarmonyExporter& x, std::function<bool()> sc,
                        ChoirSession& c, HarmonyExporter& cr)
    : choir (c), choirRecorder (cr), apvts (s), engine (e), exporter (x), sidechainConnected (std::move (sc)),
      mixKnob (s, "harm_mix", "Harmonies", {}, {}, "accent"),
      leadKnob (s, "harm_lead", "Lead", {}, {}, "cyan"),
      humanizeKnob (s, "harm_humanize", "Humanize", {}, {}, "mint"),
      sourceSelector (s, "harm_source", "Chords", "Where the chords come from: the sidechain (your beat), the Tune tab's key & scale, or MIDI notes you play (each voice sings one held note)")
{
    mixKnob.setTooltipText ("Level of all harmony voices");
    leadKnob.setTooltipText ("Level of the lead vocal. All the way down = harmonies only (to print them on their own track)");
    humanizeKnob.setTooltipText ("Humanize: each voice gets its own small timing offset and pitch drift, like separate singers");
    for (juce::Component* c : { (juce::Component*) &mixKnob, (juce::Component*) &leadKnob, (juce::Component*) &humanizeKnob, (juce::Component*) &sourceSelector })
        addAndMakeVisible (c);
    for (int i = 0; i < HarmonyEngine::numVoices; ++i)
    {
        voiceCards[(size_t) i] = std::make_unique<VoiceCard> (s, i, choir, engine);
        voiceStrip.addChildComponent (*voiceCards[(size_t) i]);
        voiceCards[(size_t) i]->removeButton.onClick = [this] { removeLastVoice(); };
    }
    addVoiceCard.onClick = [this] { showAddVoiceMenu(); };
    voiceStrip.addAndMakeVisible (addVoiceCard);
    voiceViewport.setViewedComponent (&voiceStrip, false);
    voiceViewport.setScrollBarsShown (false, true);
    voiceViewport.setScrollBarThickness (8);
    voiceViewport.getHorizontalScrollBar().setColour (juce::ScrollBar::thumbColourId, HarmonyEngineColours::textSecondary.withAlpha (0.5f));
    addAndMakeVisible (voiceViewport);
    shownVoices = visibleVoiceCount();
    voiceNotes.fill (-1);

    // ---- Export harmonies ----
    exportButton.setClickingTogglesState (false);
    exportButton.onClick = [this]
    {
        using S = HarmonyExporter::State;
        const auto st = exporter.getState();
        if (st == S::armed || st == S::recording) exporter.stop();
        else exporter.arm();
        updateExportControls();
    };
    dragButton.setButtonText ("Drag WAV to FL");
    dragButton.setTooltip ("Drag this into your FL Studio playlist (or any DAW). Drop it at the bar where you pressed play.");
    dragButton.getProperties().set ("accentButton", true);
    dragButton.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    dragButton.getFile = [this] { return exporter.getFile(); };
    saveAsButton.setButtonText ("Save as...");
    saveAsButton.setTooltip ("Save a copy of the exported harmonies somewhere else");
    saveAsButton.onClick = [this]
    {
        const auto src = exporter.getFile();
        if (! src.existsAsFile()) return;
        chooser = std::make_unique<juce::FileChooser> ("Save harmonies", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (src.getFileName()), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [src] (const juce::FileChooser& fc)
                              {
                                  auto dest = fc.getResult();
                                  if (dest == juce::File()) return;
                                  src.copyFileTo (dest.withFileExtension ("wav"));
                              });
    };
    showButton.setButtonText ("Show file");
    showButton.setTooltip ("Open the folder with the exported harmonies");
    showButton.onClick = [this] { if (exporter.getFile().existsAsFile()) exporter.getFile().revealToUser(); };
    for (auto* b : { &exportButton, &saveAsButton, &showButton })
        b->setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    dragButton.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    for (juce::Component* c : { (juce::Component*) &exportButton, (juce::Component*) &dragButton, (juce::Component*) &saveAsButton, (juce::Component*) &showButton })
        addAndMakeVisible (c);
    updateExportControls();

    // ---- Choirize ----
    choirizeButton.getProperties().set ("accentButton", true);
    choirizeButton.setTooltip ("Turns the voices that are on into AI singers: press, play the section in your DAW, stop. "
                               "Needs the Choir helper running on this computer.");
    choirizeButton.onClick = [this] { toggleChoirize(); };
    choirOnButton.setClickingTogglesState (true);
    choirOnButton.setTooltip ("Hear the AI singers instead of the live harmony voices");
    choirOnButton.onClick = [this] { choir.enabled.store (choirOnButton.getToggleState()); };
    stemsButton.setButtonText ("Drag stems");
    stemsButton.setTooltip ("Drag every singer as its own WAV into FL Studio (drop at bar 1: each file starts at the song start). Click to open the folder.");
    stemsButton.setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    stemsButton.getFiles = [this] { return exportStems(); };
    stemsButton.onClick = [this] { const auto f = exportStems(); if (! f.isEmpty()) juce::File (f[0]).revealToUser(); };
    for (auto* b : { &choirizeButton, &choirOnButton, (juce::TextButton*) &stemsButton })
    {
        b->setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
        addAndMakeVisible (b);
    }

    startTimerHz (20);
}

void HarmonyTab::updateExportControls()
{
    using S = HarmonyExporter::State;
    const auto st = exporter.getState();
    lastExportState = st;
    exportButton.setButtonText (st == S::armed ? "Cancel" : st == S::recording ? "Stop" : st == S::ready ? "Export again" : "Export harmonies");
    exportButton.setTooltip ("Records only the harmony voices (lead muted) to a WAV. Arm it, then play the part in FL Studio; stopping playback finishes the file.");
    exportButton.getProperties().set ("accentButton", st != S::ready);
    const bool ready = st == S::ready && exporter.getFile().existsAsFile();
    dragButton.setVisible (ready);
    saveAsButton.setVisible (ready);
    showButton.setVisible (ready);
    exportButton.repaint();
    repaint (exportBar);
}

void HarmonyTab::paintExportBar (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    using S = HarmonyExporter::State;
    HarmonyEngineUI::drawLiftedPanel (g, exportBar, 10.0f);
    const auto st = exporter.getState();
    auto fmt = [] (double sec)
    {
        const int m = (int) (sec / 60.0);
        return juce::String (m) + ":" + juce::String (sec - m * 60.0, 1).paddedLeft ('0', 4);
    };
    juce::String text; juce::Colour dot = textSecondary;
    switch (st)
    {
        case S::idle:      text = "Export the harmonies on their own (no lead) as a WAV"; break;
        case S::armed:     text = "Armed - press play in FL Studio; stop playback when you're done"; dot = accent; break;
        case S::recording: text = "Recording harmonies  " + fmt (exporter.getSecondsRecorded()) + "   (from " + fmt (exporter.getStartSeconds()) + " in the song)";
                           dot = (juce::Time::getMillisecondCounter() / 400) % 2 ? accentBright : accent; break;
        case S::finishing: text = "Finishing the file..."; dot = cyanAccent; break;
        case S::ready:     text = exporter.getFile().getFileName() + "  -  " + fmt (exporter.getSecondsRecorded()) + ", starts at " + fmt (exporter.getStartSeconds());
                           dot = mintAccent; break;
        case S::error:     text = "Export failed: " + exporter.getError(); dot = accent; break;
    }
    g.setColour (dot);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) exportStatus.getX() + 4.0f, (float) exportStatus.getCentreY() }));
    g.setFont (HarmonyEngineUI::font (12.0f, st == S::recording || st == S::ready));
    g.setColour (st == S::idle ? textSecondary : textPrimary);
    g.drawFittedText (text, exportStatus.withTrimmedLeft (16), juce::Justification::centredLeft, 1);
}

HarmonyTab::~HarmonyTab()
{
    stopTimer();
    for (auto* b : { &exportButton, &saveAsButton, &showButton, &choirizeButton, &choirOnButton }) b->setLookAndFeel (nullptr);
    dragButton.setLookAndFeel (nullptr);
    stemsButton.setLookAndFeel (nullptr);
}

void HarmonyTab::refreshTheme()
{
    for (auto* k : { &mixKnob, &leadKnob, &humanizeKnob }) k->refreshThemeColours();
    for (auto& v : voiceCards)
        for (auto* k : { &v->level, &v->pan, &v->formant }) k->refreshThemeColours();
    repaint();
}

void HarmonyTab::timerCallback()
{
    chord = engine.getChord();
    leadMidi = engine.getLeadMidi();
    sideActive = engine.isSidechainActive();
    sideConnected = sidechainConnected ? sidechainConnected() : false;
    for (int pc = 0; pc < 12; ++pc) chroma[(size_t) pc] = engine.getChroma (pc);
    for (int i = 0; i < HarmonyEngine::numVoices; ++i)
    {
        voiceNotes[(size_t) i] = engine.getVoiceNote (i);
        if (voiceCards[(size_t) i]->currentNote != voiceNotes[(size_t) i])
        {
            voiceCards[(size_t) i]->currentNote = voiceNotes[(size_t) i];
            voiceCards[(size_t) i]->repaint();
        }
    }
    repaint (chordCard.getUnion (keyboardCard));
    if (visibleVoiceCount() != shownVoices) { shownVoices = visibleVoiceCount(); layoutVoices(); } // preset / undo / session load
    if (choir.pollArm (choirRecorder, [this] (int part) { return engine.getVoiceNote (part - 1); }))
        for (auto& v : voiceCards) v->repaint();
    if (++singerTick % 10 == 0) for (auto& v : voiceCards) v->refreshSinger();   // ~2x a second (checks voice files)
    {
        using S = HarmonyExporter::State;
        const auto cs = choirRecorder.getState();
        choirizeButton.setButtonText (cs == S::armed ? "Cancel" : cs == S::recording ? "Stop" : "Choirize");
        if (choirOnButton.getToggleState() != choir.enabled.load()) choirOnButton.setToggleState (choir.enabled.load(), juce::dontSendNotification);
        if ((bool) choirOnButton.getProperties()["accentButton"] != choirOnButton.getToggleState())
        { choirOnButton.getProperties().set ("accentButton", choirOnButton.getToggleState()); choirOnButton.repaint(); }
        stemsButton.setEnabled (choir.getNumTakes() > 0);
        repaint (choirBar);
    }
    if (exporter.getState() != lastExportState) updateExportControls();
    else if (exporter.getState() == HarmonyExporter::State::recording) repaint (exportBar);
}

//==============================================================================
void HarmonyTab::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    HarmonyEngineUI::paintChassis (g, getLocalBounds());

    paintChordCard (g, chordCard);
    paintKeyboardCard (g, keyboardCard);

    HarmonyEngineUI::drawLiftedPanel (g, mixCard, 10.0f);
    HarmonyEngineUI::drawCardTitle (g, mixCard.reduced (14, 12).removeFromTop (16), "Mix");

    paintExportBar (g);
    paintChoirBar (g);
    g.setFont (HarmonyEngineUI::font (11.0f, false));
    g.setColour (textSecondary);
    g.drawFittedText ("Chords come from your beat on NovaTone's sidechain input (FL Studio: route the beat's mixer track to this track as a sidechain). "
                      "Without it, harmonies use the Tune tab's key & scale.",
                      helpArea, juce::Justification::centredLeft, 2);
}

void HarmonyTab::paintChordCard (juce::Graphics& g, juce::Rectangle<int> area)
{
    using namespace HarmonyEngineColours;
    HarmonyEngineUI::drawLiftedPanel (g, area, 10.0f);
    auto r = area.reduced (14, 12);
    HarmonyEngineUI::drawCardTitle (g, r.removeFromTop (16), "Chord");
    r.removeFromTop (6);

    const bool useSide = juce::roundToInt (apvts.getRawParameterValue ("harm_source")->load()) == 0;
    const bool fromSide = useSide && chord >= 0;
    juce::String big = fromSide ? chordName (chord) : juce::String ("-");
    juce::String sub;
    juce::Colour dot = textSecondary;
    if (fromSide) { sub = "From your sidechain"; dot = mintAccent; }
    else
    {
        const int root = juce::roundToInt (apvts.getRawParameterValue ("tune_root")->load());
        const int mode = juce::roundToInt (apvts.getRawParameterValue ("tune_mode")->load());
        big = juce::String (pcName (root)) + (mode == 1 ? " minor" : (mode == 2 ? " custom" : " major"));
        if (juce::roundToInt (apvts.getRawParameterValue ("harm_source")->load()) == 2) { big = "MIDI"; sub = "Voices sing your MIDI notes"; dot = mintAccent; }
        else if (! useSide)         sub = "Using the key & scale";
        else if (! sideConnected)   { sub = "No sidechain connected: using the key"; dot = accent; }
        else if (! sideActive)      { sub = "Sidechain silent: using the key"; dot = accent; }
        else                        { sub = "Listening for chords..."; dot = cyanAccent; }
    }

    g.setFont (HarmonyEngineUI::font (34.0f, true, -0.01f));
    g.setColour (textPrimary);
    g.drawText (big, r.removeFromTop (44), juce::Justification::centredLeft);
    auto subRow = r.removeFromTop (18);
    g.setColour (dot);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ (float) subRow.getX() + 3.5f, (float) subRow.getCentreY() }));
    g.setFont (HarmonyEngineUI::font (12.0f, false));
    g.setColour (textSecondary);
    g.drawText (sub, subRow.withTrimmedLeft (14), juce::Justification::centredLeft);

    // chromagram
    r.removeFromTop (12);
    auto bars = r.removeFromTop (juce::jmax (30, r.getHeight() - 16));
    const float bw = (float) bars.getWidth() / 12.0f;
    const uint16_t mask = chord >= 0 ? ChordTracker::chordMask (chord / ChordTracker::numQualities, chord % ChordTracker::numQualities) : 0;
    for (int pc = 0; pc < 12; ++pc)
    {
        auto col = juce::Rectangle<float> ((float) bars.getX() + bw * (float) pc, (float) bars.getY(), bw, (float) bars.getHeight()).reduced (2.0f, 0.0f);
        g.setColour (panelLight);
        g.fillRoundedRectangle (col, 2.0f);
        const float v = juce::jlimit (0.0f, 1.0f, chroma[(size_t) pc]);
        const bool inChord = (mask >> pc) & 1u;
        g.setColour ((inChord ? mintAccent : textSecondary).withAlpha (inChord ? 0.9f : 0.45f));
        g.fillRoundedRectangle (col.withTop (col.getBottom() - col.getHeight() * v), 2.0f);
    }
    g.setFont (HarmonyEngineUI::font (10.0f, false));
    for (int pc = 0; pc < 12; ++pc)
    {
        g.setColour (((mask >> pc) & 1u) ? textPrimary : textSecondary);
        g.drawText (pcName (pc), juce::Rectangle<float> ((float) bars.getX() + bw * (float) pc, (float) bars.getBottom() + 2.0f, bw, 14.0f),
                    juce::Justification::centred);
    }
}

void HarmonyTab::paintKeyboardCard (juce::Graphics& g, juce::Rectangle<int> area)
{
    using namespace HarmonyEngineColours;
    HarmonyEngineUI::drawLiftedPanel (g, area, 10.0f);
    auto r = area.reduced (14, 12);
    auto title = r.removeFromTop (16);
    HarmonyEngineUI::drawCardTitle (g, title, "Lead & voices");
    g.setFont (HarmonyEngineUI::font (12.0f, true));
    g.setColour (leadMidi > 0.0f ? cyanBright : textSecondary);
    g.drawText (leadMidi > 0.0f ? "Lead " + noteName ((int) std::lround (leadMidi)) : juce::String ("No lead yet"), title, juce::Justification::centredRight);
    r.removeFromTop (12);

    // Keyboard spanning 3 octaves around the lead
    const int centre = leadMidi > 0.0f ? (int) std::lround (leadMidi) : 64;
    const int lo = ((centre - 18) / 12) * 12, hi = lo + 36;
    int whites = 0; for (int m = lo; m < hi; ++m) if (! juce::MidiMessage::isMidiNoteBlack (m)) ++whites;
    const float ww = (float) r.getWidth() / (float) whites;
    const auto kb = r.toFloat();
    const uint16_t mask = chord >= 0 ? ChordTracker::chordMask (chord / ChordTracker::numQualities, chord % ChordTracker::numQualities) : 0;

    auto keyColour = [&] (int m, bool black) -> juce::Colour
    {
        if (leadMidi > 0.0f && m == (int) std::lround (leadMidi)) return cyanAccent;
        for (int v = 0; v < HarmonyEngine::numVoices; ++v)
            if (voiceNotes[(size_t) v] == m && voiceCards[(size_t) v]->power.getToggleState()) return voiceColour (v);
        return black ? background : pianoWhiteKey.interpolatedWith (panelLight, isLightChassis ? 0.0f : 0.25f);
    };

    int wi = 0;
    std::vector<std::pair<juce::Rectangle<float>, int>> blacks;
    for (int m = lo; m < hi; ++m)
    {
        if (juce::MidiMessage::isMidiNoteBlack (m))
        {
            blacks.push_back ({ juce::Rectangle<float> (kb.getX() + ww * (float) wi - ww * 0.3f, kb.getY(), ww * 0.6f, kb.getHeight() * 0.6f), m });
            continue;
        }
        auto key = juce::Rectangle<float> (kb.getX() + ww * (float) wi, kb.getY(), ww, kb.getHeight()).reduced (1.0f, 0.0f);
        g.setColour (keyColour (m, false));
        g.fillRoundedRectangle (key, 3.0f);
        if ((mask >> (m % 12)) & 1u)
        {
            g.setColour (mintAccent);
            g.fillRoundedRectangle (key.withTop (key.getBottom() - 4.0f).reduced (key.getWidth() * 0.2f, 0.0f), 1.5f);
        }
        if (m % 12 == 0)
        {
            g.setFont (HarmonyEngineUI::font (9.5f, true));
            g.setColour (juce::Colour (0xff15171c).withAlpha (0.7f));
            g.drawText (noteName (m), key.withTrimmedBottom (6.0f), juce::Justification::centredBottom);
        }
        ++wi;
    }
    for (auto& [key, m] : blacks)
    {
        g.setColour (keyColour (m, true));
        g.fillRoundedRectangle (key, 2.5f);
        if ((mask >> (m % 12)) & 1u)
        {
            g.setColour (mintAccent);
            g.fillRoundedRectangle (key.withTop (key.getBottom() - 3.0f).reduced (key.getWidth() * 0.2f, 0.0f), 1.5f);
        }
    }
}

void HarmonyTab::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    helpArea = r.removeFromBottom (30);
    r.removeFromBottom (4);
    exportBar = r.removeFromBottom (44);
    r.removeFromBottom (6);
    choirBar = r.removeFromBottom (44);
    r.removeFromBottom (10);
    {
        auto e = choirBar.reduced (8, 7);
        choirizeButton.setBounds (e.removeFromLeft (150));
        e.removeFromLeft (8);
        stemsButton.setBounds (e.removeFromRight (130));
        e.removeFromRight (6);
        choirOnButton.setBounds (e.removeFromRight (110));
        e.removeFromRight (10);
        choirStatus = e;
    }
    {
        auto e = exportBar.reduced (8, 7);
        exportButton.setBounds (e.removeFromLeft (150));
        e.removeFromLeft (8);
        showButton.setBounds (e.removeFromRight (92));
        e.removeFromRight (6);
        saveAsButton.setBounds (e.removeFromRight (92));
        e.removeFromRight (6);
        dragButton.setBounds (e.removeFromRight (140));
        e.removeFromRight (10);
        exportStatus = e;
    }

    auto top = r.removeFromTop (juce::jmax (160, r.getHeight() - 300));
    chordCard = top.removeFromLeft (juce::roundToInt ((float) top.getWidth() * 0.30f));
    top.removeFromLeft (10);
    mixCard = top.removeFromRight (juce::roundToInt ((float) top.getWidth() * 0.36f));
    top.removeFromRight (10);
    keyboardCard = top;

    auto m = mixCard.reduced (14, 12);
    m.removeFromTop (22);
    sourceSelector.setBounds (m.removeFromBottom (30));
    m.removeFromBottom (10);
    m = m.withSizeKeepingCentre (m.getWidth(), juce::jmin (m.getHeight(), 112));
    const int kw = m.getWidth() / 3;
    mixKnob.setBounds (m.removeFromLeft (kw));
    leadKnob.setBounds (m.removeFromLeft (kw));
    humanizeKnob.setBounds (m);

    r.removeFromTop (10);
    voiceArea = r.withHeight (juce::jmin (r.getHeight(), 300));
    layoutVoices();
}

int HarmonyTab::visibleVoiceCount() const
{
    if (auto* p = apvts.getRawParameterValue ("harm_voices"))
        return juce::jlimit (1, HarmonyEngine::numVoices, juce::roundToInt (p->load()));
    return 4;
}

void HarmonyTab::layoutVoices()
{
    const int n = shownVoices;
    const bool canAdd = n < HarmonyEngine::numVoices;
    const int gap = 10, addW = 64, minCard = 210;
    const int avail = voiceArea.getWidth() - addW - gap;
    const int cw = juce::jmax (minCard, (avail - gap * (n - 1)) / n);
    const int contentW = n * cw + (n - 1) * gap + gap + addW;
    const bool scrolls = contentW > voiceArea.getWidth();
    const int h = voiceArea.getHeight() - (scrolls ? 12 : 0);

    voiceViewport.setBounds (voiceArea);
    voiceStrip.setSize (juce::jmax (contentW, voiceArea.getWidth()), h);
    int x = 0;
    for (int i = 0; i < HarmonyEngine::numVoices; ++i)
    {
        auto& v = *voiceCards[(size_t) i];
        v.setVisible (i < n);
        v.removeButton.setVisible (i == n - 1 && i >= 4);
        if (i < n) { v.setBounds (x, 0, cw, h); x += cw + gap; }
    }
    addVoiceCard.enabled = canAdd;
    addVoiceCard.setBounds (x, 0, addW, h);
    addVoiceCard.setTooltip (canAdd ? "Add a harmony voice: chord tones, Maj7 / Maj9 colours, Sus2 / Sus4 or random notes in the key"
                                    : "All " + juce::String (HarmonyEngine::numVoices) + " voices are in use");
    addVoiceCard.repaint();
}

void HarmonyTab::setParam (const juce::String& id, float plainValue)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

void HarmonyTab::showAddVoiceMenu()
{
    juce::PopupMenu m;
    m.setLookAndFeel (&HarmonyEngineLookAndFeel::shared());
    m.addSectionHeader ("Add a harmony voice");
    m.addItem (1 + HarmonyEngine::chordAbove, "Chord tone above");
    m.addItem (1 + HarmonyEngine::chordBelow, "Chord tone below");
    m.addItem (1 + HarmonyEngine::octaveUp,   "Octave above");
    m.addItem (1 + HarmonyEngine::octaveDown, "Octave below");
    m.addSectionHeader ("Colour");
    m.addItem (1 + HarmonyEngine::maj7Colour, "Maj7  (the 7th of the chord)");
    m.addItem (1 + HarmonyEngine::maj9Colour, "Maj9  (the 9th of the chord)");
    m.addItem (1 + HarmonyEngine::sus2Voice,  "Sus2  (moves through root, 2nd, 5th)");
    m.addItem (1 + HarmonyEngine::sus4Voice,  "Sus4  (moves through root, 4th, 5th)");
    m.addSectionHeader ("Just for fun");
    m.addItem (1 + HarmonyEngine::randomInKey, "Random (in key)  - a new note above the lead every time you change notes");
    juce::Component::SafePointer<HarmonyTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addVoiceCard).withMinimumWidth (260),
                     [safe] (int r) { if (safe != nullptr && r > 0) safe->addVoice (r - 1); });
}

void HarmonyTab::addVoice (int mode)
{
    const int n = visibleVoiceCount();
    if (n >= HarmonyEngine::numVoices) return;
    const juce::String id = "harm_v" + juce::String (n + 1) + "_";
    setParam (id + "mode", (float) mode);
    setParam (id + "level", mode == HarmonyEngine::randomInKey ? -10.0f : -8.0f);
    setParam (id + "on", 1.0f);
    setParam ("harm_voices", (float) (n + 1));
    shownVoices = n + 1;
    layoutVoices();
    // bring the new card into view
    voiceViewport.setViewPosition (juce::jmax (0, voiceStrip.getWidth() - voiceViewport.getWidth()), 0);
}

void HarmonyTab::removeLastVoice()
{
    const int n = visibleVoiceCount();
    if (n <= 4) return;
    setParam ("harm_v" + juce::String (n) + "_on", 0.0f);
    setParam ("harm_voices", (float) (n - 1));
    shownVoices = n - 1;
    layoutVoices();
}

void HarmonyTab::AddVoiceCard::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool hover = enabled && isMouseOver();
    g.setColour (hover ? panelLight : panelLight.withAlpha (0.45f));
    g.fillRoundedRectangle (b, 10.0f);
    juce::Path border; border.addRoundedRectangle (b, 10.0f);
    const float dashes[] = { 5.0f, 4.0f };
    juce::Path dashed;
    juce::PathStrokeType (1.2f).createDashedStroke (dashed, border, dashes, 2);
    g.setColour ((hover ? accent : textSecondary).withAlpha (enabled ? 0.8f : 0.3f));
    g.fillPath (dashed);
    const auto c = b.getCentre();
    g.setColour ((hover ? accent : textPrimary).withAlpha (enabled ? 0.9f : 0.3f));
    g.fillRoundedRectangle (juce::Rectangle<float> (18.0f, 2.5f).withCentre (c), 1.2f);
    g.fillRoundedRectangle (juce::Rectangle<float> (2.5f, 18.0f).withCentre (c), 1.2f);
    g.setFont (HarmonyEngineUI::font (10.5f, true));
    g.setColour (textSecondary.withAlpha (enabled ? 1.0f : 0.4f));
    g.drawText (enabled ? "Add" : "Max", b.withTrimmedTop (c.y - b.getY() + 16.0f).withHeight (16.0f), juce::Justification::centred);
}

//==============================================================================
void HarmonyTab::toggleChoirize()
{
    using S = HarmonyExporter::State;
    const auto st = choirRecorder.getState();
    if (st == S::armed || st == S::recording) { choirRecorder.stop(); return; }
    ChoirSession::PendingArm a;
    for (int v = 1; v <= HarmonyEngine::numVoices; ++v)
        if (auto* p = apvts.getRawParameterValue ("harm_v" + juce::String (v) + "_on"); p != nullptr && p->load() > 0.5f)
        {
            a.parts.push_back (v);
            a.singers.push_back (apvts.state.getProperty ("choirSinger" + juce::String (v), "auto").toString());
            a.doubles.push_back ((bool) apvts.state.getProperty ("choirDouble" + juce::String (v), false));
        }
    if (a.parts.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Choirize", "Turn on at least one voice first."); return; }
    for (size_t k = 0; k < a.parts.size(); ++k)   // MIDI mode: lowest held note -> first voice on, so registers go up in voice order
        a.regs.push_back (engine.isMidiMode() ? (int) (k * 4 / a.parts.size()) : choirRegisterFor (engine.getVoiceMode (a.parts[k] - 1)));
    choir.beginArm (choirRecorder, std::move (a));
}

juce::StringArray HarmonyTab::exportStems()
{
    juce::StringArray paths;
    if (! choir.isOpen() || choir.getNumTakes() == 0) return paths;
    for (auto& f : choir.exportStems (choir.getFolder().getChildFile ("Stems"))) paths.add (f.getFullPathName());
    return paths;
}

void HarmonyTab::paintChoirBar (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    using S = HarmonyExporter::State;
    HarmonyEngineUI::drawLiftedPanel (g, choirBar, 10.0f);
    juce::String text; juce::Colour dot = textSecondary;
    const auto st = choirRecorder.getState();
    const int pending = choir.pendingCount();
    if (st == S::armed)          { text = "Choirize armed - press play in FL Studio, stop when the section is done"; dot = accent; }
    else if (st == S::recording) { text = "Recording the voices  " + juce::String (choirRecorder.getSecondsRecorded(), 1) + " s"; dot = (juce::Time::getMillisecondCounter() / 400) % 2 ? accentBright : accent; }
    else if (pending > 0)        { text = "Making " + juce::String (pending) + (pending == 1 ? " singer" : " singers") + " (about 30 s each) - the Choir helper must be running"; dot = cyanAccent; }
    else if (choir.getNumTakes() > 0) { text = "Choirized: pick singers per voice, click notes to switch AI / your voice / mute, then drag the stems"; dot = mintAccent; }
    else text = "Choirize: pick a singer on each voice (or leave Auto), then press Choirize and play the section";
    g.setColour (dot);
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ (float) choirStatus.getX() + 4.0f, (float) choirStatus.getCentreY() }));
    g.setFont (HarmonyEngineUI::font (12.0f, st == S::recording));
    g.setColour (st == S::idle && pending == 0 ? textSecondary : textPrimary);
    g.drawFittedText (text, choirStatus.withTrimmedLeft (16), juce::Justification::centredLeft, 1);
}
