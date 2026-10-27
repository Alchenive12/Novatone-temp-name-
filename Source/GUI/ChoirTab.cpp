#include "ChoirTab.h"
#include "HarmonyEngineColours.h"

namespace
{
    /** Default singer register for a harmony mode: 0 low male, 1 mid male, 2 mid female, 3 high female. */
    int registerFor (int mode)
    {
        switch (mode)
        {
            case HarmonyEngine::octaveDown: case HarmonyEngine::chordRoot:  return 0;
            case HarmonyEngine::chordBelow: case HarmonyEngine::chordBelow2: return 1;
            case HarmonyEngine::chordAbove2: case HarmonyEngine::octaveUp: case HarmonyEngine::maj9Colour: return 3;
            default: return 2;
        }
    }
}

//==============================================================================
ChoirTab::Row::Row (ChoirTab& o, int i) : owner (o), index (i)
{
    auto& s = owner.choir.strip (index);
    int id = 1;
    for (auto& v : ChoirSession::catalog()) voiceBox.addItem (v.label, id++);
    voiceBox.onChange = [this]
    {
        const int k = voiceBox.getSelectedItemIndex();
        if (k >= 0) owner.choir.setVoice (index, ChoirSession::catalog()[(size_t) k].id);
        update();
    };
    level.setSliderStyle (juce::Slider::LinearHorizontal);
    level.setRange (-30.0, 6.0, 0.1); level.setTextValueSuffix (" dB");
    level.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    level.setValue (s.levelDb.load(), juce::dontSendNotification);
    level.onValueChange = [this] { owner.choir.strip (index).levelDb.store ((float) level.getValue()); };
    level.onDragEnd = [this] { owner.choir.saveMix(); };
    pan.setSliderStyle (juce::Slider::LinearHorizontal);
    pan.setRange (-1.0, 1.0, 0.01); pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setValue (s.pan.load(), juce::dontSendNotification);
    pan.setTooltip ("Pan (double-click = centre)");
    pan.onValueChange = [this] { owner.choir.strip (index).pan.store ((float) pan.getValue()); };
    pan.onDragEnd = [this] { owner.choir.saveMix(); };
    for (auto* b : { &mute, &solo }) b->setClickingTogglesState (true);
    mute.setToggleState (s.mute.load(), juce::dontSendNotification);
    solo.setToggleState (s.solo.load(), juce::dontSendNotification);
    mute.onClick = [this] { owner.choir.strip (index).mute.store (mute.getToggleState()); owner.choir.saveMix(); };
    solo.onClick = [this] { owner.choir.strip (index).solo.store (solo.getToggleState()); owner.choir.saveMix(); };
    remove.setTooltip ("Remove this singer");
    remove.onClick = [this] { owner.choir.removeStrip (index); juce::MessageManager::callAsync ([o = &owner] { o->rebuildRows(); }); };
    for (auto* c : std::initializer_list<juce::Component*> { &voiceBox, &level, &pan, &mute, &solo, &remove }) addAndMakeVisible (c);
    update();
}

void ChoirTab::Row::update()
{
    auto& s = owner.choir.strip (index);
    const auto shown = s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice;
    int k = 0;
    for (auto& v : ChoirSession::catalog()) { if (shown == v.id) voiceBox.setSelectedItemIndex (k, juce::dontSendNotification); ++k; }
    if (status != s.status) { status = s.status; repaint(); }
}

void ChoirTab::Row::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    r.removeFromLeft (70);                       // part name
    voiceBox.setBounds (r.removeFromLeft (150).withSizeKeepingCentre (150, 26));
    r.removeFromLeft (10);
    remove.setBounds (r.removeFromRight (28).withSizeKeepingCentre (26, 26));
    solo.setBounds (r.removeFromRight (32).withSizeKeepingCentre (28, 26));
    mute.setBounds (r.removeFromRight (32).withSizeKeepingCentre (28, 26));
    r.removeFromRight (8);
    pan.setBounds (r.removeFromRight (90));
    r.removeFromRight (8);
    level.setBounds (r.removeFromLeft (juce::jmin (260, r.getWidth())));
}

void ChoirTab::Row::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    g.setColour (panelLight);
    g.fillRoundedRectangle (getLocalBounds().reduced (2).toFloat(), 8.0f);
    const auto& s = owner.choir.strip (index);
    g.setColour (textPrimary);
    g.setFont (HarmonyEngineUI::font (14.0f, true, 0.0f));
    g.drawText ("Part " + juce::String (s.part), 10, 0, 66, getHeight(), juce::Justification::centredLeft);
    if (status.isNotEmpty())
    {
        g.setColour (status.contains ("didn't") || status.contains ("Can't") ? accent : cyanAccent);
        g.setFont (HarmonyEngineUI::font (12.0f, false, 0.0f));
        g.drawText (status, getLocalBounds().reduced (12, 0).withTrimmedLeft (230 + juce::jmin (260, getWidth() - 470)),
                    juce::Justification::centredLeft, true);
    }
}

//==============================================================================
ChoirTab::ChoirTab (juce::AudioProcessorValueTreeState& s, const HarmonyEngine& e, HarmonyExporter& r, ChoirSession& c)
    : apvts (s), engine (e), recorder (r), choir (c)
{
    recordButton.getProperties().set ("accentButton", true);
    recordButton.setTooltip ("Turn on the Harmony tab voices you want as parts, arm this, then play the song section in your DAW. "
                             "Stopping playback finishes the parts and opens them here.");
    recordButton.onClick = [this] { toggleRecord(); };
    openButton.onClick = [this] { openChooser(); };
    onButton.setTooltip ("Play the choir instead of the live harmonies");
    onButton.onClick = [this] { choir.enabled.store (onButton.getToggleState()); };
    dipButton.setTooltip ("Dips the choir around 3 kHz so the lead stays clear");
    dipButton.onClick = [this] { choir.dip.store (dipButton.getToggleState()); choir.saveMix(); };
    busSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    busSlider.setRange (-40.0, 6.0, 0.1); busSlider.setTextValueSuffix (" dB");
    busSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
    busSlider.setTooltip ("Whole choir level");
    busSlider.onValueChange = [this] { choir.busDb.store ((float) busSlider.getValue()); };
    busSlider.onDragEnd = [this] { choir.saveMix(); };
    balanceButton.setTooltip ("Every singer at 0 dB and the choir about 7.5 dB under the lead");
    balanceButton.onClick = [this] { choir.balance(); rebuildRows(); };
    addButton.onClick = [this]
    {
        const int part = addPart.getSelectedId();
        if (part > 0) { choir.addStrip (part, "original"); rebuildRows(); }
    };
    addPart.setTextWhenNothingSelected ("Part");
    for (auto* comp : std::initializer_list<juce::Component*> { &recordButton, &openButton, &onButton, &dipButton, &busSlider, &balanceButton, &addPart, &addButton, &viewport })
        addAndMakeVisible (comp);
    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, false);
    lastRec = recorder.getState();
    rebuildRows();
    startTimerHz (5);
}

ChoirTab::~ChoirTab() { stopTimer(); }

juce::String ChoirTab::recordText() const
{
    using S = HarmonyExporter::State;
    switch (recorder.getState())
    {
        case S::armed:     return "Armed - press play in your DAW, stop when the section is done";
        case S::recording: return "Recording parts  " + juce::String (recorder.getSecondsRecorded(), 1) + " s";
        case S::finishing: return "Finishing...";
        case S::error:     return recorder.getError();
        default: break;
    }
    if (! choir.isOpen())
        return choir.getMessage().isNotEmpty() ? choir.getMessage()
             : "No choir yet: turn on Harmony voices, then Record parts. Needs the Choir helper running on this computer.";
    return choir.getFolder().getFileName() + "  -  " + juce::String ((int) choir.getParts().size()) + " parts";
}

void ChoirTab::toggleRecord()
{
    using S = HarmonyExporter::State;
    const auto st = recorder.getState();
    if (st == S::armed || st == S::recording) { recorder.stop(); return; }
    armParts.clear(); armRegs.clear();
    std::vector<int> on;
    for (int v = 1; v <= HarmonyEngine::numVoices; ++v)
        if (auto* p = apvts.getRawParameterValue ("harm_v" + juce::String (v) + "_on"); p != nullptr && p->load() > 0.5f) on.push_back (v);
    if (on.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Choir", "Turn on at least one voice in the Harmony tab first."); return; }
    for (size_t k = 0; k < on.size(); ++k)
    {
        armParts.push_back (on[k]);
        // MIDI mode: lowest held note goes to the first voice on, so registers go up in voice order
        armRegs.push_back (engine.isMidiMode() ? (int) (k * 4 / on.size()) : registerFor (engine.getVoiceMode (on[k] - 1)));
    }
    choir.enabled.store (false);
    const auto dir = ChoirSession::rootFolder().getNonexistentChildFile ("Choir " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), {}, false);
    recorder.arm (dir.getChildFile ("parts.wav"));
}

void ChoirTab::openChooser()
{
    ChoirSession::rootFolder().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Open a choir folder", ChoirSession::rootFolder());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        if (fc.getResult().isDirectory()) { choir.open (fc.getResult()); rebuildRows(); }
    });
}

void ChoirTab::rebuildRows()
{
    rows.clear();
    for (int i = 0; i < ChoirSession::maxStrips; ++i)
        if (choir.strip (i).used.load())
        {
            rows.push_back (std::make_unique<Row> (*this, i));
            rowHolder.addAndMakeVisible (*rows.back());
        }
    addPart.clear (juce::dontSendNotification);
    for (int p : choir.getParts()) addPart.addItem ("Part " + juce::String (p), p);
    onButton.setToggleState (choir.enabled.load(), juce::dontSendNotification);
    dipButton.setToggleState (choir.dip.load(), juce::dontSendNotification);
    busSlider.setValue (choir.busDb.load(), juce::dontSendNotification);
    lastFolder = choir.getFolder();
    resized();
    repaint();
}

void ChoirTab::timerCallback()
{
    using S = HarmonyExporter::State;
    const auto st = recorder.getState();
    if (st == S::ready && lastRec != S::ready && ! armParts.empty())   // parts this tab armed
    {
        choir.createFromRecording (recorder.getFile().getParentDirectory(), recorder.getSampleRate(), recorder.getStartSample(), armParts, armRegs);
        rebuildRows();
    }
    lastRec = st;
    recordButton.setButtonText (st == S::armed ? "Cancel" : st == S::recording ? "Stop" : "Record parts");
    int used = 0;
    for (int i = 0; i < ChoirSession::maxStrips; ++i) used += choir.strip (i).used.load() ? 1 : 0;
    if (choir.getFolder() != lastFolder || used != (int) rows.size()) rebuildRows();
    if (onButton.getToggleState() != choir.enabled.load()) onButton.setToggleState (choir.enabled.load(), juce::dontSendNotification);
    for (auto& r : rows) r->update();
    repaint (topBar);
}

void ChoirTab::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    g.fillAll (background);
    HarmonyEngineUI::drawLiftedPanel (g, topBar, 10.0f);
    auto r = topBar.reduced (14, 10);
    HarmonyEngineUI::drawCardTitle (g, r.removeFromTop (16), "Choir");
    g.setColour (textSecondary);
    g.setFont (HarmonyEngineUI::font (13.0f, false, 0.0f));
    g.drawText (recordText(), r.removeFromBottom (20), juce::Justification::centredLeft, true);
}

void ChoirTab::resized()
{
    auto b = getLocalBounds().reduced (16, 12);
    topBar = b.removeFromTop (124);
    auto t = topBar.reduced (14, 10);
    t.removeFromTop (22);
    auto line1 = t.removeFromTop (32);
    recordButton.setBounds (line1.removeFromLeft (130).reduced (0, 2));
    line1.removeFromLeft (8);
    openButton.setBounds (line1.removeFromLeft (120).reduced (0, 2));
    line1.removeFromLeft (16);
    onButton.setBounds (line1.removeFromLeft (100));
    dipButton.setBounds (line1.removeFromLeft (190));
    balanceButton.setBounds (line1.removeFromRight (90).reduced (0, 2));
    line1.removeFromRight (8);
    busSlider.setBounds (line1.removeFromRight (juce::jmin (260, juce::jmax (120, line1.getWidth()))));
    t.removeFromTop (6);
    auto line2 = t.removeFromTop (30);
    addPart.setBounds (line2.removeFromLeft (110).reduced (0, 2));
    line2.removeFromLeft (8);
    addButton.setBounds (line2.removeFromLeft (100).reduced (0, 2));
    b.removeFromTop (10);
    viewport.setBounds (b);
    const int rowH = 46;
    rowHolder.setSize (juce::jmax (0, b.getWidth() - viewport.getScrollBarThickness()), rowH * (int) rows.size());
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds (0, (int) i * rowH, rowHolder.getWidth(), rowH);
}
