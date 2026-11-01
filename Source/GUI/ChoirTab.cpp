#include "ChoirTab.h"
#include "HarmonyEngineColours.h"
#include "../Choir/ChoirRegister.h"

namespace
{
    juce::String fmtTime (double sec)
    {
        const int m = (int) (sec / 60.0);
        return juce::String (m) + ":" + juce::String (sec - m * 60.0, 1).paddedLeft ('0', 4);
    }
    constexpr int rowH = 46, headH = 40;
}

//==============================================================================
ChoirTab::Row::Row (ChoirTab& o, int i) : owner (o), index (i)
{
    auto& s = owner.choir.strip (index);
    fillVoices();
    voiceBox.setTooltip ("Singer for this part. \xe2\x9c\x93 = its range fits the part, \xe2\x97\x8f = ready now (switching is instant); "
                         "others are made by the Choir helper in about 30 s while the current singer keeps playing.");
    voiceBox.onChange = [this]
    {
        const int k = voiceBox.getSelectedId() - 1;
        if (k >= 0) owner.choir.setVoice (index, ChoirSession::catalog()[(size_t) k].id);
        update();
    };
    prev.setTooltip ("Previous singer that fits this part");
    next.setTooltip ("Next singer that fits this part");
    prev.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
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
    remove.onClick = [this] { owner.choir.removeStrip (index); juce::MessageManager::callAsync ([o = juce::Component::SafePointer<ChoirTab> (&owner)] { if (o != nullptr) o->rebuildRows(); }); };
    for (auto* c : std::initializer_list<juce::Component*> { &prev, &voiceBox, &next, &level, &pan, &mute, &solo, &remove }) addAndMakeVisible (c);
    update();
}

/** "Gali (Mid female)  G3-E5  ✓ ●" for every voice; ids = catalog index + 1. */
void ChoirTab::Row::fillVoices()
{
    auto& s = owner.choir.strip (index);
    voiceBox.clear (juce::dontSendNotification);
    juce::String key;
    int id = 1;
    for (auto& v : ChoirSession::catalog())
    {
        const bool fit = owner.choir.fits (s.take, s.part, v), ready = owner.choir.isReady (index, v.id);
        auto text = ChoirSession::voiceLabel (v.id);
        if (v.reg >= 0) text << "   " << ChoirSession::rangeText (v.id);
        if (fit && v.reg >= 0) text += juce::String (juce::CharPointer_UTF8 ("  \xe2\x9c\x93"));
        if (ready) text += juce::String (juce::CharPointer_UTF8 ("  \xe2\x97\x8f"));
        voiceBox.addItem (text, id++);
        key << (ready ? "1" : "0");
    }
    readyKey = key;
}

void ChoirTab::Row::step (int dir)
{
    auto& s = owner.choir.strip (index);
    const auto& cat = ChoirSession::catalog();
    const auto cur = s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice;
    int k = 0;
    for (int i = 0; i < (int) cat.size(); ++i) if (cur == cat[(size_t) i].id) k = i;
    for (int n = 1; n < (int) cat.size(); ++n)
    {
        const int j = ((k + dir * n) % (int) cat.size() + (int) cat.size()) % (int) cat.size();
        const auto& v = cat[(size_t) j];
        if (v.reg >= 0 && owner.choir.fits (s.take, s.part, v)) { owner.choir.setVoice (index, v.id); update(); return; }
    }
}

void ChoirTab::Row::update()
{
    auto& s = owner.choir.strip (index);
    juce::String key;
    for (auto& v : ChoirSession::catalog()) key << (owner.choir.isReady (index, v.id) ? "1" : "0");
    if (key != readyKey) fillVoices();
    const auto shown = s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice;
    int k = 1;
    for (auto& v : ChoirSession::catalog()) { if (shown == v.id) voiceBox.setSelectedId (k, juce::dontSendNotification); ++k; }
    if (status != s.status) { status = s.status; repaint(); }
}

void ChoirTab::Row::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    r.removeFromLeft (112);                       // part name + range
    prev.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (24, 26));
    voiceBox.setBounds (r.removeFromLeft (250).withSizeKeepingCentre (250, 26));
    next.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (24, 26));
    r.removeFromLeft (10);
    remove.setBounds (r.removeFromRight (28).withSizeKeepingCentre (26, 26));
    solo.setBounds (r.removeFromRight (32).withSizeKeepingCentre (28, 26));
    mute.setBounds (r.removeFromRight (32).withSizeKeepingCentre (28, 26));
    r.removeFromRight (8);
    pan.setBounds (r.removeFromRight (80));
    r.removeFromRight (8);
    level.setBounds (r.removeFromLeft (juce::jmin (220, r.getWidth())));
}

void ChoirTab::Row::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    g.setColour (panelLight);
    g.fillRoundedRectangle (getLocalBounds().reduced (2).toFloat(), 8.0f);
    const auto& s = owner.choir.strip (index);
    const auto& tk = owner.choir.take (s.take);
    g.setColour (textPrimary);
    g.setFont (HarmonyEngineUI::font (14.0f, true, 0.0f));
    g.drawText ("Part " + juce::String (s.part), 12, 4, 100, getHeight() / 2, juce::Justification::bottomLeft);
    const int lo = tk.lo[(size_t) s.part - 1], hi = tk.hi[(size_t) s.part - 1];
    g.setColour (textSecondary);
    g.setFont (HarmonyEngineUI::font (11.0f, false, 0.0f));
    g.drawText (lo > 0 ? ChoirSession::noteName (lo) + "-" + ChoirSession::noteName (hi) : juce::String ("range ?"),
                12, getHeight() / 2, 100, getHeight() / 2 - 4, juce::Justification::topLeft);
    if (status.isNotEmpty())
    {
        g.setColour (status.contains ("didn't") || status.contains ("Can't") ? accent : cyanAccent);
        g.setFont (HarmonyEngineUI::font (11.0f, false, 0.0f));
        g.drawText (status, voiceBox.getX(), getHeight() - 13, getWidth() - voiceBox.getX() - 120, 12, juce::Justification::centredLeft, true);
    }
}

//==============================================================================
ChoirTab::TakeHeader::TakeHeader (ChoirTab& o, int t) : owner (o), take (t)
{
    moveBox.setTextWhenNothingSelected ("Move to group...");
    moveBox.setTooltip ("Move this take (its voices and mix) to another group");
    int id = 1;
    for (auto& g : ChoirSession::groupNames())
        if (g != owner.choir.getFolder().getFileName()) moveBox.addItem (g, id++);
    moveBox.addItem ("New group...", 1000);
    moveBox.onChange = [this]
    {
        const int sel = moveBox.getSelectedId();
        auto target = moveBox.getText();
        juce::Component::SafePointer<ChoirTab> o2 (&owner); const int tk = take;
        if (sel == 1000)
        {
            auto* w = new juce::AlertWindow ("Move take", "New group name:", juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", "Group " + juce::String (ChoirSession::groupNames().size() + 1));
            w->addButton ("Move", 1); w->addButton ("Cancel", 0);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([w, o2, tk] (int r)
            {
                if (r == 1 && o2 != nullptr) { o2->choir.moveTake (tk, w->getTextEditorContents ("name")); juce::MessageManager::callAsync ([o2] { if (o2 != nullptr) o2->rebuildRows(); }); }
            }), true);
            return;
        }
        if (sel > 0) juce::MessageManager::callAsync ([o2, tk, target] { if (o2 != nullptr) { o2->choir.moveTake (tk, target); o2->rebuildRows(); } });
    };
    partBox.setTextWhenNothingSelected ("Part");
    for (int p : owner.choir.take (take).parts) partBox.addItem ("Part " + juce::String (p), p);
    addButton.setTooltip ("Add another singer on the chosen part");
    addButton.onClick = [this]
    {
        if (const int p = partBox.getSelectedId(); p > 0)
        {
            owner.choir.addStrip (take, p, "original");
            juce::MessageManager::callAsync ([o2 = juce::Component::SafePointer<ChoirTab> (&owner)] { if (o2 != nullptr) o2->rebuildRows(); });
        }
    };
    for (auto* c : std::initializer_list<juce::Component*> { &moveBox, &partBox, &addButton }) addAndMakeVisible (c);
}

void ChoirTab::TakeHeader::resized()
{
    auto r = getLocalBounds().reduced (4, 6);
    addButton.setBounds (r.removeFromRight (84));
    r.removeFromRight (6);
    partBox.setBounds (r.removeFromRight (90));
    r.removeFromRight (12);
    moveBox.setBounds (r.removeFromRight (170));
}

void ChoirTab::TakeHeader::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto& tk = owner.choir.take (take);
    g.setColour (textPrimary);
    g.setFont (HarmonyEngineUI::font (13.0f, true, 0.02f));
    const auto len = (double) tk.length / tk.rate;
    g.drawText (tk.folder.getFileName() + "   " + fmtTime ((double) tk.start / tk.rate) + " - " + fmtTime ((double) tk.start / tk.rate + len),
                getLocalBounds().withTrimmedLeft (6).withTrimmedRight (370), juce::Justification::centredLeft, true);
}

//==============================================================================
ChoirTab::ChoirTab (juce::AudioProcessorValueTreeState& s, const HarmonyEngine& e, HarmonyExporter& r, ChoirSession& c)
    : apvts (s), engine (e), recorder (r), choir (c)
{
    recordButton.getProperties().set ("accentButton", true);
    recordButton.setTooltip ("Turn on the Harmony tab voices you want as parts, arm this, then play the section in your DAW. "
                             "Stopping playback adds the take to the open group.");
    recordButton.onClick = [this] { toggleRecord(); };
    groupBox.setTextWhenNothingSelected ("No group");
    groupBox.setTooltip ("Groups hold takes (one group = one song's choir). Every take in the open group plays.");
    groupBox.onChange = [this]
    {
        const auto name = groupBox.getText();
        if (name.isNotEmpty() && name != choir.getFolder().getFileName()) { choir.saveMix(); choir.open (ChoirSession::rootFolder().getChildFile (name)); rebuildRows(); }
    };
    newGroupButton.onClick = [this]
    {
        auto* w = new juce::AlertWindow ("New group", "Name:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "Group " + juce::String (ChoirSession::groupNames().size() + 1));
        w->addButton ("Create", 1); w->addButton ("Cancel", 0);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<ChoirTab> (this), w] (int res)
        {
            if (res == 1 && safe != nullptr) { safe->choir.saveMix(); safe->choir.newGroup (w->getTextEditorContents ("name")); safe->rebuildRows(); }
        }), true);
    };
    openButton.setTooltip ("Open a group folder from anywhere");
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
    prepareButton.setTooltip ("Has the Choir helper make every singer that fits each part, so trying them is instant");
    prepareButton.onClick = [this] { const int n = choir.prepareFittingVoices(); note = n > 0 ? "Making " + juce::String (n) + " voices (about 30 s each)" : "All fitting voices are ready"; };
    exportButton.setTooltip ("Writes the group's choir as you hear it to a WAV (drop it at the time shown)");
    exportButton.onClick = [this] { exportGroup(); };
    tuneButton.setTooltip ("Renders the group's choir and opens it in the Tune tab (Poly) at its song position, to edit notes. "
                           "Use it on a separate track: in Poly the clip replaces that track's audio.");
    tuneButton.onClick = [this] { sendToTune(); };
    for (auto* comp : std::initializer_list<juce::Component*> { &recordButton, &groupBox, &newGroupButton, &openButton, &onButton, &dipButton,
                                                                &busSlider, &balanceButton, &prepareButton, &exportButton, &tuneButton, &viewport })
        addAndMakeVisible (comp);
    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, false);
    lastRec = recorder.getState();
    rebuildRows();
    startTimerHz (10);
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
    if (note.isNotEmpty()) return note;
    if (! choir.isOpen())
        return choir.getMessage().isNotEmpty() ? choir.getMessage()
             : "Harmonies play live; AI singers are rendered from recorded parts. Turn on Harmony voices, then Record parts (needs the Choir helper on this computer).";
    return choir.getFolder().getFileName() + "  -  " + juce::String (choir.getNumTakes()) + (choir.getNumTakes() == 1 ? " take" : " takes");
}

void ChoirTab::toggleRecord()
{
    using S = HarmonyExporter::State;
    const auto st = recorder.getState();
    if (st == S::armed || st == S::recording) { recorder.stop(); return; }
    ChoirSession::PendingArm a;
    for (int v = 1; v <= HarmonyEngine::numVoices; ++v)
        if (auto* p = apvts.getRawParameterValue ("harm_v" + juce::String (v) + "_on"); p != nullptr && p->load() > 0.5f) a.parts.push_back (v);
    if (a.parts.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Choir", "Turn on at least one voice in the Harmony tab first."); return; }
    for (size_t k = 0; k < a.parts.size(); ++k)   // MIDI mode: lowest held note goes to the first voice on, so registers go up in voice order
        a.regs.push_back (engine.isMidiMode() ? (int) (k * 4 / a.parts.size()) : choirRegisterFor (engine.getVoiceMode (a.parts[k] - 1)));
    choir.beginArm (recorder, std::move (a));
}

void ChoirTab::openChooser()
{
    ChoirSession::rootFolder().createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Open a choir group", ChoirSession::rootFolder());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        if (fc.getResult().isDirectory()) { choir.saveMix(); choir.open (fc.getResult()); rebuildRows(); }
    });
}

void ChoirTab::exportGroup()
{
    if (! choir.isOpen() || choir.getNumTakes() == 0) return;
    const auto f = choir.getFolder().getNonexistentChildFile (choir.getFolder().getFileName() + " choir", ".wav", false);
    double rate = 48000.0;
    const auto start = choir.renderGroup (f, rate);
    note = start < 0 ? "Couldn't write the choir file" : f.getFileName() + " - place it at " + fmtTime ((double) start / rate) + " in the song";
    if (start >= 0) f.revealToUser();
}

void ChoirTab::sendToTune()
{
    if (! choir.isOpen() || choir.getNumTakes() == 0) return;
    const auto f = choir.getFolder().getChildFile (choir.getFolder().getFileName() + " for Tune.wav");
    double rate = 48000.0;
    const auto start = choir.renderGroup (f, rate);
    if (start < 0) { note = "Couldn't render the group"; return; }
    choir.enabled.store (false);
    note = "Sent to the Tune tab (choir playback off; edit it there)";
    if (onSendToTune) onSendToTune (f, start, rate);
}

void ChoirTab::refreshGroups()
{
    groupBox.clear (juce::dontSendNotification);
    int id = 1;
    for (auto& g : ChoirSession::groupNames())
    {
        groupBox.addItem (g, id);
        if (g == choir.getFolder().getFileName()) groupBox.setSelectedId (id, juce::dontSendNotification);
        ++id;
    }
}

void ChoirTab::rebuildRows()
{
    rows.clear(); headers.clear();
    for (int t = 0; t < choir.getNumTakes(); ++t)
    {
        headers.push_back (std::make_unique<TakeHeader> (*this, t));
        rowHolder.addAndMakeVisible (*headers.back());
        for (int i = 0; i < ChoirSession::maxStrips; ++i)
            if (choir.strip (i).used.load() && choir.strip (i).take == t)
            {
                rows.push_back (std::make_unique<Row> (*this, i));
                rowHolder.addAndMakeVisible (*rows.back());
            }
    }
    refreshGroups();
    onButton.setToggleState (choir.enabled.load(), juce::dontSendNotification);
    dipButton.setToggleState (choir.dip.load(), juce::dontSendNotification);
    busSlider.setValue (choir.busDb.load(), juce::dontSendNotification);
    lastFolder = choir.getFolder();
    lastTakes = choir.getNumTakes();
    resized();
    repaint();
}

void ChoirTab::timerCallback()
{
    using S = HarmonyExporter::State;
    const auto st = recorder.getState();
    if (choir.pollArm (recorder, [this] (int part) { return engine.getVoiceNote (part - 1); })) rebuildRows();
    lastRec = st;
    recordButton.setButtonText (st == S::armed ? "Cancel" : st == S::recording ? "Stop" : "Record parts");
    int used = 0;
    for (int i = 0; i < ChoirSession::maxStrips; ++i) used += choir.strip (i).used.load() ? 1 : 0;
    if (choir.getFolder() != lastFolder || used != (int) rows.size() || lastTakes != choir.getNumTakes()) rebuildRows();
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
    topBar = b.removeFromTop (142);
    auto t = topBar.reduced (14, 10);
    t.removeFromTop (22);
    auto line1 = t.removeFromTop (32);
    recordButton.setBounds (line1.removeFromLeft (124).reduced (0, 2));
    line1.removeFromLeft (12);
    groupBox.setBounds (line1.removeFromLeft (180).reduced (0, 2));
    line1.removeFromLeft (6);
    newGroupButton.setBounds (line1.removeFromLeft (96).reduced (0, 2));
    line1.removeFromLeft (6);
    openButton.setBounds (line1.removeFromLeft (76).reduced (0, 2));
    balanceButton.setBounds (line1.removeFromRight (84).reduced (0, 2));
    line1.removeFromRight (8);
    busSlider.setBounds (line1.removeFromRight (juce::jmin (220, juce::jmax (100, line1.getWidth() - 12))));
    t.removeFromTop (6);
    auto line2 = t.removeFromTop (32);
    onButton.setBounds (line2.removeFromLeft (100));
    dipButton.setBounds (line2.removeFromLeft (190));
    tuneButton.setBounds (line2.removeFromRight (150).reduced (0, 2));
    line2.removeFromRight (6);
    exportButton.setBounds (line2.removeFromRight (110).reduced (0, 2));
    line2.removeFromRight (6);
    prepareButton.setBounds (line2.removeFromRight (190).reduced (0, 2));
    b.removeFromTop (10);
    viewport.setBounds (b);
    const int w = juce::jmax (0, b.getWidth() - viewport.getScrollBarThickness());
    int y = 0;
    size_t ri = 0;
    for (auto& h : headers)
    {
        h->setBounds (0, y, w, headH); y += headH;
        while (ri < rows.size() && choir.strip (rows[ri]->index).take == h->take) { rows[ri]->setBounds (0, y, w, rowH); y += rowH; ++ri; }
        y += 8;
    }
    rowHolder.setSize (w, y);
}
