#include "ChordLane.h"
#include "HarmonyEngineColours.h"
#include "ModuleTabs.h"

namespace
{
    constexpr int rootQualityId (int root, int quality) { return 1000 + root * 16 + quality; }
    const char* pcLabel (int pc)
    {
        static const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return n[((pc % 12) + 12) % 12];
    }
    juce::PopupMenu chordMenu()
    {
        static const char* names[] = { "C","C#/Db","D","D#/Eb","E","F","F#/Gb","G","G#/Ab","A","A#/Bb","B" };
        juce::PopupMenu m;
        for (int r = 0; r < 12; ++r)
        {
            juce::PopupMenu sub;
            for (int q = 0; q < ChordTracker::numQualities; ++q)
                sub.addItem (rootQualityId (r, q), MidiChordTrack::chordName (r, q));
            m.addSubMenu (names[r], sub);
        }
        return m;
    }
}

//==============================================================================
ChordLane::ChordLane (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    auto& laf = HarmonyEngineLookAndFeel::shared();
    clearButton.setButtonText (juce::CharPointer_UTF8 ("\xc3\x97"));
    leftButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x80"));
    rightButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));
    expandButton.setButtonText ("Expand");
    importButton.getProperties().set ("accentButton", true);
    clearButton.getProperties().set ("ghost", true);
    writeButton.setClickingTogglesState (true);
    for (auto* b : { &importButton, &clearButton, &leftButton, &rightButton, &alignButton, &expandButton, &writeButton })
    {
        b->setLookAndFeel (&laf);
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    snapBox.addItemList ({ "Snap: bar", "Snap: 1/2 bar", "Snap: beat", "Snap: 1/2 beat", "Snap: 1/4 beat" }, 1);
    snapBox.setSelectedId (3, juce::dontSendNotification);
    snapBox.setLookAndFeel (&laf);
    snapBox.setTooltip ("Where chords snap to when you place, move or resize them");
    addChildComponent (snapBox);
    addChildComponent (writeButton);
    addChildComponent (palette);

    importButton.setTooltip ("Import a MIDI file with the song's chords (export the chord pattern from FL Studio). "
                             "Harmonies then follow these chords by song position, no sidechain needed.");
    clearButton.setTooltip ("Remove all chords");
    leftButton.setTooltip ("Move the chords 1 beat earlier (Shift: 1/4 beat, Ctrl: 1 bar)");
    rightButton.setTooltip ("Move the chords 1 beat later (Shift: 1/4 beat, Ctrl: 1 bar)");
    alignButton.setTooltip ("Line the chords up with your lead vocal: finds the shift (up to 4 bars either way) "
                            "where the most of the lead lands on chord tones. Play the song (or your Mono take) once first.");
    expandButton.setTooltip ("Open the chord editor: move around like the piano roll, and write chords yourself");
    writeButton.setTooltip ("Write chords: pick one below and drag it onto the timeline (or click a chip, then click the timeline). "
                            "Drag chords to move them, drag their edges to resize, right-click to change or delete.");

    importButton.onClick = [this] { importFile(); };
    clearButton.onClick  = [this]
    {
        if (track != nullptr) { undoStack.push_back (track); redoStack.clear(); }
        if (hooks.clear) hooks.clear();
        track.reset(); selected = -1; message = {}; repaint();
    };
    leftButton.onClick   = [this] { nudge (-1.0, juce::ModifierKeys::getCurrentModifiers()); };
    rightButton.onClick  = [this] { nudge (+1.0, juce::ModifierKeys::getCurrentModifiers()); };
    alignButton.onClick  = [this] { autoAlign(); };
    expandButton.onClick = [this] { setExpanded (! expanded); };
    writeButton.onClick  = [this]
    {
        writeMode = writeButton.getToggleState();
        writeButton.getProperties().set ("accentButton", writeMode);
        selected = -1;
        message = writeMode ? "Write mode: drag a chord from the row below onto the timeline, or click one and then click the timeline." : juce::String();
        resized(); repaint();
    };
    startTimerHz (30);
}

ChordLane::~ChordLane()
{
    stopTimer();
    for (auto* b : { &importButton, &clearButton, &leftButton, &rightButton, &alignButton, &expandButton, &writeButton })
        b->setLookAndFeel (nullptr);
    snapBox.setLookAndFeel (nullptr);
}

void ChordLane::setExpanded (bool shouldExpand)
{
    if (expanded == shouldExpand) return;
    expanded = shouldExpand;
    expandButton.setButtonText (expanded ? "Collapse" : "Expand");
    expandButton.setTooltip (expanded ? "Back to the Harmony tab" : "Open the chord editor: move around like the piano roll, and write chords yourself");
    if (! expanded && writeMode) writeButton.setToggleState (false, juce::sendNotificationSync);
    setWantsKeyboardFocus (expanded);
    if (expanded) { viewBeats = juce::jmax (viewBeats, 32.0); pitchViewSet = false; }
    else viewBeats = 16.0;
    if (onExpandedChanged) onExpandedChanged();
    resized();
    repaint();
}

double ChordLane::offset() const
{
    if (auto* p = apvts.getRawParameterValue ("harm_midiOffset")) return p->load();
    return 0.0;
}

void ChordLane::setOffset (double beats)
{
    if (auto* p = apvts.getParameter ("harm_midiOffset"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (-64.0, 64.0, beats)));
        p->endChangeGesture();
    }
    repaint();
}

double ChordLane::snapBeats() const
{
    static const double q[] = { 4.0, 2.0, 1.0, 0.5, 0.25 };
    return q[juce::jlimit (0, 4, snapBox.getSelectedId() - 1)];
}

int ChordLane::keyRoot() const
{
    if (auto* p = apvts.getRawParameterValue ("tune_root")) return juce::roundToInt (p->load()) % 12;
    return 0;
}

bool ChordLane::keyMinor() const
{
    if (auto* p = apvts.getRawParameterValue ("tune_mode")) return juce::roundToInt (p->load()) == 1;
    return false;
}

void ChordLane::nudge (double sign, const juce::ModifierKeys& mods)
{
    const double step = mods.isShiftDown() ? 0.25 : (mods.isCtrlDown() || mods.isCommandDown()) ? 4.0 : 1.0;
    setOffset (offset() + sign * step);
    message = {};
}

void ChordLane::importFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a MIDI file with the chords", juce::File(), "*.mid;*.midi");
    juce::Component::SafePointer<ChordLane> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr) return;
            const auto f = fc.getResult();
            if (! f.existsAsFile() || ! safe->hooks.load) return;
            juce::String err;
            const auto before = safe->track;
            if (safe->hooks.load (f, err))
            {
                if (before != nullptr) { safe->undoStack.push_back (before); safe->redoStack.clear(); }
                safe->track = safe->hooks.track ? safe->hooks.track() : nullptr;
                safe->selected = -1;
                safe->message = "Loaded " + f.getFileName() + ". Play the song with the vocal, then Auto-align (or drag the chords).";
                safe->fitOffset = 1.0e9;
            }
            else safe->message = err;
            safe->repaint();
        });
}

void ChordLane::refreshLead (bool force)
{
    if (! hooks.leadNotes) return;
    if (! force && ++leadTick % 15 != 0) return;   // ~2x a second
    lead = hooks.leadNotes();
    if (lead.empty()) return;
    std::vector<float> m;
    m.reserve (lead.size());
    for (const auto& n : lead) m.push_back (n.midi);
    std::sort (m.begin(), m.end());
    leadLo = m[m.size() / 20] - 1.5f;          // 5th..95th percentile, so a stray octave error doesn't squash the view
    leadHi = m[m.size() * 19 / 20] + 1.5f;
    if (leadHi - leadLo < 8.0f) { const float c = 0.5f * (leadHi + leadLo); leadLo = c - 4.0f; leadHi = c + 4.0f; }
    if (expanded && ! pitchViewSet)
    {
        const float c = m[m.size() / 2];
        viewLo = c - 14.0f; viewHi = c + 10.0f;   // room below for the chords' register
        pitchViewSet = true;
    }
}

void ChordLane::autoAlign()
{
    refreshLead (true);
    if (track == nullptr) { message = "Import or write chords first."; repaint(); return; }
    double best = 0.0; float fit = 0.0f;
    if (! track->bestOffset (lead, 16.0, offset(), best, fit))
    {
        message = "Not enough lead vocal yet: play the song (or your Mono take) once with the vocal, then Auto-align.";
        repaint();
        return;
    }
    setOffset (best);
    message = "Aligned: chords " + juce::String (best > 0 ? "+" : "") + juce::String (best, 2) + " beats. "
            + juce::String (juce::roundToInt (fit * 100.0f)) + "% of the lead is on chord tones. Drag or nudge to adjust.";
    fitOffset = 1.0e9;
    repaint();
}

//==============================================================================
// Write mode
std::shared_ptr<MidiChordTrack> ChordLane::editableCopy() const
{
    auto t = track != nullptr ? std::make_shared<MidiChordTrack> (*track) : std::make_shared<MidiChordTrack>();
    if (t->fileName.isEmpty()) t->fileName = "Written chords";
    return t;
}

void ChordLane::commit (std::shared_ptr<MidiChordTrack> edited)
{
    if (track != nullptr) undoStack.push_back (track);
    if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
    redoStack.clear();
    track = edited;
    if (hooks.publish) hooks.publish (track);
    fitOffset = 1.0e9;
    repaint();
}

void ChordLane::placeChord (int root, int quality, double songStart)
{
    auto t = editableCopy();
    const double start = snap (songStart) - offset();                     // file time
    const double next = t->nextStartAfter (start + 1.0e-6);
    const double end = juce::jmax (start + snapBeats(), juce::jmin (start + 4.0, next));   // a bar, or up to the next chord
    t->overwrite (MidiChordTrack::makeChord (root, quality, start, end));
    commit (t);
    selected = track->indexAt (start + 1.0e-6);
    message = {};
}

int ChordLane::segmentAtX (float x) const
{
    return track != nullptr ? track->indexAt (ppqFor (x) - offset()) : -1;
}

void ChordLane::chooseChord (std::function<void (int, int)> then)
{
    juce::Component::SafePointer<ChordLane> safe (this);
    chordMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
        [safe, then] (int id)
        {
            if (safe == nullptr || id < 1000) return;
            then ((id - 1000) / 16, (id - 1000) % 16);
        });
}

void ChordLane::showChordMenu (int segIndex)
{
    if (track == nullptr || segIndex < 0 || segIndex >= (int) track->segments.size()) return;
    selected = segIndex;
    repaint();
    juce::PopupMenu m;
    m.addSubMenu ("Change to", chordMenu());
    m.addSeparator();
    m.addItem (1, "Delete");
    juce::Component::SafePointer<ChordLane> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
        [safe, segIndex] (int id)
        {
            if (safe == nullptr || safe->track == nullptr || segIndex >= (int) safe->track->segments.size()) return;
            if (id == 1) { safe->selected = segIndex; safe->deleteSelected(); return; }
            if (id < 1000) return;
            auto t = safe->editableCopy();
            const auto old = t->segments[(size_t) segIndex];
            t->overwrite (MidiChordTrack::makeChord ((id - 1000) / 16, (id - 1000) % 16, old.start, old.end));
            safe->commit (t);
        });
}

void ChordLane::deleteSelected()
{
    if (track == nullptr || selected < 0 || selected >= (int) track->segments.size()) return;
    auto t = editableCopy();
    t->remove ((size_t) selected);
    selected = -1;
    commit (t);
}

//==============================================================================
void ChordLane::timerCallback()
{
    if (hooks.track && drag == Drag::none)
    {
        auto t = hooks.track();
        if (t != track) { track = t; fitOffset = 1.0e9; if (track == nullptr || selected >= (int) track->segments.size()) selected = -1; }
    }
    refreshLead (false);
    if (palette.isVisible() && leadTick % 15 == 0) { palette.rebuild(); palette.repaint(); }   // follows the Tune tab's key
    pos = hooks.positionPpq ? hooks.positionPpq() : 0.0;
    const bool moving = hooks.moving && hooks.moving();
    if (moving) following = true;
    if (following && drag == Drag::none)
    {
        // keep the playhead about a quarter in; page by whole bars
        if (pos < viewStart + viewBeats * 0.1 || pos > viewStart + viewBeats * 0.85)
            viewStart = juce::jmax (-4.0, std::floor ((pos - viewBeats * 0.25) / 4.0) * 4.0);
    }
    if (track != nullptr && ! lead.empty() && (std::abs (fitOffset - offset()) > 1.0e-6 || fitLeadCount != lead.size()))
    {
        double total = 0.0, hit = 0.0;
        for (const auto& n : lead)
        {
            const int pc = ((int) std::lround (n.midi) % 12 + 12) % 12;
            const double mid = 0.5 * (n.start + n.end), w = n.end - n.start;
            if (const auto* s = track->at (mid - offset())) { total += w; if ((s->mask >> pc) & 1u) hit += w; }
        }
        fitNow = total > 0.5 ? (float) (hit / total) : -1.0f;
        fitOffset = offset();
        fitLeadCount = lead.size();
    }
    repaint();
}

void ChordLane::resized()
{
    auto r = getLocalBounds().reduced (12, 8);
    auto top = r.removeFromTop (26);
    importButton.setBounds (top.removeFromLeft (118));
    top.removeFromLeft (4);
    clearButton.setBounds (top.removeFromLeft (26));
    top.removeFromLeft (8);
    expandButton.setBounds (top.removeFromRight (86));
    top.removeFromRight (8);
    alignButton.setBounds (top.removeFromRight (96));
    top.removeFromRight (8);
    rightButton.setBounds (top.removeFromRight (28));
    top.removeFromRight (2);
    leftButton.setBounds (top.removeFromRight (28));
    top.removeFromRight (8);
    writeButton.setVisible (expanded);
    snapBox.setVisible (expanded);
    if (expanded)
    {
        writeButton.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (6);
        snapBox.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
    }
    infoArea = top.reduced (6, 0);
    r.removeFromTop (6);

    palette.setVisible (expanded && writeMode);
    if (palette.isVisible())
    {
        palette.setBounds (r.removeFromTop (30));
        palette.rebuild();
        r.removeFromTop (6);
    }
    laneArea = r;
    if (expanded)
    {
        auto l = laneArea;
        keysArea = l.removeFromLeft (38);
        chordRow = l.removeFromTop (32);
        l.removeFromTop (2);
        rollArea = l;
    }
    else
    {
        keysArea = {};
        chordRow = laneArea.withTrimmedTop (13).withHeight (juce::jmin (26, juce::roundToInt (laneArea.getHeight() * 0.42f)));
        rollArea = laneArea.withTop (chordRow.getBottom() + 4).reduced (0, 2);
    }
}

float ChordLane::xFor (double ppq) const
{
    const auto t = expanded ? rollArea : laneArea;
    return (float) t.getX() + (float) ((ppq - viewStart) / viewBeats) * (float) t.getWidth();
}

double ChordLane::ppqFor (float x) const
{
    const auto t = expanded ? rollArea : laneArea;
    return viewStart + (double) (x - (float) t.getX()) / juce::jmax (1, t.getWidth()) * viewBeats;
}

float ChordLane::yFor (float midi) const
{
    const float lo = expanded ? viewLo : leadLo, hi = expanded ? viewHi : leadHi;
    return (float) rollArea.getBottom() - (midi - lo) / juce::jmax (1.0f, hi - lo) * (float) rollArea.getHeight();
}

void ChordLane::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    if (expanded) g.fillAll (background);   // covers the Harmony tab underneath
    HarmonyEngineUI::drawLiftedPanel (g, getLocalBounds(), 10.0f);

    const double off = offset();
    {
        juce::String txt;
        if (message.isNotEmpty()) txt = message;
        else if (track == nullptr) txt = expanded ? "Import a MIDI file, or turn on Write and draw the chords." : "Chords from a MIDI file: import one, then line it up with the lead.";
        else
        {
            txt = track->fileName + "   |   offset " + (off > 0 ? "+" : "") + juce::String (off, 2) + " beats";
            if (fitNow >= 0.0f) txt += "   |   lead on chord tones: " + juce::String (juce::roundToInt (fitNow * 100.0f)) + "%";
        }
        g.setFont (HarmonyEngineUI::font (11.5f, false));
        g.setColour (textSecondary);
        g.drawFittedText (txt, infoArea, juce::Justification::centredLeft, 1);
    }

    auto lane = laneArea.toFloat();
    g.setColour (graphBackground);
    g.fillRoundedRectangle (lane, 6.0f);
    g.saveState();
    g.reduceClipRegion (laneArea);

    // what to draw: the chords, or (while dragging one) the rest + the dragged chord
    const MidiChordTrack* shown = dragBase != nullptr ? dragBase.get() : track.get();
    const bool showDragSeg = dragBase != nullptr && (drag == Drag::move || drag == Drag::resizeLeft || drag == Drag::resizeRight || drag == Drag::draw);

    // piano roll rows (expanded)
    if (expanded)
    {
        for (int m = (int) std::floor (viewLo); m <= (int) std::ceil (viewHi); ++m)
        {
            const float yTop = yFor ((float) m + 0.5f), yBot = yFor ((float) m - 0.5f);
            const int pc = ((m % 12) + 12) % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            if (black) { g.setColour (backgroundDeep.withAlpha (0.55f)); g.fillRect (juce::Rectangle<float> ((float) rollArea.getX(), yTop, (float) rollArea.getWidth(), yBot - yTop)); }
            g.setColour (graphGridLine);
            g.drawHorizontalLine (juce::roundToInt (yBot), (float) rollArea.getX(), (float) rollArea.getRight());
            // keys
            g.setColour (black ? pianoBlackKey : pianoWhiteKey.withAlpha (0.85f));
            g.fillRect (juce::Rectangle<float> ((float) keysArea.getX(), yTop, (float) keysArea.getWidth() - 2.0f, yBot - yTop).reduced (0.0f, 0.5f));
            if (pc == 0 && yBot - yTop > 7.0f)
            {
                g.setFont (HarmonyEngineUI::font (9.0f, true));
                g.setColour (backgroundDeep);
                g.drawText ("C" + juce::String (m / 12 - 1), juce::Rectangle<float> ((float) keysArea.getX() + 2.0f, yTop, (float) keysArea.getWidth() - 6.0f, yBot - yTop), juce::Justification::centredRight, false);
            }
        }
        // chord tones shaded under each chord (root stronger)
        auto shade = [&] (const MidiChordTrack::Segment& s, int idx)
        {
            const double a = s.start + off, b = s.end + off;
            if (b < viewStart || a > viewStart + viewBeats) return;
            const float x1 = xFor (a), x2 = xFor (b);
            const auto col = idx % 2 ? accent : cyanAccent;
            for (int m = (int) std::floor (viewLo); m <= (int) std::ceil (viewHi); ++m)
            {
                const int pc = ((m % 12) + 12) % 12;
                if (! ((s.mask >> pc) & 1u)) continue;
                g.setColour (col.withAlpha (pc == s.root ? 0.20f : 0.10f));
                g.fillRect (juce::Rectangle<float> (x1, yFor ((float) m + 0.5f), x2 - x1, yFor ((float) m - 0.5f) - yFor ((float) m + 0.5f)));
            }
        };
        if (shown != nullptr) { int i = 0; for (const auto& s : shown->segments) shade (s, ++i); }
        if (showDragSeg) shade (dragSeg, 0);
    }

    // beat grid (4/4 bars)
    for (int b = (int) std::floor (viewStart); b <= (int) std::ceil (viewStart + viewBeats); ++b)
    {
        const float x = xFor ((double) b);
        const bool bar = (b % 4 + 4) % 4 == 0;
        if (! bar && viewBeats > 96.0) continue;
        g.setColour (bar ? graphGridLineBright : graphGridLine);
        g.drawVerticalLine (juce::roundToInt (x), lane.getY(), lane.getBottom());
        if (bar && (viewBeats <= 64.0 || (b / 4) % 2 == 0))
        {
            g.setFont (HarmonyEngineUI::font (9.5f, false));
            g.setColour (textSecondary.withAlpha (0.7f));
            g.drawText (juce::String (b / 4 + 1), juce::Rectangle<float> (x + 3.0f, lane.getY() + 1.0f, 30.0f, 11.0f), juce::Justification::topLeft, false);
        }
    }

    // chord blocks
    auto drawChord = [&] (const MidiChordTrack::Segment& s, int idx, bool isSel, bool ghost)
    {
        const double a = s.start + off, b = s.end + off;
        if (b < viewStart || a > viewStart + viewBeats) return;
        const auto row = chordRow.toFloat();
        auto box = juce::Rectangle<float> (xFor (a) + 1.0f, row.getY() + (expanded ? 2.0f : 0.0f), juce::jmax (2.0f, xFor (b) - xFor (a) - 2.0f), row.getHeight() - (expanded ? 4.0f : 0.0f));
        const bool current = pos >= a && pos < b;
        const auto col = idx % 2 ? accent : cyanAccent;
        g.setColour (col.withAlpha (ghost ? 0.15f : (current ? 0.42f : 0.24f)));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (isSel ? textPrimary : col.withAlpha (0.85f));
        g.drawRoundedRectangle (box, 4.0f, isSel || current ? 1.8f : 1.0f);
        if (box.getWidth() > 18.0f)
        {
            g.setFont (HarmonyEngineUI::font (expanded ? 12.5f : 11.0f, true));
            g.setColour (textPrimary);
            g.drawText (MidiChordTrack::chordName (s.root, s.quality), box.reduced (5.0f, 0.0f), juce::Justification::centredLeft, false);
        }
        if (expanded && writeMode && isSel)
        {
            g.setColour (textPrimary.withAlpha (0.7f));
            g.fillRect (box.withWidth (3.0f).reduced (0.0f, 5.0f));
            g.fillRect (box.withX (box.getRight() - 3.0f).withWidth (3.0f).reduced (0.0f, 5.0f));
        }
    };
    if (shown != nullptr)
    {
        int idx = 0;
        for (const auto& s : shown->segments) { drawChord (s, ++idx, ! showDragSeg && idx - 1 == selected, false); }
    }
    if (showDragSeg) drawChord (dragSeg, 0, true, false);
    if (drag == Drag::none && dropPreview > noDrop * 0.5)
            {
                const auto row = chordRow.toFloat();
                const float x1 = xFor (dropPreview), x2 = xFor (dropPreview + juce::jmax (snapBeats(), 1.0));
                g.setColour (textPrimary.withAlpha (0.8f));
                const float dashes[] = { 4.0f, 3.0f };
                g.drawDashedLine ({ x1, row.getY(), x1, row.getBottom() }, dashes, 2, 1.5f);
                g.drawDashedLine ({ x1, row.getY(), x2, row.getY() }, dashes, 2, 1.5f);
                g.drawDashedLine ({ x1, row.getBottom(), x2, row.getBottom() }, dashes, 2, 1.5f);
            }

    // lead notes: green on a chord tone, orange off it
    for (const auto& n : lead)
    {
        if (n.end < viewStart || n.start > viewStart + viewBeats) continue;
        bool on = true;
        const MidiChordTrack* chordsNow = shown;
        if (chordsNow != nullptr)
        {
            const int pc = ((int) std::lround (n.midi) % 12 + 12) % 12;
            const auto* s = chordsNow->at (0.5 * (n.start + n.end) - off);
            on = s == nullptr || ((s->mask >> pc) & 1u);
        }
        const float y = yFor (n.midi);
        if (y < (float) rollArea.getY() - 2.0f || y > (float) rollArea.getBottom() + 2.0f) continue;
        g.setColour ((on ? mintAccent : accentBright).withAlpha (chordsNow != nullptr ? 0.95f : 0.6f));
        const float x1 = xFor (n.start), x2 = juce::jmax (x1 + 1.5f, xFor (n.end));
        const float hgt = expanded ? juce::jlimit (3.0f, 10.0f, (yFor (n.midi - 0.5f) - yFor (n.midi + 0.5f)) * 0.7f) : 3.0f;
        g.fillRoundedRectangle (juce::Rectangle<float> (x1, y - hgt * 0.5f, x2 - x1, hgt), juce::jmin (2.0f, hgt * 0.5f));
    }
    if (lead.empty())
    {
        g.setFont (HarmonyEngineUI::font (10.5f, false));
        g.setColour (textSecondary.withAlpha (0.8f));
        g.drawText ("Lead vocal shows here once the song (or your Mono take) has played", rollArea, juce::Justification::centred, false);
    }

    const float px = xFor (pos);
    g.setColour (cyanBright.withAlpha (0.8f));
    g.drawVerticalLine (juce::roundToInt (px), lane.getY(), lane.getBottom());
    g.restoreState();

    g.setColour (graphOutline);
    g.drawRoundedRectangle (lane, 6.0f, 1.0f);
    if (expanded)
    {
        g.setFont (HarmonyEngineUI::font (10.0f, false));
        g.setColour (textSecondary.withAlpha (0.75f));
        g.drawText (writeMode ? (armed ? "Armed: " + MidiChordTrack::chordName (armedRoot, armedQuality) + " - click the chord row to place it" : juce::String ("Pick a chord above, or drag one onto the chord row"))
                              : juce::String ("Wheel: pitch  |  Shift+wheel: time  |  Ctrl+wheel: zoom  |  Alt+wheel: pitch zoom  |  drag: move around  |  drag the chord row: shift chords"),
                    laneArea.withTop (laneArea.getBottom() - 14).reduced (6, 0), juce::Justification::centredRight, false);
    }
}

//==============================================================================
// Mouse
void ChordLane::mouseMove (const juce::MouseEvent& e)
{
    if (! (expanded && writeMode) || ! chordRow.contains (e.getPosition()))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }
    const int idx = segmentAtX (e.position.x);
    juce::MouseCursor c = juce::MouseCursor::NormalCursor;
    if (idx >= 0)
    {
        const auto& s = track->segments[(size_t) idx];
        const float x1 = xFor (s.start + offset()), x2 = xFor (s.end + offset());
        c = (e.position.x - x1 < 6.0f || x2 - e.position.x < 6.0f) ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
    }
    else if (armed) c = juce::MouseCursor::CopyingCursor;
    setMouseCursor (c);
}

void ChordLane::mouseDown (const juce::MouseEvent& e)
{
    if (expanded) grabKeyboardFocus();
    drag = Drag::none;
    if (! laneArea.contains (e.getPosition())) return;
    dragStartX = e.position.x; dragStartY = e.position.y;
    dragStartOffset = offset(); dragStartViewStart = viewStart; dragStartLo = viewLo;

    if (expanded && writeMode && chordRow.contains (e.getPosition()))
    {
        const int idx = segmentAtX (e.position.x);
        if (idx >= 0)
        {
            selected = idx;
            if (e.mods.isPopupMenu()) { showChordMenu (idx); return; }
            dragSeg = track->segments[(size_t) idx];
            const float x1 = xFor (dragSeg.start + offset()), x2 = xFor (dragSeg.end + offset());
            drag = e.position.x - x1 < 6.0f ? Drag::resizeLeft : x2 - e.position.x < 6.0f ? Drag::resizeRight : Drag::move;
            dragAnchor = ppqFor (e.position.x) - offset() - dragSeg.start;   // grab point inside the chord
            dragBase = editableCopy();
            dragBase->remove ((size_t) idx);
        }
        else if (armed && ! e.mods.isPopupMenu())
        {
            const double start = snap (ppqFor (e.position.x)) - offset();
            dragBase = editableCopy();
            const double next = dragBase->nextStartAfter (start + 1.0e-6);
            dragSeg = MidiChordTrack::makeChord (armedRoot, armedQuality, start, juce::jmax (start + snapBeats(), juce::jmin (start + 4.0, next)));
            drag = Drag::draw;
            selected = -1;
        }
        else
        {
            const double at = ppqFor (e.position.x);
            chooseChord ([this, at] (int r, int q) { armedRoot = r; armedQuality = q; armed = true; placeChord (r, q, at); palette.repaint(); });
        }
        repaint();
        return;
    }
    if (expanded && rollArea.contains (e.getPosition())) { drag = Drag::pan; following = false; return; }
    drag = Drag::offset;   // compact lane, or the chord row outside write mode: shift the chords
}

void ChordLane::mouseDrag (const juce::MouseEvent& e)
{
    const double beats = (double) (e.position.x - dragStartX) / juce::jmax (1, (expanded ? rollArea : laneArea).getWidth()) * viewBeats;
    switch (drag)
    {
        case Drag::offset:
        {
            const double q = e.mods.isAltDown() ? 0.0625 : 0.25;   // 1/4 beat, Alt: 1/16
            setOffset (dragStartOffset + std::round (beats / q) * q);
            message = {};
            break;
        }
        case Drag::pan:
        {
            viewStart = dragStartViewStart - beats;
            const float semis = (e.position.y - dragStartY) / juce::jmax (1.0f, (float) rollArea.getHeight()) * (viewHi - viewLo);
            const float span = viewHi - viewLo;
            viewLo = juce::jlimit (12.0f, 108.0f - span, dragStartLo + semis);
            viewHi = viewLo + span;
            break;
        }
        case Drag::move:
        {
            const double len = dragSeg.end - dragSeg.start;
            const double start = snap (ppqFor (e.position.x) - dragAnchor) - offset();
            dragSeg.start = start; dragSeg.end = start + len;
            break;
        }
        case Drag::resizeLeft:
            dragSeg.start = juce::jmin (dragSeg.end - snapBeats(), snap (ppqFor (e.position.x)) - offset());
            break;
        case Drag::resizeRight:
        case Drag::draw:
            dragSeg.end = juce::jmax (dragSeg.start + snapBeats(), snap (ppqFor (e.position.x)) - offset());
            break;
        case Drag::none: break;
    }
    repaint();
}

void ChordLane::mouseUp (const juce::MouseEvent&)
{
    if (dragBase != nullptr && (drag == Drag::move || drag == Drag::resizeLeft || drag == Drag::resizeRight || drag == Drag::draw))
    {
        auto t = dragBase;
        dragBase.reset();
        t->overwrite (dragSeg);
        commit (t);
        selected = track->indexAt (dragSeg.start + 1.0e-6);
    }
    dragBase.reset();
    drag = Drag::none;
    repaint();
}

void ChordLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (expanded && writeMode && chordRow.contains (e.getPosition()))
    {
        if (const int idx = segmentAtX (e.position.x); idx >= 0) showChordMenu (idx);
        return;
    }
    // compact lane, or the chord row: back to the chords' own timing
    if ((! expanded && laneArea.contains (e.getPosition())) || (expanded && chordRow.contains (e.getPosition())))
    { setOffset (0.0); message = {}; }
}

void ChordLane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float d = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        // zoom time around the mouse
        const double at = ppqFor (e.position.x);
        viewBeats = juce::jlimit (4.0, 256.0, viewBeats * (d > 0 ? 0.8 : 1.25));
        viewStart = at - (double) (e.position.x - (float) (expanded ? rollArea : laneArea).getX()) / juce::jmax (1, (expanded ? rollArea : laneArea).getWidth()) * viewBeats;
    }
    else if (expanded && e.mods.isAltDown())
    {
        const float c = 0.5f * (viewLo + viewHi), half = juce::jlimit (4.0f, 30.0f, 0.5f * (viewHi - viewLo) * (d > 0 ? 0.88f : 1.14f));
        viewLo = c - half; viewHi = c + half;
    }
    else if (! expanded || e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        viewStart -= (double) d * viewBeats * 0.3;   // scroll time
        following = false;
    }
    else
    {
        const float span = viewHi - viewLo;
        viewLo = juce::jlimit (12.0f, 108.0f - span, viewLo + d * 3.0f);   // scroll pitch
        viewHi = viewLo + span;
    }
    repaint();
}

bool ChordLane::keyPressed (const juce::KeyPress& key)
{
    if (! expanded) return false;
    const auto mods = key.getModifiers();
    if ((mods.isCtrlDown() || mods.isCommandDown()) && (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z'))
    {
        auto& from = mods.isShiftDown() ? redoStack : undoStack;
        auto& to   = mods.isShiftDown() ? undoStack : redoStack;
        if (from.empty()) return true;
        if (track != nullptr) to.push_back (track);
        track = from.back(); from.pop_back();
        if (hooks.publish) hooks.publish (track);
        selected = -1; repaint();
        return true;
    }
    if ((mods.isCtrlDown() || mods.isCommandDown()) && (key.getKeyCode() == 'Y' || key.getKeyCode() == 'y'))
        return keyPressed (juce::KeyPress ('z', juce::ModifierKeys (juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier), 0));
    if (writeMode && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)) { deleteSelected(); return true; }
    if (key == juce::KeyPress::escapeKey) { if (armed) { armed = false; palette.repaint(); repaint(); } else setExpanded (false); return true; }
    return false;
}

//==============================================================================
// Drag and drop from the chord chips
bool ChordLane::isInterestedInDragSource (const SourceDetails& d) { return d.description.toString().startsWith ("chord:"); }
void ChordLane::itemDragMove (const SourceDetails& d)
{
    dropPreview = chordRow.getUnion (rollArea).contains (d.localPosition) ? snap (ppqFor ((float) d.localPosition.x)) : noDrop;
    repaint();
}
void ChordLane::itemDragExit (const SourceDetails&) { dropPreview = noDrop; repaint(); }
void ChordLane::itemDropped (const SourceDetails& d)
{
    dropPreview = noDrop;
    const auto parts = juce::StringArray::fromTokens (d.description.toString(), ":", "");
    if (parts.size() < 3) return;
    placeChord (parts[1].getIntValue(), parts[2].getIntValue(), ppqFor ((float) d.localPosition.x));
}

//==============================================================================
// Chord chips
void ChordLane::Palette::rebuild()
{
    chips.clear();
    const int key = owner.keyRoot();
    const bool minor = owner.keyMinor();
    static const int majDeg[7] = { 0, 2, 4, 5, 7, 9, 11 }, minDeg[7] = { 0, 2, 3, 5, 7, 8, 10 };
    static const int majQ[7] = { ChordTracker::major, ChordTracker::minor, ChordTracker::minor, ChordTracker::major, ChordTracker::major, ChordTracker::minor, ChordTracker::dim };
    static const int minQ[7] = { ChordTracker::minor, ChordTracker::dim, ChordTracker::major, ChordTracker::minor, ChordTracker::minor, ChordTracker::major, ChordTracker::major };
    for (int i = 0; i < 7; ++i)
        chips.push_back ({ (key + (minor ? minDeg[i] : majDeg[i])) % 12, minor ? minQ[i] : majQ[i], {} });
    // the dominant 7th of the key is common enough to have its own chip
    chips.push_back ({ (key + 7) % 12, ChordTracker::dom7, {} });
    chips.push_back ({ 0, 0, {}, true });
    auto r = getLocalBounds();
    for (auto& c : chips)
    {
        c.r = r.removeFromLeft (c.more ? 84 : 58).reduced (2, 2);
        r.removeFromLeft (2);
    }
}

int ChordLane::Palette::hit (juce::Point<int> p) const
{
    for (size_t i = 0; i < chips.size(); ++i) if (chips[i].r.contains (p)) return (int) i;
    return -1;
}

void ChordLane::Palette::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    if (chips.empty()) rebuild();
    for (const auto& c : chips)
    {
        const bool isArmed = ! c.more && owner.armed && owner.armedRoot == c.root && owner.armedQuality == c.quality;
        g.setColour (isArmed ? accent.withAlpha (0.45f) : panelLight);
        g.fillRoundedRectangle (c.r.toFloat(), 5.0f);
        g.setColour (isArmed ? accentBright : graphOutline);
        g.drawRoundedRectangle (c.r.toFloat(), 5.0f, 1.0f);
        g.setFont (HarmonyEngineUI::font (12.0f, true));
        g.setColour (textPrimary);
        g.drawText (c.more ? juce::String ("More...") : MidiChordTrack::chordName (c.root, c.quality), c.r, juce::Justification::centred, false);
    }
    g.setFont (HarmonyEngineUI::font (10.5f, false));
    g.setColour (textSecondary);
    const int x = chips.empty() ? 0 : chips.back().r.getRight() + 10;
    g.drawText (juce::String ("Chords in ") + pcLabel (owner.keyRoot()) + (owner.keyMinor() ? " minor" : " major") + " (Tune tab key). Drag one onto the timeline.",
                getLocalBounds().withLeft (x), juce::Justification::centredLeft, false);
}

void ChordLane::Palette::mouseDown (const juce::MouseEvent& e)
{
    pressed = hit (e.getPosition());
    if (pressed < 0) return;
    const auto& c = chips[(size_t) pressed];
    if (c.more)
    {
        owner.chooseChord ([this] (int r, int q) { owner.armedRoot = r; owner.armedQuality = q; owner.armed = true; repaint(); owner.repaint(); });
        pressed = -1;
        return;
    }
    owner.armedRoot = c.root; owner.armedQuality = c.quality; owner.armed = true;
    repaint(); owner.repaint();
}

void ChordLane::Palette::mouseDrag (const juce::MouseEvent& e)
{
    if (pressed < 0 || e.getDistanceFromDragStart() < 5 || owner.isDragAndDropActive()) return;
    const auto& c = chips[(size_t) pressed];
    owner.startDragging ("chord:" + juce::String (c.root) + ":" + juce::String (c.quality), this,
                         juce::ScaledImage (createComponentSnapshot (c.r)), true);
}
