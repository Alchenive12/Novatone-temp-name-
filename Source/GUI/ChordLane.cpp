#include "ChordLane.h"
#include "HarmonyEngineColours.h"
#include "ModuleTabs.h"

ChordLane::ChordLane (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    auto& laf = HarmonyEngineLookAndFeel::shared();
    clearButton.setButtonText (juce::CharPointer_UTF8 ("\xc3\x97"));
    leftButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x97\x80"));
    rightButton.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb6"));
    importButton.getProperties().set ("accentButton", true);
    clearButton.getProperties().set ("ghost", true);
    for (auto* b : { &importButton, &clearButton, &leftButton, &rightButton, &alignButton })
    {
        b->setLookAndFeel (&laf);
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    importButton.setTooltip ("Import a MIDI file with the song's chords (export the chord pattern from FL Studio). "
                             "Harmonies then follow these chords by song position, no sidechain needed.");
    clearButton.setTooltip ("Remove the MIDI file");
    leftButton.setTooltip ("Move the chords 1 beat earlier (Shift: 1/4 beat, Ctrl: 1 bar)");
    rightButton.setTooltip ("Move the chords 1 beat later (Shift: 1/4 beat, Ctrl: 1 bar)");
    alignButton.setTooltip ("Line the chords up with your lead vocal: finds the shift (up to 4 bars either way) "
                            "where the most of the lead lands on chord tones. Play the song (or your Mono take) once first.");

    importButton.onClick = [this] { importFile(); };
    clearButton.onClick  = [this] { if (hooks.clear) hooks.clear(); track.reset(); message = {}; repaint(); };
    leftButton.onClick   = [this] { nudge (-1.0, juce::ModifierKeys::getCurrentModifiers()); };
    rightButton.onClick  = [this] { nudge (+1.0, juce::ModifierKeys::getCurrentModifiers()); };
    alignButton.onClick  = [this] { autoAlign(); };
    startTimerHz (30);
}

ChordLane::~ChordLane()
{
    stopTimer();
    for (auto* b : { &importButton, &clearButton, &leftButton, &rightButton, &alignButton })
        b->setLookAndFeel (nullptr);
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
            if (safe->hooks.load (f, err))
            {
                safe->track = safe->hooks.track ? safe->hooks.track() : nullptr;
                safe->message = "Loaded " + f.getFileName() + ". Play the song with the vocal, then Auto-align (or drag the lane).";
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
}

void ChordLane::autoAlign()
{
    refreshLead (true);
    if (track == nullptr) { message = "Import a MIDI file first."; repaint(); return; }
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

void ChordLane::timerCallback()
{
    if (hooks.track)
    {
        auto t = hooks.track();
        if (t != track) { track = t; fitOffset = 1.0e9; }
    }
    refreshLead (false);
    pos = hooks.positionPpq ? hooks.positionPpq() : 0.0;
    const bool moving = hooks.moving && hooks.moving();
    if (moving) following = true;
    if (following && ! dragging)
    {
        // keep the playhead about a quarter in; page by whole bars
        if (pos < viewStart + viewBeats * 0.1 || pos > viewStart + viewBeats * 0.85)
            viewStart = juce::jmax (-4.0, std::floor ((pos - viewBeats * 0.25) / 4.0) * 4.0);
    }
    // current fit (recomputed when the offset or the lead changes)
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
    alignButton.setBounds (top.removeFromRight (96));
    top.removeFromRight (8);
    rightButton.setBounds (top.removeFromRight (28));
    top.removeFromRight (2);
    leftButton.setBounds (top.removeFromRight (28));
    top.removeFromRight (8);
    infoArea = top.reduced (10, 0);
    r.removeFromTop (6);
    laneArea = r;
}

float ChordLane::xFor (double ppq) const
{
    return (float) laneArea.getX() + (float) ((ppq - viewStart) / viewBeats) * (float) laneArea.getWidth();
}

double ChordLane::ppqFor (float x) const
{
    return viewStart + (double) (x - (float) laneArea.getX()) / juce::jmax (1, laneArea.getWidth()) * viewBeats;
}

void ChordLane::paint (juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    HarmonyEngineUI::drawLiftedPanel (g, getLocalBounds(), 10.0f);

    const double off = offset();
    // info line
    {
        juce::String txt;
        if (track == nullptr) txt = message.isNotEmpty() ? message : "Chords from a MIDI file: import one, then line it up with the lead.";
        else
        {
            txt = track->fileName + "   |   offset " + (off > 0 ? "+" : "") + juce::String (off, 2) + " beats";
            if (fitNow >= 0.0f) txt += "   |   lead on chord tones: " + juce::String (juce::roundToInt (fitNow * 100.0f)) + "%";
            if (message.isNotEmpty()) txt = message;
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

    // beat grid (4/4 bars)
    for (int b = (int) std::floor (viewStart); b <= (int) std::ceil (viewStart + viewBeats); ++b)
    {
        const float x = xFor ((double) b);
        const bool bar = (b % 4 + 4) % 4 == 0;
        g.setColour (bar ? graphGridLineBright : graphGridLine);
        g.drawVerticalLine (juce::roundToInt (x), lane.getY(), lane.getBottom());
        if (bar)
        {
            g.setFont (HarmonyEngineUI::font (9.5f, false));
            g.setColour (textSecondary.withAlpha (0.7f));
            g.drawText (juce::String (b / 4 + 1), juce::Rectangle<float> (x + 3.0f, lane.getY() + 1.0f, 30.0f, 11.0f), juce::Justification::topLeft, false);
        }
    }

    // chord blocks (top strip)
    const auto chordRow = lane.withTrimmedTop (13.0f).withHeight (juce::jmin (26.0f, lane.getHeight() * 0.42f));
    if (track != nullptr)
    {
        int idx = 0;
        for (const auto& s : track->segments)
        {
            const double a = s.start + off, b = s.end + off;
            ++idx;
            if (b < viewStart || a > viewStart + viewBeats) continue;
            auto box = juce::Rectangle<float> (xFor (a) + 1.0f, chordRow.getY(), juce::jmax (2.0f, xFor (b) - xFor (a) - 2.0f), chordRow.getHeight());
            const bool current = pos >= a && pos < b;
            g.setColour ((idx % 2 ? accent : cyanAccent).withAlpha (current ? 0.42f : 0.22f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour ((idx % 2 ? accent : cyanAccent).withAlpha (0.8f));
            g.drawRoundedRectangle (box, 4.0f, current ? 1.6f : 1.0f);
            if (box.getWidth() > 18.0f)
            {
                g.setFont (HarmonyEngineUI::font (11.0f, true));
                g.setColour (textPrimary);
                g.drawText (MidiChordTrack::chordName (s.root, s.quality), box.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
            }
        }
    }

    // lead notes under the chords: green on a chord tone, orange off it
    const auto leadRow = lane.withTop (chordRow.getBottom() + 4.0f).reduced (0.0f, 2.0f);
    for (const auto& n : lead)
    {
        if (n.end < viewStart || n.start > viewStart + viewBeats) continue;
        const float y = leadRow.getBottom() - (n.midi - leadLo) / juce::jmax (1.0f, leadHi - leadLo) * leadRow.getHeight();
        bool on = true;
        if (track != nullptr)
        {
            const int pc = ((int) std::lround (n.midi) % 12 + 12) % 12;
            const auto* s = track->at (0.5 * (n.start + n.end) - off);
            on = s == nullptr || ((s->mask >> pc) & 1u);
        }
        g.setColour ((on ? mintAccent : accentBright).withAlpha (track != nullptr ? 0.95f : 0.6f));
        const float x1 = xFor (n.start), x2 = juce::jmax (x1 + 1.5f, xFor (n.end));
        g.fillRect (juce::Rectangle<float> (x1, y - 1.5f, x2 - x1, 3.0f));
    }
    if (lead.empty())
    {
        g.setFont (HarmonyEngineUI::font (10.5f, false));
        g.setColour (textSecondary.withAlpha (0.8f));
        g.drawText ("Lead vocal shows here once the song (or your Mono take) has played", leadRow, juce::Justification::centred, false);
    }

    // playhead
    const float px = xFor (pos);
    g.setColour (cyanBright.withAlpha (0.8f));
    g.drawVerticalLine (juce::roundToInt (px), lane.getY(), lane.getBottom());
    g.restoreState();

    g.setColour (graphOutline);
    g.drawRoundedRectangle (lane, 6.0f, 1.0f);
}

void ChordLane::mouseDown (const juce::MouseEvent& e)
{
    if (! laneArea.contains (e.getPosition())) return;
    dragging = true;
    dragStartOffset = offset();
    dragStartX = e.position.x;
}

void ChordLane::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    // drag = move the chords (snaps to 1/4 beat; Alt: 1/16)
    const double beats = (double) (e.position.x - dragStartX) / juce::jmax (1, laneArea.getWidth()) * viewBeats;
    const double snap = e.mods.isAltDown() ? 0.0625 : 0.25;
    setOffset (dragStartOffset + std::round (beats / snap) * snap);
    message = {};
}

void ChordLane::mouseUp (const juce::MouseEvent&) { dragging = false; }

void ChordLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (laneArea.contains (e.getPosition())) { setOffset (0.0); message = {}; }   // back to the file's own timing
}

void ChordLane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        viewBeats = juce::jlimit (4.0, 128.0, viewBeats * (w.deltaY > 0 ? 0.8 : 1.25));   // zoom
    else
    {
        viewStart -= (double) (w.deltaY != 0.0f ? w.deltaY : -w.deltaX) * viewBeats * 0.5;   // scroll
        following = false;
    }
    repaint();
}
