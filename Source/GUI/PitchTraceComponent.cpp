#include "PitchTraceComponent.h"
#include "../Poly/PolyClipSession.h"
#include "ModuleTabs.h"
#if __has_include("../DSP/MPMDetector.h")
 #include "../DSP/MPMDetector.h"
#else
 #include "MPMDetector.h"
#endif
#include <algorithm>
#include <cmath>
#include <limits>

PitchTraceComponent::PitchTraceComponent(PitchTraceQueue& queueToDrain)
    : queue(queueToDrain)
{
    popScratch.reserve(256);
    timeline.reserve(maxTimelineHops);
    notes.reserve(256);
    setOpaque(false);
    setWantsKeyboardFocus(true);

    lastAppliedSensitivity = queue.globalSensitivity.load(std::memory_order_relaxed);
    lastAppliedScaleMask   = queue.activeScaleMask.load(std::memory_order_relaxed);

    currentCoachHint = "Sing, play, or drop an audio file here. Drag notes to retune them.";

    // Seed an interactive vocal phrase so the NovaTone Piano Roll is immediately
    // testable with Center / Sensitivity / Variation / Transition / Formant before live audio starts.
    loadPhrasePreview();

    startTimerHz(60);
}

PitchTraceComponent::~PitchTraceComponent()
{
    stopTimer();
    if (clipSession != nullptr)
        clipSession->onClipChanged = nullptr;
}

//==============================================================================
// Poly clip integration
//==============================================================================
void PitchTraceComponent::setClipSession(Poly::ClipSession* session)
{
    clipSession = session;
    if (clipSession == nullptr) return;
    juce::Component::SafePointer<PitchTraceComponent> safe(this);
    clipSession->onClipChanged = [safe] { if (safe != nullptr) safe->handleClipSessionChanged(); };
    if (polyphonicModeEnabled)
        importClipFromSession();
}

void PitchTraceComponent::handleClipSessionChanged()
{
    if (clipSession == nullptr) return;
    const int st = static_cast<int>(clipSession->getStatus());
    const bool statusChanged = st != lastClipStatus;
    lastClipStatus = st;

    if (isClipMode())
    {
        // Re-import only when there's a new analysis (our own edits also trigger renders)
        if (clipSession->getAnalysis().get() != lastImportedAnalysis || (statusChanged && clipSession->getAnalysis() == nullptr))
            importClipFromSession();
        currentCoachHint = clipSession->getStatusText();
        repaint();
    }
    if (statusChanged && onClipStatusChanged)
        onClipStatusChanged();
}

void PitchTraceComponent::importClipFromSession()
{
    if (clipSession == nullptr) return;
    const auto an = clipSession->getAnalysis();
    lastImportedAnalysis = an.get();

    timeline.clear();
    notes.clear();
    undoStack.clear();
    redoStack.clear();
    selectedNoteId = hoveredNoteId = advancedEditNoteId = -1;
    clipPlayHop = -1;
    currentCoachHint = clipSession->getStatusText();

    if (an == nullptr)
    {
        notifySelectionChanged();
        repaint();
        return;
    }

    clipHopSize = an->hopSize;
    const auto clipNotes = clipSession->getNotes();
    timeline.resize((size_t) an->numFrames);
    for (int f = 0; f < an->numFrames; ++f)
    {
        auto& th = timeline[(size_t) f];
        th.data.hopIndex = f;
        th.data.isPolyphonic = true;
        th.timeStampSeconds = an->frameToSeconds(f);
        th.polyNoteIds.fill(-1);
    }

    float lo = 127.0f, hi = 0.0f;
    for (const auto& cn : clipNotes)
    {
        const Poly::Note* src = nullptr;
        for (const auto& n : an->notes) if (n.id == cn.sourceId) { src = &n; break; }
        const int lane = juce::jlimit(0, PitchTraceQueue::maxPolyVoices - 1, cn.lane);
        for (int f = juce::jmax(0, cn.startFrame); f < juce::jmin(an->numFrames, cn.endFrame); ++f)
        {
            auto& d = timeline[(size_t) f].data;
            const int si = src != nullptr ? f - src->startFrame : -1;
            const bool inSrc = src != nullptr && si >= 0 && si < (int) src->pitchMidi.size();
            d.polyRawMidi[(size_t) lane] = inSrc ? src->pitchMidi[(size_t) si] : cn.detectedMidi;
            d.polyVoiceRms[(size_t) lane] = inSrc ? src->amp[(size_t) si] : 0.0f;
            d.numPolyVoices = (uint8_t) juce::jmax((int) d.numPolyVoices, lane + 1);
            d.isVoiced = true;
            d.rms += 1.5f * d.polyVoiceRms[(size_t) lane];
            timeline[(size_t) f].rawMidi = juce::jmax(timeline[(size_t) f].rawMidi, d.polyRawMidi[(size_t) lane]);
        }

        NewtoneNoteBlock nb;
        nb.id = cn.id;
        nb.sourceId = cn.sourceId;
        nb.voiceLane = lane;
        nb.startHop = cn.startFrame;
        nb.endHop = cn.endFrame;
        nb.detectedMidi = cn.detectedMidi;
        nb.nearestMidi = (int) std::lround(cn.detectedMidi);
        nb.targetMidi = cn.targetMidi;
        nb.fineCents = cn.fineCents;
        nb.gainDb = cn.gainDb;
        nb.formantSemitones = cn.formantSemitones;
        nb.userEdited = cn.userEdited;
        nb.meanRms = src != nullptr ? src->level : 0.02f;
        notes.push_back(nb);
        nextNoteId = juce::jmax(nextNoteId, cn.id + 1);
        lo = juce::jmin(lo, cn.detectedMidi);
        hi = juce::jmax(hi, cn.detectedMidi);
    }
    relinkTimelineToNotes();

    viewStartHop = 0.0f;
    visibleHopsCount = juce::jlimit(180.0f, 1400.0f, (float) an->numFrames + 12.0f);
    if (hi >= lo)
    {
        viewCentreMidiNote = 0.5f * (lo + hi);
        viewHalfRangeSemitones = juce::jlimit(8.0f, 30.0f, 0.5f * (hi - lo) + 3.0f);
    }
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::pushNotesToClipSession()
{
    if (clipSession == nullptr) return;
    std::vector<Poly::ClipNote> out;
    out.reserve(notes.size());
    for (const auto& n : notes)
    {
        if (n.isSibilant) continue;
        Poly::ClipNote c;
        c.id = n.id;
        c.sourceId = n.sourceId >= 0 ? n.sourceId : n.id;
        c.lane = n.voiceLane;
        c.startFrame = n.startHop;
        c.endFrame = n.endHop;
        c.detectedMidi = n.detectedMidi;
        c.targetMidi = n.targetMidi;
        c.fineCents = n.fineCents;
        c.gainDb = n.gainDb;
        c.formantSemitones = n.formantSemitones;
        c.userEdited = n.userEdited;
        out.push_back(c);
    }
    clipSession->setNotes(out);
}

int PitchTraceComponent::playheadHopForPaint() const noexcept
{
    if (isClipMode())
        return clipPlayHop;
    const int scrubbedHop = queue.currentPlayheadHop.load(std::memory_order_relaxed);
    return (scrubbedHop > 0 && scrubbedHop < static_cast<int>(timeline.size())) ? scrubbedHop : (static_cast<int>(timeline.size()) - 1);
}

void PitchTraceComponent::setFrequencyRange(float minHz, float maxHz) noexcept
{
    if (minHz > 0.0f && maxHz > minHz)
    {
        const float loMidi = hzToMidiNote(minHz);
        const float hiMidi = hzToMidiNote(maxHz);
        if (std::isfinite(loMidi) && std::isfinite(hiMidi))
            viewCentreMidiNote = 0.5f * (loMidi + hiMidi);
    }
}

void PitchTraceComponent::resized() {}

//==============================================================================
// Pitch & Scale Helpers
//==============================================================================
float PitchTraceComponent::hzToMidiNote(float hz) noexcept
{
    if (hz <= 0.0f) return std::numeric_limits<float>::quiet_NaN();
    return 12.0f * std::log2(hz / 440.0f) + 69.0f;
}

float PitchTraceComponent::midiNoteToHz(float midi) noexcept
{
    return 440.0f * std::exp2((midi - 69.0f) / 12.0f);
}

juce::String PitchTraceComponent::midiNoteName(int midiNote)
{
    static constexpr const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pc = ((midiNote % 12) + 12) % 12;
    const int oct = (midiNote / 12) - 1;
    return juce::String(names[pc]) + juce::String(oct);
}

bool PitchTraceComponent::isBlackKey(int midiNote) noexcept
{
    const int pc = ((midiNote % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

bool PitchTraceComponent::isMidiNoteInScale(int midiNote, uint16_t scaleMask) const noexcept
{
    const int pc = ((midiNote % 12) + 12) % 12;
    return (scaleMask & static_cast<uint16_t>(1u << pc)) != 0;
}

int PitchTraceComponent::snapMidiToScale(float midiNote, uint16_t scaleMask) const noexcept
{
    if (scaleMask == 0)
        return static_cast<int>(std::lround(midiNote));

    const int base = static_cast<int>(std::floor(midiNote)) - 6;
    int bestNote = static_cast<int>(std::lround(midiNote));
    float bestDist = 1000.0f;

    for (int m = base; m <= base + 12; ++m)
    {
        if (!isMidiNoteInScale(m, scaleMask))
            continue;
        const float d = std::abs(midiNote - static_cast<float>(m));
        if (d < bestDist)
        {
            bestDist = d;
            bestNote = m;
        }
    }
    return bestNote;
}

//==============================================================================
// Undo / Redo & Selected-Note Inspector API
//==============================================================================
PitchTraceComponent::EditSnapshot PitchTraceComponent::captureSnapshot() const
{
    EditSnapshot s { notes, {} };
    s.drawnMidi.reserve(timeline.size());
    for (const auto& th : timeline)
        s.drawnMidi.push_back(th.hasCustomPitch ? th.customMidi : std::numeric_limits<float>::quiet_NaN());
    return s;
}

void PitchTraceComponent::restoreSnapshot(EditSnapshot&& s)
{
    notes = std::move(s.notes);
    for (size_t h = 0; h < timeline.size(); ++h)
    {
        const float m = h < s.drawnMidi.size() ? s.drawnMidi[h] : std::numeric_limits<float>::quiet_NaN();
        timeline[h].hasCustomPitch = ! std::isnan(m);
        timeline[h].customMidi = std::isnan(m) ? 0.0f : m;
    }
}

void PitchTraceComponent::pushUndoSnapshot()
{
    undoStack.push_back(captureSnapshot());
    if (undoStack.size() > maxUndoSteps)
        undoStack.erase(undoStack.begin());
    redoStack.clear();
}

void PitchTraceComponent::relinkTimelineToNotes() noexcept
{
    for (auto& th : timeline)
    {
        th.noteId = -1;
        th.polyNoteIds.fill(-1);
    }

    for (const auto& n : notes)
    {
        const int lane = juce::jlimit(0, PitchTraceQueue::maxPolyVoices - 1, n.voiceLane);
        for (int k = juce::jmax(0, n.startHop); k < juce::jmin(static_cast<int>(timeline.size()), n.endHop); ++k)
        {
            if (n.voiceLane == 0 || !polyphonicModeEnabled)
                timeline[(size_t)k].noteId = n.id;
            timeline[(size_t)k].polyNoteIds[(size_t)lane] = n.id;
        }
    }
}

void PitchTraceComponent::undo()
{
    if (undoStack.empty())
        return;

    redoStack.push_back(captureSnapshot());
    restoreSnapshot(std::move(undoStack.back()));
    undoStack.pop_back();
    relinkTimelineToNotes();
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::redo()
{
    if (redoStack.empty())
        return;

    undoStack.push_back(captureSnapshot());
    restoreSnapshot(std::move(redoStack.back()));
    redoStack.pop_back();
    relinkTimelineToNotes();
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

int PitchTraceComponent::getSelectedNoteCount() const noexcept
{
    int count = 0;
    for (const auto& n : notes)
        if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
            ++count;
    return count;
}

void PitchTraceComponent::selectAllNotes()
{
    int firstVoicedId = -1;
    int count = 0;
    for (auto& n : notes)
    {
        if (!n.isSibilant)
        {
            n.selected = true;
            if (firstVoicedId < 0) firstVoicedId = n.id;
            ++count;
        }
        else
        {
            n.selected = false;
        }
    }
    if (selectedNoteId < 0 || findNoteById(selectedNoteId) == nullptr)
        selectedNoteId = firstVoicedId;

    currentCoachHint = "Selected all " + juce::String(count) + " notes. Drag any note, use the arrow keys, or the inspector to edit them together.";
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::deselectAllNotes()
{
    for (auto& n : notes)
        n.selected = false;
    selectedNoteId = -1;
    advancedEditNoteId = -1;
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

const NewtoneNoteBlock* PitchTraceComponent::getSelectedNote() const noexcept
{
    if (const auto* primary = findNoteById(selectedNoteId))
        return primary;
    for (const auto& n : notes)
        if (!n.isSibilant && n.selected)
            return &n;
    return nullptr;
}

void PitchTraceComponent::notifySelectionChanged()
{
    if (onNoteSelectionChanged)
        onNoteSelectionChanged();
}

void PitchTraceComponent::transposeSelectedNote(int semitoneDelta)
{
    if (semitoneDelta == 0 || getSelectedNoteCount() == 0)
        return;

    pushUndoSnapshot();
    const uint16_t mask = queue.activeScaleMask.load(std::memory_order_relaxed);
    for (auto& n : notes)
    {
        if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
            continue;
        int candidate = juce::jlimit(12, 108, n.targetMidi + semitoneDelta);
        if (snapToScaleEnabled && mask != 0)
        {
            const int step = semitoneDelta > 0 ? 1 : -1;
            while (candidate >= 12 && candidate <= 108 && !isMidiNoteInScale(candidate, mask))
                candidate += step;
            candidate = juce::jlimit(12, 108, candidate);
        }
        n.targetMidi = candidate;
        n.userEdited = true;
    }
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::setSelectedNoteFineCents(float cents)
{
    const float clamped = juce::jlimit(-50.0f, 50.0f, cents);
    bool changed = false;
    for (auto& n : notes)
    {
        if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
        {
            n.fineCents  = clamped;
            n.userEdited = true;
            changed = true;
        }
    }
    if (changed)
    {
        syncActiveOverrideToEngine();
        repaint();
    }
}

void PitchTraceComponent::setSelectedNoteFormant(float semitones)
{
    const float clamped = juce::jlimit(-12.0f, 12.0f, semitones);
    bool changed = false;
    for (auto& n : notes)
    {
        if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
        {
            n.formantSemitones = clamped;
            n.userEdited       = true;
            changed = true;
        }
    }
    if (changed)
    {
        syncActiveOverrideToEngine();
        repaint();
    }
}

void PitchTraceComponent::setSelectedNoteVariation(float variation0to2)
{
    const float clamped = juce::jlimit(0.0f, 2.0f, variation0to2);
    bool changed = false;
    for (auto& n : notes)
    {
        if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
        {
            n.noteVariation = clamped;
            n.userEdited    = true;
            changed = true;
        }
    }
    if (changed)
    {
        syncActiveOverrideToEngine();
        repaint();
    }
}

void PitchTraceComponent::setSelectedNoteGainDb(float gainDb)
{
    const float clamped = juce::jlimit(-18.0f, 12.0f, gainDb);
    bool changed = false;
    for (auto& n : notes)
    {
        if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
        {
            n.gainDb     = clamped;
            n.userEdited = true;
            changed = true;
        }
    }
    if (changed)
    {
        syncActiveOverrideToEngine();
        repaint();
    }
}

void PitchTraceComponent::setSelectedNoteSibilantDb(float sibilantDb)
{
    const float clamped = juce::jlimit(-18.0f, 12.0f, sibilantDb);
    bool changed = false;
    for (auto& n : notes)
    {
        if (n.selected || n.id == selectedNoteId)
        {
            n.sibilantDb = clamped;
            n.userEdited = true;
            changed = true;
        }
    }
    if (changed)
    {
        syncActiveOverrideToEngine();
        repaint();
    }
}

void PitchTraceComponent::resetSelectedNote()
{
    if (getSelectedNoteCount() == 0)
        return;

    pushUndoSnapshot();
    const uint16_t mask = queue.activeScaleMask.load(std::memory_order_relaxed);
    for (auto& n : notes)
    {
        if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
            continue;
        n.targetMidi       = snapToScaleEnabled ? snapMidiToScale(n.detectedMidi, mask) : n.nearestMidi;
        n.fineCents        = 0.0f;
        n.noteCenter       = 1.0f;
        n.noteVariation    = 1.0f;
        n.formantSemitones = 0.0f;
        n.gainDb           = 0.0f;
        n.sibilantDb       = 0.0f;
        n.rampInRatio      = 0.18f;
        n.rampOutRatio     = 0.18f;
        n.fadeInRatio      = 0.0f;
        n.fadeOutRatio     = 0.0f;
        n.userEdited       = false;
        for (int k = juce::jmax(0, n.startHop); k < juce::jmin(static_cast<int>(timeline.size()), n.endHop); ++k)
            timeline[(size_t)k].hasCustomPitch = false;
    }
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

//==============================================================================
// Tool & Mode Setters
//==============================================================================
void PitchTraceComponent::setToolMode(ToolMode mode) noexcept
{
    currentToolMode = mode;
    if (mode == ToolMode::CutAndGlue)
        currentCoachHint = "Slice: click inside a note to split it. Ctrl+click to glue neighbouring notes.";
    else if (mode == ToolMode::AdvancedEdit)
        currentCoachHint = "Shape: drag the top of a note for formant, the middle for variation, the bottom for gain, the corners for ramps.";
    else if (mode == ToolMode::FreehandDraw)
        currentCoachHint = "Draw: paint a freehand pitch curve or vibrato over any note. Right-click to reset.";
    else
        currentCoachHint = "Move: drag a note for semitones, Alt+drag for cents. Ctrl+K detects the key, Ctrl+L levels volume.";
    repaint();
}

void PitchTraceComponent::selectAdjacentNote(int direction)
{
    if (notes.empty() || direction == 0)
        return;

    int curIdx = -1;
    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
    {
        if (notes[(size_t)i].id == selectedNoteId)
        {
            curIdx = i;
            break;
        }
    }

    int idx = (curIdx < 0) ? 0 : curIdx;
    const int n = static_cast<int>(notes.size());
    for (int step = 0; step < n; ++step)
    {
        idx = (idx + (direction > 0 ? 1 : -1) + n) % n;
        if (!notes[(size_t)idx].isSibilant)
        {
            for (auto& nb : notes)
                nb.selected = false;
            notes[(size_t)idx].selected = true;
            selectedNoteId = notes[(size_t)idx].id;
            syncActiveOverrideToEngine();
            notifySelectionChanged();
            repaint();
            return;
        }
    }
}

bool PitchTraceComponent::hasPreviewClip() const
{
    if (isClipMode())
        return clipSession->getStatus() == Poly::ClipSession::Status::ready;
    return ! polyphonicModeEnabled && queue.monoPreview.hasClip();
}

void PitchTraceComponent::setPreviewPlaying(bool shouldPlay)
{
    if (isClipMode())
    {
        clipSession->setPreviewPlaying(shouldPlay);
        return;
    }
    auto& mp = queue.monoPreview;
    if (shouldPlay && ! mp.hasClip()) return;
    if (shouldPlay && mp.pos.load() >= static_cast<double>(loadedAudioClip.getNumSamples()))
        mp.pos.store(0.0);
    mp.playing.store(shouldPlay);
}

void PitchTraceComponent::seekClipToStart()
{
    if (isClipMode())
    {
        clipSession->seekPreview(0);
        clipPlayHop = 0;
    }
    else
    {
        queue.monoPreview.pos.store(0.0);
        queue.currentPlayheadHop.store(0, std::memory_order_relaxed);
    }
    viewStartHop = 0.0f;
    repaint();
}

void PitchTraceComponent::setIndependentPlay(bool independent)
{
    queue.independentPlay.store(independent);
    if (clipSession != nullptr)
        clipSession->setIndependent(independent);
}

double PitchTraceComponent::hopToHostPpq(int hop) const noexcept
{
    if (isClipMode())
    {
        // shortcut: one tempo from song start (host's current BPM); use the host's PPQ per hop if tempo changes matter.
        const double rate = clipSession->getClipSampleRate();
        if (rate <= 0.0) return 0.0;
        const double sec = static_cast<double>(clipSession->getClipStart() + static_cast<juce::int64>(hop) * clipHopSize) / rate;
        return sec * queue.hostBpm.load(std::memory_order_relaxed) / 60.0;
    }
    return timeline[(size_t)hop].data.ppqPosition; // captured from the host while it played
}

int PitchTraceComponent::snapHopToHostGrid(int hop) const noexcept
{
    const int size = static_cast<int>(timeline.size());
    if (! snapToGridEnabled || size < 2) return hop;
    hop = juce::jlimit(0, size - 1, hop);
    constexpr double step = 0.25; // 1/16 note in quarter-note PPQ
    const double target = std::round(hopToHostPpq(hop) / step) * step;
    // Nearest hop to the grid line; no host PPQ (all equal) leaves the hop unchanged.
    int best = hop;
    double bestErr = std::abs(hopToHostPpq(hop) - target);
    for (int k = juce::jmax(0, hop - 128); k <= juce::jmin(size - 1, hop + 128); ++k)
    {
        const double err = std::abs(hopToHostPpq(k) - target);
        if (err < bestErr) { bestErr = err; best = k; }
    }
    return best;
}

void PitchTraceComponent::setSnapToScale(bool shouldSnap) noexcept
{
    snapToScaleEnabled = shouldSnap;
    queue.snapToScale.store(shouldSnap, std::memory_order_relaxed);
    repaint();
}

void PitchTraceComponent::setAutoScroll(bool shouldFollow) noexcept
{
    autoScrollEnabled = shouldFollow;
    queue.autoScroll.store(shouldFollow, std::memory_order_relaxed);
    repaint();
}

void PitchTraceComponent::setPolyphonicMode(bool polyEnabled)
{
    if (polyphonicModeEnabled == polyEnabled)
        return;

    polyphonicModeEnabled = polyEnabled;
    queue.globalPolyphonic.store(polyEnabled, std::memory_order_relaxed);

    if (clipSession != nullptr)
    {
        if (polyphonicModeEnabled)
        {
            importClipFromSession();
            if (onClipStatusChanged) onClipStatusChanged();
            repaint();
            return;
        }
        // Back to mono: the clip stays in the session; the roll returns to live tuning.
        timeline.clear();
        notes.clear();
        lastImportedAnalysis = nullptr;
        if (onClipStatusChanged) onClipStatusChanged();
    }

    if (!clearedPreviewOnFirstLiveVoice)
    {
        if (polyphonicModeEnabled)
            loadPolyphonicChordPreview();
        else
            loadPhrasePreview();
    }
    else
    {
        rebuildSegmentationFromSensitivity();
        notifySelectionChanged();
    }

    currentCoachHint = polyphonicModeEnabled
        ? "Poly mode: chord, choir and string detection. Drag individual chord notes to re-voice harmonies."
        : "Mono mode: solo-voice detection and tuning.";
    repaint();
}

void PitchTraceComponent::setMidiCleanMode(bool cleanEnabled)
{
    if (midiCleanModeEnabled == cleanEnabled)
        return;

    midiCleanModeEnabled = cleanEnabled;
    queue.globalMidiCleanMode.store(cleanEnabled, std::memory_order_relaxed);

    rebuildSegmentationFromSensitivity();
    notifySelectionChanged();

    currentCoachHint = midiCleanModeEnabled
        ? "MIDI clean on: noise gate, gap bridging, micro-note removal and legato stitching."
        : "MIDI clean off: raw frame-by-frame segmentation.";
    repaint();
}

void PitchTraceComponent::snapAllNotesToScale()
{
    pushUndoSnapshot();
    const uint16_t mask = queue.activeScaleMask.load(std::memory_order_relaxed);
    for (auto& n : notes)
    {
        if (n.isSibilant) continue;
        n.targetMidi = snapMidiToScale(n.detectedMidi, mask);
        n.fineCents = 0.0f;
        n.userEdited = true;
    }
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::resetAllNotes()
{
    pushUndoSnapshot();
    const uint16_t mask = queue.activeScaleMask.load(std::memory_order_relaxed);
    for (auto& n : notes)
    {
        n.targetMidi       = snapToScaleEnabled ? snapMidiToScale(n.detectedMidi, mask) : n.nearestMidi;
        n.fineCents        = 0.0f;
        n.noteCenter       = 1.0f;
        n.noteVariation    = 1.0f;
        n.formantSemitones = 0.0f;
        n.gainDb           = 0.0f;
        n.sibilantDb       = 0.0f;
        n.rampInRatio      = 0.18f;
        n.rampOutRatio     = 0.18f;
        n.fadeInRatio      = 0.0f;
        n.fadeOutRatio     = 0.0f;
        n.warpOffsetHops   = 0;
        n.userEdited       = false;
    }
    for (auto& th : timeline)
        th.hasCustomPitch = false;
    queue.getLiveOverride().active.store(false, std::memory_order_relaxed);
    queue.clearHopOverrides();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::clearTimeline()
{
    if (isClipMode())
    {
        clipSession->clear();
        return;
    }

    pushUndoSnapshot();
    timeline.clear();
    notes.clear();
    referenceGhostNotes.clear();
    loadedAudioClip.setSize(0, 0);
    queue.monoPreview.clear();
    selectedNoteId = -1;
    hoveredNoteId = -1;
    advancedEditNoteId = -1;
    viewStartHop = 0.0f;
    queue.currentPlayheadHop.store(0, std::memory_order_relaxed);
    queue.getLiveOverride().active.store(false, std::memory_order_relaxed);
    queue.clearHopOverrides();
    notifySelectionChanged();
    repaint();
}

bool PitchTraceComponent::detectKeyAndScaleFromTimeline(int& outRoot, int& outMode, float& outConfidence) const noexcept
{
    // Krumhansl-Kessler / Temperley cognitive pitch-class profiles for Major and Minor keys:
    static constexpr float majorProfile[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static constexpr float minorProfile[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

    std::array<double, 12> chroma {};
    double totalWeight = 0.0;

    for (const auto& th : timeline)
    {
        if (!th.data.isVoiced)
            continue;

        const float c = juce::jlimit(0.25f, 1.0f, th.data.clarity);
        const float r = juce::jmax(0.001f, th.data.rms);
        const double baseW = static_cast<double>(r * c * c);

        if (polyphonicModeEnabled && th.data.numPolyVoices > 0)
        {
            for (int v = 0; v < static_cast<int>(th.data.numPolyVoices) && v < PitchTraceQueue::maxPolyVoices; ++v)
            {
                const float m = th.data.polyRawMidi[(size_t)v];
                if (m <= 12.0f) continue;
                const int nearest = static_cast<int>(std::lround(m));
                const float err = m - static_cast<float>(nearest);
                const double w = baseW * std::exp(-2.5 * static_cast<double>(err * err));
                const int pc = ((nearest % 12) + 12) % 12;
                chroma[(size_t)pc] += w;
                totalWeight += w;
            }
        }
        else if (th.rawMidi > 12.0f)
        {
            const int nearest = static_cast<int>(std::lround(th.rawMidi));
            const float err = th.rawMidi - static_cast<float>(nearest);
            const double w = baseW * std::exp(-2.5 * static_cast<double>(err * err));
            const int pc = ((nearest % 12) + 12) % 12;
            chroma[(size_t)pc] += w;
            totalWeight += w;
        }
    }

    if (totalWeight <= 1.0e-6)
    {
        outRoot = 0;
        outMode = 0;
        outConfidence = 0.0f;
        return false;
    }

    double chromaMean = 0.0;
    for (double v : chroma) chromaMean += v;
    chromaMean /= 12.0;

    auto correlate = [&](const float profile[12], int root) noexcept -> double
    {
        double pMean = 0.0;
        for (int i = 0; i < 12; ++i) pMean += profile[i];
        pMean /= 12.0;

        double num = 0.0, denC = 0.0, denP = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double dc = chroma[(size_t)((root + i) % 12)] - chromaMean;
            const double dp = static_cast<double>(profile[i]) - pMean;
            num  += dc * dp;
            denC += dc * dc;
            denP += dp * dp;
        }
        return (denC > 1.0e-12 && denP > 1.0e-12) ? (num / std::sqrt(denC * denP)) : 0.0;
    };

    int bestRoot = 0;
    int bestMode = 0; // 0 = Major, 1 = Minor
    double bestCorr = -2.0;

    for (int r = 0; r < 12; ++r)
    {
        const double majC = correlate(majorProfile, r);
        if (majC > bestCorr) { bestCorr = majC; bestRoot = r; bestMode = 0; }

        const double minC = correlate(minorProfile, r);
        if (minC > bestCorr) { bestCorr = minC; bestRoot = r; bestMode = 1; }
    }

    outRoot = bestRoot;
    outMode = bestMode;
    outConfidence = static_cast<float>(juce::jlimit(0.0, 1.0, (bestCorr + 0.25) / 1.15));
    return true;
}

void PitchTraceComponent::autoDetectKeyAndApply()
{
    int detRoot = 0, detMode = 0;
    float confidence = 0.0f;
    if (!detectKeyAndScaleFromTimeline(detRoot, detMode, confidence))
    {
        currentCoachHint = "Detect key: sing or load a phrase first so there is something to analyse.";
        repaint(getCoachBarArea().toNearestInt());
        return;
    }

    static constexpr const char* rootNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static constexpr int majorSteps[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minorSteps[7] = { 0, 2, 3, 5, 7, 8, 10 };

    uint16_t newMask = 0;
    for (int s : (detMode == 1 ? minorSteps : majorSteps))
        newMask |= static_cast<uint16_t>(1u << ((detRoot + s) % 12));

    queue.activeScaleMask.store(newMask, std::memory_order_relaxed);
    lastAppliedScaleMask = newMask;

    if (onAutoKeyDetected)
        onAutoKeyDetected(detRoot, detMode);

    if (snapToScaleEnabled)
        snapAllNotesToScale();

    const juce::String keyName = juce::String(rootNames[detRoot]) + (detMode == 1 ? " Minor" : " Major");
    currentCoachHint = "Detected " + keyName + " (" + juce::String(juce::roundToInt(confidence * 100.0f))
                     + "% confidence). Scale set and notes snapped.";
    repaint();
}

void PitchTraceComponent::levelAllNotesVolume(float levelingAmount0to1)
{
    const float amt = juce::jlimit(0.0f, 1.0f, levelingAmount0to1);
    double dbSum = 0.0;
    int voicedCount = 0;
    const bool anySelected = (getSelectedNoteCount() > 1);

    for (const auto& n : notes)
    {
        if (n.isSibilant || n.meanRms <= 0.0005f)
            continue;
        if (anySelected && !(n.selected || n.id == selectedNoteId))
            continue;
        dbSum += static_cast<double>(juce::Decibels::gainToDecibels(n.meanRms, -60.0f));
        ++voicedCount;
    }

    if (voicedCount <= 0)
        return;

    pushUndoSnapshot();
    const float targetMeanDb = static_cast<float>(dbSum / static_cast<double>(voicedCount));

    for (auto& n : notes)
    {
        if (n.isSibilant || n.meanRms <= 0.0005f)
            continue;
        if (anySelected && !(n.selected || n.id == selectedNoteId))
            continue;
        const float noteDb = juce::Decibels::gainToDecibels(n.meanRms, -60.0f);
        const float deltaDb = (targetMeanDb - noteDb) * amt;
        n.gainDb     = juce::jlimit(-12.0f, 9.0f, deltaDb);
        n.userEdited = true;
    }

    currentCoachHint = "Levelled " + juce::String(voicedCount)
                     + " notes toward the phrase average (" + juce::String(targetMeanDb, 1) + " dB).";
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::paintFreehandStrokeBetween(juce::Point<float> p0, juce::Point<float> p1, juce::Rectangle<float> grid)
{
    if (timeline.empty())
        return;

    const int h0 = juce::jlimit(0, static_cast<int>(timeline.size()) - 1, static_cast<int>(std::lround(xToHop(p0.x, grid))));
    const int h1 = juce::jlimit(0, static_cast<int>(timeline.size()) - 1, static_cast<int>(std::lround(xToHop(p1.x, grid))));
    const float m0 = juce::jlimit(12.0f, 108.0f, yToMidiNote(p0.y));
    const float m1 = juce::jlimit(12.0f, 108.0f, yToMidiNote(p1.y));

    const int lo = juce::jmin(h0, h1);
    const int hi = juce::jmax(h0, h1);

    for (int h = lo; h <= hi; ++h)
    {
        const float t = (hi > lo) ? static_cast<float>(h - h0) / static_cast<float>(h1 - h0) : 1.0f;
        const float interpM = m0 + juce::jlimit(0.0f, 1.0f, std::abs(t)) * (m1 - m0);
        auto& th = timeline[(size_t)h];
        if (th.data.isVoiced)
        {
            th.hasCustomPitch = true;
            th.customMidi     = interpM;
            if (auto* nb = findNoteById(th.noteId))
            {
                nb->userEdited = true;
                selectedNoteId = nb->id;
            }
        }
    }

    // 3-point Gaussian smoothing across adjacent custom-drawn hops so hand-drawn curves never stair-step:
    const int smoothLo = juce::jmax(1, lo - 1);
    const int smoothHi = juce::jmin(static_cast<int>(timeline.size()) - 2, hi + 1);
    for (int h = smoothLo; h <= smoothHi; ++h)
    {
        if (timeline[(size_t)h].hasCustomPitch && timeline[(size_t)(h - 1)].hasCustomPitch && timeline[(size_t)(h + 1)].hasCustomPitch)
        {
            timeline[(size_t)h].customMidi = 0.25f * timeline[(size_t)(h - 1)].customMidi
                                           + 0.50f * timeline[(size_t)h].customMidi
                                           + 0.25f * timeline[(size_t)(h + 1)].customMidi;
        }
    }
}

void PitchTraceComponent::loadPhrasePreview()
{
    timeline.clear();
    notes.clear();
    undoStack.clear();
    redoStack.clear();
    selectedNoteId = -1;
    advancedEditNoteId = -1;
    clearedPreviewOnFirstLiveVoice = false;

    struct PhraseSegment
    {
        int   hops;
        float baseMidi;
        float scoopSemitones;
        float vibDepth;
        float rms;
        bool  voiced;
        bool  sibilant;
    };

    static constexpr PhraseSegment segments[] = {
        {  6,  0.0f,  0.0f, 0.00f, 0.02f, false, true  }, // initial "s" consonant
        { 26, 60.2f, -0.6f, 0.18f, 0.42f, true,  false }, // C4 (slightly sharp + scoop + vibrato)
        {  4,  0.0f,  0.0f, 0.00f, 0.03f, false, true  }, // "t" consonant
        { 24, 61.8f, -0.4f, 0.14f, 0.50f, true,  false }, // D4 (slightly flat)
        { 28, 64.3f,  0.3f, 0.22f, 0.62f, true,  false }, // E4 (expressive vibrato)
        {  5,  0.0f,  0.0f, 0.00f, 0.02f, false, true  }, // "sh" sibilant breath
        { 34, 66.7f, -0.8f, 0.28f, 0.72f, true,  false }, // G4 (hero sustained high note)
        { 30, 64.1f,  0.5f, 0.20f, 0.48f, true,  false }, // E4 resolution
        {  8,  0.0f,  0.0f, 0.00f, 0.00f, false, false }  // tail silence
    };

    double tSec = 0.0;
    constexpr double hopDt = 512.0 / 48000.0;
    int hopIdx = 0;

    for (const auto& seg : segments)
    {
        for (int i = 0; i < seg.hops; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(juce::jmax(1, seg.hops - 1));
            PitchTraceQueue::PitchData pd {};
            pd.hopIndex = hopIdx++;
            pd.isVoiced = seg.voiced;
            pd.isSibilant = seg.sibilant;

            const float envShape = std::sin(juce::MathConstants<float>::pi * juce::jlimit(0.05f, 0.95f, u));
            pd.rms = seg.rms * (0.65f + 0.35f * envShape);

            if (seg.voiced)
            {
                const float scoop = seg.scoopSemitones * std::exp(-6.0f * u);
                const float vibEnv = juce::jlimit(0.0f, 1.0f, (u - 0.2f) / 0.5f);
                const float vib = seg.vibDepth * vibEnv * std::sin(2.0f * juce::MathConstants<float>::pi * 5.6f * static_cast<float>(tSec));
                const float rawMidi = seg.baseMidi + scoop + vib;
                pd.uncorrectedPitchHz = midiNoteToHz(rawMidi);
                pd.correctedPitchHz = midiNoteToHz(std::round(rawMidi));
                pd.clarity = 0.88f;
            }

            TimelineHop th {};
            th.data = pd;
            th.timeStampSeconds = tSec;
            th.rawMidi = seg.voiced ? hzToMidiNote(pd.uncorrectedPitchHz) : 0.0f;
            timeline.push_back(th);
            tSec += hopDt;
        }
    }

    rebuildSegmentationFromSensitivity();

    // Select the first voiced note so the Quick Inspector bar shows an active note right away
    for (const auto& n : notes)
    {
        if (!n.isSibilant)
        {
            selectedNoteId = n.id;
            break;
        }
    }

    viewStartHop = 0.0f;
    visibleHopsCount = static_cast<float>(std::max<size_t>(120, timeline.size() + 12));
    viewCentreMidiNote = 63.0f;
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::loadPolyphonicChordPreview()
{
    timeline.clear();
    notes.clear();
    undoStack.clear();
    redoStack.clear();
    selectedNoteId = -1;
    advancedEditNoteId = -1;
    clearedPreviewOnFirstLiveVoice = false;

    // Multi-Voice Choir & String Ensemble Progression (5 to 6 simultaneous notes per chord — no 4-note limit!):
    // Bar 1: Cmaj9   (5 voices: C3=48, G3=55, B3=59, E4=64, D5=74)
    // Bar 2: Am11    (6 voices: A2=45, E3=52, G3=55, C4=60, E4=64, B4=71)
    // Bar 3: Fmaj9   (5 voices: F3=53, C4=60, E4=64, A4=69, G5=79)
    // Bar 4: G13sus4 (6 voices: G3=55, D4=62, F4=65, A4=69, C5=72, E5=76)
    // Bar 5: C6/9    (6 voices: C3=48, G3=55, E4=64, A4=69, D5=74, G5=79)
    struct PolyChordSegment
    {
        int   hops;
        int   numVoices;
        float baseMidi[6];
        float detuneCents[6];
        float vibDepth[6];
    };

    static constexpr PolyChordSegment chords[] = {
        { 32, 5, { 48.0f, 55.0f, 59.0f, 64.0f, 74.0f,  0.0f }, { +16.0f, -14.0f, +21.0f, -18.0f, +15.0f,  0.0f }, { 0.09f, 0.12f, 0.15f, 0.18f, 0.22f, 0.00f } },
        {  3, 0, {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, {   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,  0.0f }, { 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f } },
        { 32, 6, { 45.0f, 52.0f, 55.0f, 60.0f, 64.0f, 71.0f }, { -15.0f, +18.0f, -19.0f, +14.0f, -22.0f, +16.0f }, { 0.09f, 0.11f, 0.14f, 0.17f, 0.20f, 0.23f } },
        {  3, 0, {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, {   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,  0.0f }, { 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f } },
        { 32, 5, { 53.0f, 60.0f, 64.0f, 69.0f, 79.0f,  0.0f }, { +12.0f, -17.0f, +16.0f, -23.0f, +19.0f,  0.0f }, { 0.10f, 0.13f, 0.16f, 0.19f, 0.24f, 0.00f } },
        {  3, 0, {  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, {   0.0f,   0.0f,   0.0f,   0.0f,   0.0f,  0.0f }, { 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f } },
        { 34, 6, { 55.0f, 62.0f, 65.0f, 69.0f, 72.0f, 76.0f }, { -20.0f, +22.0f, -14.0f, +18.0f, -16.0f, +21.0f }, { 0.10f, 0.13f, 0.16f, 0.18f, 0.21f, 0.24f } },
        { 36, 6, { 48.0f, 55.0f, 64.0f, 69.0f, 74.0f, 79.0f }, { +10.0f, -12.0f, +15.0f, -18.0f, +14.0f, -16.0f }, { 0.10f, 0.12f, 0.15f, 0.18f, 0.20f, 0.23f } }
    };

    double tSec = 0.0;
    constexpr double hopDt = 512.0 / 48000.0;
    int hopIdx = 0;

    for (const auto& ch : chords)
    {
        const bool voiced = (ch.numVoices > 0);
        for (int i = 0; i < ch.hops; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(juce::jmax(1, ch.hops - 1));
            PitchTraceQueue::PitchData pd {};
            pd.hopIndex     = hopIdx++;
            pd.isVoiced     = voiced;
            pd.isSibilant   = !voiced;
            pd.isPolyphonic = true;

            const float envShape = std::sin(juce::MathConstants<float>::pi * juce::jlimit(0.05f, 0.95f, u));
            pd.rms = voiced ? (0.60f * (0.68f + 0.32f * envShape)) : 0.02f;

            if (voiced)
            {
                pd.numPolyVoices = static_cast<uint8_t>(ch.numVoices);
                for (int v = 0; v < ch.numVoices; ++v)
                {
                    const float scoop = (v % 2 == 0 ? -0.26f : 0.20f) * std::exp(-6.5f * u);
                    const float vibEnv = juce::jlimit(0.0f, 1.0f, (u - 0.15f) / 0.55f);
                    const float vibRate = 5.0f + 0.30f * static_cast<float>(v);
                    const float vib = ch.vibDepth[v] * vibEnv * std::sin(2.0f * juce::MathConstants<float>::pi * vibRate * static_cast<float>(tSec) + 0.7f * v);
                    const float rawM = ch.baseMidi[v] + 0.01f * ch.detuneCents[v] + scoop + vib;
                    pd.polyRawMidi[(size_t)v]       = rawM;
                    pd.polyCorrectedMidi[(size_t)v] = std::round(ch.baseMidi[v]);
                    pd.polyVoiceRms[(size_t)v]      = pd.rms / static_cast<float>(ch.numVoices);
                }
                pd.uncorrectedPitchHz = midiNoteToHz(pd.polyRawMidi[(size_t)(ch.numVoices - 1)]);
                pd.correctedPitchHz   = midiNoteToHz(pd.polyCorrectedMidi[(size_t)(ch.numVoices - 1)]);
                pd.clarity            = 0.88f;
            }

            TimelineHop th {};
            th.data = pd;
            th.timeStampSeconds = tSec;
            th.rawMidi = voiced ? pd.polyRawMidi[(size_t)(ch.numVoices - 1)] : 0.0f;
            th.polyNoteIds.fill(-1);
            timeline.push_back(th);
            tSec += hopDt;
        }
    }

    rebuildSegmentationFromSensitivity();

    for (const auto& n : notes)
    {
        if (!n.isSibilant)
        {
            selectedNoteId = n.id;
            break;
        }
    }

    viewStartHop = 0.0f;
    visibleHopsCount = static_cast<float>(std::max<size_t>(140, timeline.size() + 12));
    viewCentreMidiNote = 63.0f;
    viewHalfRangeSemitones = 18.0f;
    notifySelectionChanged();
    repaint();
}

//==============================================================================
// Segmentation Driven by Global SENSITIVITY Knob
//==============================================================================
void PitchTraceComponent::rebuildSegmentationFromSensitivity()
{
    if (isClipMode())
        return; // clip notes come from the clip analysis, not from live segmentation

    if (timeline.empty())
    {
        notes.clear();
        return;
    }

    std::vector<NewtoneNoteBlock> savedUserEdits;
    for (const auto& n : notes)
        if (n.userEdited)
            savedUserEdits.push_back(n);

    notes.clear();

    const float sensitivity = juce::jlimit(0.0f, 1.0f, queue.globalSensitivity.load(std::memory_order_relaxed));
    const uint16_t scaleMask = queue.activeScaleMask.load(std::memory_order_relaxed);

    const float splitSemitones = juce::jmap(sensitivity, 0.0f, 1.0f, 1.45f, 0.38f);
    const int   minNoteHops    = juce::roundToInt(juce::jmap(sensitivity, 0.0f, 1.0f, 8.0f, 2.0f));

    const int numHops = static_cast<int>(timeline.size());
    int i = 0;

    while (i < numHops)
    {
        auto& h0 = timeline[(size_t)i];

        if (!h0.data.isVoiced)
        {
            if (h0.data.isSibilant)
            {
                int j = i + 1;
                float rmsSum = h0.data.rms;
                while (j < numHops && !timeline[(size_t)j].data.isVoiced && timeline[(size_t)j].data.isSibilant)
                {
                    rmsSum += timeline[(size_t)j].data.rms;
                    ++j;
                }

                NewtoneNoteBlock sib {};
                sib.id = nextNoteId++;
                sib.startHop = i;
                sib.endHop = j;
                sib.detectedMidi = viewCentreMidiNote;
                sib.nearestMidi = static_cast<int>(std::lround(viewCentreMidiNote));
                sib.targetMidi = sib.nearestMidi;
                sib.meanRms = rmsSum / static_cast<float>(j - i);
                sib.isSibilant = true;

                for (int k = i; k < j; ++k)
                    timeline[(size_t)k].noteId = sib.id;

                notes.push_back(sib);
                i = j;
                continue;
            }

            h0.noteId = -1;
            ++i;
            continue;
        }

        int j = i + 1;
        const double c0 = juce::jlimit(0.25f, 1.0f, h0.data.clarity);
        const double w0 = static_cast<double>(juce::jmax(0.001f, h0.data.rms)) * c0 * c0;
        double weightedMidiSum = static_cast<double>(h0.rawMidi) * w0;
        double weightSum       = w0;
        float  rmsSum          = h0.data.rms;

        while (j < numHops && timeline[(size_t)j].data.isVoiced && timeline[(size_t)j].rawMidi > 0.0f)
        {
            const float curMean = static_cast<float>(weightedMidiSum / weightSum);
            const float candMidi = timeline[(size_t)j].rawMidi;
            const float diff = std::abs(candMidi - curMean);

            if (j - i >= minNoteHops && diff >= splitSemitones)
            {
                // Fix #6: Onset + Pitch-Derivative (Glissando-Aware) Segmentation:
                // Distinguish true phonetic re-articulations (RMS dip) or settled pitch plateaus
                // from active mid-glissando pitch slides so vocal slurs aren't chopped into fake stair-steps.
                const float curMeanRms = rmsSum / static_cast<float>(juce::jmax(1, j - i));
                const bool hasOnsetDip = (timeline[(size_t)j].data.rms < curMeanRms * 0.58f);

                int plateauCount = 0;
                int settledCount = 0;
                const int lookEnd = juce::jmin(numHops, j + minNoteHops + 1);
                for (int look = j; look < lookEnd; ++look)
                {
                    if (timeline[(size_t)look].data.isVoiced
                        && std::abs(timeline[(size_t)look].rawMidi - curMean) >= splitSemitones * 0.85f)
                    {
                        ++plateauCount;
                        if (std::abs(timeline[(size_t)look].rawMidi - candMidi) < 0.42f)
                            ++settledCount;
                    }
                }
                if (plateauCount >= juce::jmax(1, minNoteHops - 1)
                    && (hasOnsetDip || settledCount >= juce::jmax(1, minNoteHops - 1) || diff >= splitSemitones * 1.65f))
                    break;
            }

            const double cj = juce::jlimit(0.25f, 1.0f, timeline[(size_t)j].data.clarity);
            const double w  = static_cast<double>(juce::jmax(0.001f, timeline[(size_t)j].data.rms)) * cj * cj;
            weightedMidiSum += static_cast<double>(candMidi) * w;
            weightSum += w;
            rmsSum += timeline[(size_t)j].data.rms;
            ++j;
        }

        NewtoneNoteBlock nb {};
        nb.id           = nextNoteId++;
        nb.startHop     = i;
        nb.endHop       = j;
        nb.detectedMidi = static_cast<float>(weightedMidiSum / weightSum);
        nb.nearestMidi  = static_cast<int>(std::lround(nb.detectedMidi));
        nb.targetMidi   = snapToScaleEnabled ? snapMidiToScale(nb.detectedMidi, scaleMask) : nb.nearestMidi;
        nb.meanRms      = rmsSum / static_cast<float>(j - i);
        nb.isSibilant   = false;

        for (const auto& saved : savedUserEdits)
        {
            const int overlapStart = juce::jmax(nb.startHop, saved.startHop);
            const int overlapEnd   = juce::jmin(nb.endHop, saved.endHop);
            if (overlapEnd > overlapStart)
            {
                nb.targetMidi       = saved.targetMidi;
                nb.fineCents        = saved.fineCents;
                nb.noteCenter       = saved.noteCenter;
                nb.noteVariation    = saved.noteVariation;
                nb.formantSemitones = saved.formantSemitones;
                nb.gainDb           = saved.gainDb;
                nb.sibilantDb       = saved.sibilantDb;
                nb.rampInRatio      = saved.rampInRatio;
                nb.rampOutRatio     = saved.rampOutRatio;
                nb.fadeInRatio      = saved.fadeInRatio;
                nb.fadeOutRatio     = saved.fadeOutRatio;
                nb.userEdited       = true;
                break;
            }
        }

        for (int k = i; k < j; ++k)
            timeline[(size_t)k].noteId = nb.id;

        notes.push_back(nb);
        i = j;
    }

    // If Polyphonic Mode (Melodyne Chord/Choir/String DNA) is active and timeline contains polyphonic voices,
    // use Pitch-Proximity Multi-Voice Stream Tracking across all detected voices (up to 16 simultaneous voices,
    // with no 4-note limit!):
    bool hasPolyData = false;
    if (polyphonicModeEnabled)
    {
        for (const auto& th : timeline)
            if (th.data.numPolyVoices > 1) { hasPolyData = true; break; }
    }

    if (polyphonicModeEnabled && hasPolyData)
    {
        notes.clear();
        for (auto& th : timeline)
        {
            th.noteId = -1;
            th.polyNoteIds.fill(-1);
        }

        struct ActiveTrack
        {
            NewtoneNoteBlock block;
            double weightedMidiSum = 0.0;
            double weightTotal     = 0.0;
            float  rmsSum          = 0.0f;
            int    lastVoiceSlot   = 0;
            int    missHops        = 0;
        };

        std::vector<ActiveTrack> activeTracks;
        activeTracks.reserve(PitchTraceQueue::maxPolyVoices);

        auto finalizeTrack = [&](ActiveTrack& tr)
        {
            const int hopLen = juce::jmax(1, tr.block.endHop - tr.block.startHop);
            const int minPolyHops = juce::jmax(7, minNoteHops + 3);
            if (hopLen < minPolyHops)
                return;

            tr.block.detectedMidi = static_cast<float>(tr.weightedMidiSum / juce::jmax(1.0e-6, tr.weightTotal));
            tr.block.nearestMidi  = static_cast<int>(std::lround(tr.block.detectedMidi));
            tr.block.targetMidi   = snapToScaleEnabled ? snapMidiToScale(tr.block.detectedMidi, scaleMask) : tr.block.nearestMidi;
            tr.block.meanRms      = tr.rmsSum / static_cast<float>(hopLen);

            for (const auto& saved : savedUserEdits)
            {
                const int overlapStart = juce::jmax(tr.block.startHop, saved.startHop);
                const int overlapEnd   = juce::jmin(tr.block.endHop, saved.endHop);
                if (overlapEnd > overlapStart && std::abs(saved.detectedMidi - tr.block.detectedMidi) < 1.2f)
                {
                    tr.block.targetMidi       = saved.targetMidi;
                    tr.block.fineCents        = saved.fineCents;
                    tr.block.noteCenter       = saved.noteCenter;
                    tr.block.noteVariation    = saved.noteVariation;
                    tr.block.formantSemitones = saved.formantSemitones;
                    tr.block.gainDb           = saved.gainDb;
                    tr.block.rampInRatio      = saved.rampInRatio;
                    tr.block.rampOutRatio     = saved.rampOutRatio;
                    tr.block.userEdited       = true;
                    break;
                }
            }

            notes.push_back(tr.block);
        };

        for (int h = 0; h < numHops; ++h)
        {
            auto& th = timeline[(size_t)h];
            const int nVoices = th.data.isVoiced
                ? juce::jlimit(0, PitchTraceQueue::maxPolyVoices, static_cast<int>(th.data.numPolyVoices))
                : 0;

            std::array<bool, PitchTraceQueue::maxPolyVoices> voiceMatched {};
            std::vector<bool> trackExtended(activeTracks.size(), false);

            // Match each active track to the closest detected voice at hop `h` within `splitSemitones`
            for (size_t t = 0; t < activeTracks.size(); ++t)
            {
                const float curMean = static_cast<float>(activeTracks[t].weightedMidiSum / juce::jmax(1.0e-6, activeTracks[t].weightTotal));
                int bestV = -1;
                float bestDiff = splitSemitones;

                for (int v = 0; v < nVoices; ++v)
                {
                    if (voiceMatched[(size_t)v] || th.data.polyRawMidi[(size_t)v] <= 0.0f)
                        continue;
                    const float d = std::abs(th.data.polyRawMidi[(size_t)v] - curMean);
                    if (d < bestDiff)
                    {
                        bestDiff = d;
                        bestV = v;
                    }
                }

                if (bestV >= 0)
                {
                    voiceMatched[(size_t)bestV] = true;
                    trackExtended[t] = true;
                    const float m = th.data.polyRawMidi[(size_t)bestV];
                    const float w = juce::jmax(0.001f, th.data.polyVoiceRms[(size_t)bestV]);
                    activeTracks[t].weightedMidiSum += static_cast<double>(m) * w;
                    activeTracks[t].weightTotal     += w;
                    activeTracks[t].rmsSum          += w;
                    activeTracks[t].block.endHop     = h + 1;
                    activeTracks[t].lastVoiceSlot    = bestV;
                    th.polyNoteIds[(size_t)bestV]    = activeTracks[t].block.id;
                    if (bestV == 0)
                        th.noteId = activeTracks[t].block.id;
                }
            }

            // Finalize any tracks that ended before hop `h` (with multi-hop gap bridging in MIDI Clean Mode!)
            const int maxGapHops = midiCleanModeEnabled ? 5 : 0;
            for (int t = static_cast<int>(activeTracks.size()) - 1; t >= 0; --t)
            {
                if (trackExtended[(size_t)t])
                {
                    activeTracks[(size_t)t].missHops = 0;
                }
                else
                {
                    ++activeTracks[(size_t)t].missHops;
                    if (activeTracks[(size_t)t].missHops > maxGapHops)
                    {
                        finalizeTrack(activeTracks[(size_t)t]);
                        activeTracks.erase(activeTracks.begin() + t);
                    }
                }
            }

            // Spawn new note blocks for any newly started chord voices at hop `h`
            for (int v = 0; v < nVoices; ++v)
            {
                if (voiceMatched[(size_t)v] || th.data.polyRawMidi[(size_t)v] <= 0.0f)
                    continue;

                const float m = th.data.polyRawMidi[(size_t)v];
                const float w = juce::jmax(0.001f, th.data.polyVoiceRms[(size_t)v]);

                ActiveTrack tr {};
                tr.block.id           = nextNoteId++;
                tr.block.voiceLane    = v;
                tr.block.startHop     = h;
                tr.block.endHop       = h + 1;
                tr.block.detectedMidi = m;
                tr.block.nearestMidi  = static_cast<int>(std::lround(m));
                tr.block.targetMidi   = snapToScaleEnabled ? snapMidiToScale(m, scaleMask) : tr.block.nearestMidi;
                tr.block.meanRms      = w;
                tr.block.isSibilant   = false;
                tr.weightedMidiSum    = static_cast<double>(m) * w;
                tr.weightTotal        = w;
                tr.rmsSum             = w;
                tr.lastVoiceSlot      = v;

                th.polyNoteIds[(size_t)v] = tr.block.id;
                if (v == 0)
                    th.noteId = tr.block.id;

                activeTracks.push_back(tr);
            }
        }

        for (auto& tr : activeTracks)
            finalizeTrack(tr);
    }

    // =========================================================================
    // Post-Segmentation Instrument MIDI Clean & Denoise Pass:
    //   1. Prunes isolated pick/fret/drum micro-blips (< 10 hops or low RMS)
    //   2. Stitches adjacent same-pitch note blocks separated by short <= 6-hop
    //      dropouts so sustained guitar/piano strings form single clean MIDI notes
    //   3. Quantizes note start/end boundaries to a clean sub-beat grid
    // =========================================================================
    if (midiCleanModeEnabled && !notes.empty())
    {
        float maxNoteRms = 0.001f;
        for (const auto& n : notes)
            if (!n.isSibilant)
                maxNoteRms = juce::jmax(maxNoteRms, n.meanRms);

        const int minCleanHops = juce::jmax(10, minNoteHops * 2 + 2);
        const float minCleanRms = maxNoteRms * 0.085f;

        // 1. Remove short transient blips and low-energy noise fragments
        notes.erase(std::remove_if(notes.begin(), notes.end(),
            [&](const NewtoneNoteBlock& n)
            {
                if (n.userEdited) return false;
                if (n.isSibilant) return true; // Strip unpitched breath/noise blocks in MIDI Clean Mode
                const int len = n.endHop - n.startHop;
                return (len < minCleanHops) || (n.meanRms < minCleanRms);
            }), notes.end());

        // 2. Sort by (targetMidi, startHop) to stitch fragmented same-pitch notes across <= 7-hop gaps
        std::sort(notes.begin(), notes.end(),
            [](const NewtoneNoteBlock& a, const NewtoneNoteBlock& b)
            {
                if (a.targetMidi != b.targetMidi) return a.targetMidi < b.targetMidi;
                return a.startHop < b.startHop;
            });

        std::vector<NewtoneNoteBlock> stitched;
        stitched.reserve(notes.size());
        for (const auto& n : notes)
        {
            if (!stitched.empty()
                && stitched.back().targetMidi == n.targetMidi
                && (n.startHop - stitched.back().endHop) <= 7)
            {
                auto& prev = stitched.back();
                const float w1 = static_cast<float>(juce::jmax(1, prev.endHop - prev.startHop));
                const float w2 = static_cast<float>(juce::jmax(1, n.endHop - n.startHop));
                prev.detectedMidi = (prev.detectedMidi * w1 + n.detectedMidi * w2) / (w1 + w2);
                prev.meanRms      = (prev.meanRms * w1 + n.meanRms * w2) / (w1 + w2);
                prev.endHop       = juce::jmax(prev.endHop, n.endHop);
            }
            else
            {
                stitched.push_back(n);
            }
        }

        // 2b. Remove any remaining isolated fragments shorter than minStitchedHops after stitching
        const int minStitchedHops = juce::jmax(14, minNoteHops * 3);
        stitched.erase(std::remove_if(stitched.begin(), stitched.end(),
            [&](const NewtoneNoteBlock& n) { return !n.userEdited && (n.endHop - n.startHop) < minStitchedHops; }),
            stitched.end());

        // 3. Sort back chronologically by startHop and relink timeline
        std::sort(stitched.begin(), stitched.end(),
            [](const NewtoneNoteBlock& a, const NewtoneNoteBlock& b)
            {
                if (a.startHop != b.startHop) return a.startHop < b.startHop;
                return a.targetMidi < b.targetMidi;
            });

        notes = std::move(stitched);
        relinkTimelineToNotes();
    }

    lastAppliedSensitivity = sensitivity;
    lastAppliedScaleMask   = scaleMask;
}

void PitchTraceComponent::appendHopToTimeline(const PitchTraceQueue::PitchData& hopData, double timeSec)
{
    if (!clearedPreviewOnFirstLiveVoice && hopData.isVoiced && hopData.hopIndex > 0)
    {
        clearedPreviewOnFirstLiveVoice = true;
        timeline.clear();
        notes.clear();
        viewStartHop = 0.0f;
    }

    if (static_cast<int>(timeline.size()) >= maxTimelineHops)
    {
        const int dropCount = 256;
        timeline.erase(timeline.begin(), timeline.begin() + dropCount);
        for (auto& n : notes)
        {
            n.startHop = juce::jmax(0, n.startHop - dropCount); // a note straddling the cut must not go negative
            n.endHop  -= dropCount;
        }
        notes.erase(std::remove_if(notes.begin(), notes.end(),
            [](const NewtoneNoteBlock& n) { return n.endHop <= 0; }), notes.end());
        viewStartHop = juce::jmax(0.0f, viewStartHop - static_cast<float>(dropCount));
    }

    const int hopIdx = static_cast<int>(timeline.size());
    TimelineHop th {};
    th.data = hopData;
    th.timeStampSeconds = timeSec;
    th.rawMidi = (hopData.isVoiced && hopData.uncorrectedPitchHz > 0.0f)
        ? hzToMidiNote(hopData.uncorrectedPitchHz) : 0.0f;

    const float sensitivity = juce::jlimit(0.0f, 1.0f, queue.globalSensitivity.load(std::memory_order_relaxed));
    const uint16_t scaleMask = queue.activeScaleMask.load(std::memory_order_relaxed);
    const float splitSemitones = juce::jmap(sensitivity, 0.0f, 1.0f, 1.45f, 0.38f);

    th.polyNoteIds.fill(-1);

    if (polyphonicModeEnabled && hopData.isVoiced && hopData.numPolyVoices > 0)
    {
        const int nVoices = juce::jlimit(0, PitchTraceQueue::maxPolyVoices, static_cast<int>(hopData.numPolyVoices));
        for (int v = 0; v < nVoices; ++v)
        {
            const float vm = hopData.polyRawMidi[(size_t)v];
            if (vm <= 0.0f) continue;

            // Find an active note ending at `hopIdx` within `splitSemitones`
            NewtoneNoteBlock* matchedNote = nullptr;
            float bestDiff = splitSemitones;
            for (int ni = static_cast<int>(notes.size()) - 1; ni >= 0; --ni)
            {
                auto& cand = notes[(size_t)ni];
                if (cand.endHop < hopIdx) break;
                if (cand.isSibilant || cand.endHop != hopIdx) continue;
                const float d = std::abs(vm - cand.detectedMidi);
                if (d < bestDiff)
                {
                    bestDiff = d;
                    matchedNote = &cand;
                }
            }

            if (matchedNote != nullptr)
            {
                const float prevCount = static_cast<float>(juce::jmax(1, matchedNote->endHop - matchedNote->startHop));
                matchedNote->detectedMidi = (matchedNote->detectedMidi * prevCount + vm) / (prevCount + 1.0f);
                matchedNote->nearestMidi  = static_cast<int>(std::lround(matchedNote->detectedMidi));
                if (!matchedNote->userEdited)
                    matchedNote->targetMidi = snapToScaleEnabled ? snapMidiToScale(matchedNote->detectedMidi, scaleMask) : matchedNote->nearestMidi;
                matchedNote->meanRms = (matchedNote->meanRms * prevCount + hopData.polyVoiceRms[(size_t)v]) / (prevCount + 1.0f);
                matchedNote->endHop  = hopIdx + 1;
                th.polyNoteIds[(size_t)v] = matchedNote->id;
                if (v == 0) th.noteId = matchedNote->id;
            }
            else
            {
                NewtoneNoteBlock nb {};
                nb.id           = nextNoteId++;
                nb.voiceLane    = v;
                nb.startHop     = hopIdx;
                nb.endHop       = hopIdx + 1;
                nb.detectedMidi = vm;
                nb.nearestMidi  = static_cast<int>(std::lround(vm));
                nb.targetMidi   = snapToScaleEnabled ? snapMidiToScale(vm, scaleMask) : nb.nearestMidi;
                nb.meanRms      = hopData.polyVoiceRms[(size_t)v];
                nb.isSibilant   = false;
                th.polyNoteIds[(size_t)v] = nb.id;
                if (v == 0) th.noteId = nb.id;
                notes.push_back(nb);
            }
        }
    }
    else if (hopData.isVoiced && th.rawMidi > 0.0f)
    {
        if (!notes.empty() && !notes.back().isSibilant && notes.back().endHop == hopIdx
            && std::abs(th.rawMidi - notes.back().detectedMidi) < splitSemitones)
        {
            auto& cur = notes.back();
            const float prevCount = static_cast<float>(cur.endHop - cur.startHop);
            cur.detectedMidi = (cur.detectedMidi * prevCount + th.rawMidi) / (prevCount + 1.0f);
            cur.nearestMidi  = static_cast<int>(std::lround(cur.detectedMidi));
            if (!cur.userEdited)
                cur.targetMidi = snapToScaleEnabled ? snapMidiToScale(cur.detectedMidi, scaleMask) : cur.nearestMidi;
            cur.meanRms = (cur.meanRms * prevCount + hopData.rms) / (prevCount + 1.0f);
            cur.endHop = hopIdx + 1;
            th.noteId = cur.id;
        }
        else
        {
            NewtoneNoteBlock nb {};
            nb.id           = nextNoteId++;
            nb.startHop     = hopIdx;
            nb.endHop       = hopIdx + 1;
            nb.detectedMidi = th.rawMidi;
            nb.nearestMidi  = static_cast<int>(std::lround(th.rawMidi));
            nb.targetMidi   = snapToScaleEnabled ? snapMidiToScale(th.rawMidi, scaleMask) : nb.nearestMidi;
            nb.meanRms      = hopData.rms;
            nb.isSibilant   = false;
            th.noteId       = nb.id;
            notes.push_back(nb);
        }
    }
    else if (hopData.isSibilant)
    {
        if (!notes.empty() && notes.back().isSibilant && notes.back().endHop == hopIdx)
        {
            notes.back().endHop = hopIdx + 1;
            th.noteId = notes.back().id;
        }
        else
        {
            NewtoneNoteBlock sib {};
            sib.id           = nextNoteId++;
            sib.startHop     = hopIdx;
            sib.endHop       = hopIdx + 1;
            sib.detectedMidi = viewCentreMidiNote;
            sib.nearestMidi  = static_cast<int>(std::lround(viewCentreMidiNote));
            sib.targetMidi   = sib.nearestMidi;
            sib.meanRms      = hopData.rms;
            sib.isSibilant   = true;
            th.noteId        = sib.id;
            notes.push_back(sib);
        }
    }

    timeline.push_back(th);
}

void PitchTraceComponent::syncActiveOverrideToEngine()
{
    if (isClipMode())
    {
        pushNotesToClipSession();
        return;
    }

    auto& ov = queue.getLiveOverride();
    const NewtoneNoteBlock* active = findNoteById(selectedNoteId);
    if (active == nullptr && !notes.empty())
        active = &notes.back();

    if (active != nullptr && !active->isSibilant && active->userEdited)
    {
        ov.noteId.store(active->id, std::memory_order_relaxed);
        ov.detectedMidiRef.store(active->detectedMidi, std::memory_order_relaxed);
        ov.targetMidiFloat.store(active->effectiveTargetMidi(), std::memory_order_relaxed);
        ov.noteCenter.store(active->noteCenter, std::memory_order_relaxed);
        ov.noteVariation.store(active->noteVariation, std::memory_order_relaxed);
        ov.formantSemitones.store(active->formantSemitones, std::memory_order_relaxed);
        ov.gainDb.store(active->gainDb, std::memory_order_relaxed);
        ov.sibilantDb.store(active->sibilantDb, std::memory_order_relaxed);
        ov.rampInRatio.store(active->rampInRatio, std::memory_order_relaxed);
        ov.rampOutRatio.store(active->rampOutRatio, std::memory_order_relaxed);

        bool customFound = false;
        float customMean = active->effectiveTargetMidi();
        int customCount = 0;
        double customSum = 0.0;
        for (int k = juce::jmax(0, active->startHop); k < juce::jmin(static_cast<int>(timeline.size()), active->endHop); ++k)
        {
            if (timeline[(size_t)k].hasCustomPitch)
            {
                customFound = true;
                customSum += timeline[(size_t)k].customMidi;
                ++customCount;
            }
        }
        if (customFound && customCount > 0)
            customMean = static_cast<float>(customSum / customCount);

        ov.fadeInRatio.store(active->fadeInRatio, std::memory_order_relaxed);
        ov.fadeOutRatio.store(active->fadeOutRatio, std::memory_order_relaxed);
        ov.fadeMultiplier.store(1.0f, std::memory_order_relaxed);
        ov.hasCustomPitch.store(customFound, std::memory_order_relaxed);
        ov.customPitchMidi.store(customMean, std::memory_order_relaxed);
        ov.active.store(true, std::memory_order_relaxed);
    }
    else
    {
        ov.active.store(false, std::memory_order_relaxed);
    }

    // Populate Hop-Indexed Timeline Override Table so every edited note in the phrase
    // applies its own pitch, formant, variation, gain, sibilant trim, custom curve,
    // and curved fade-in/out envelope during playback:
    queue.beginHopOverrideUpdate();
    for (const auto& n : notes)
    {
        if (n.isSibilant || !n.userEdited)
            continue;
        const float span = static_cast<float>(juce::jmax(1, n.endHop - n.startHop));
        for (int h = juce::jmax(0, n.startHop); h < juce::jmin(static_cast<int>(timeline.size()), n.endHop); ++h)
        {
            const float u = (static_cast<float>(h - n.startHop) + 0.5f) / span;
            float fadeEnv = 1.0f;
            if (n.fadeInRatio > 0.005f && u < n.fadeInRatio)
                fadeEnv *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (u / n.fadeInRatio));
            if (n.fadeOutRatio > 0.005f && (1.0f - u) < n.fadeOutRatio)
                fadeEnv *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * ((1.0f - u) / n.fadeOutRatio));

            PitchTraceQueue::HopOverrideSlot slot {};
            slot.active           = true;
            slot.targetMidi       = n.effectiveTargetMidi();
            slot.noteCenter       = n.noteCenter;
            slot.noteVariation    = n.noteVariation;
            slot.formantSemitones = n.formantSemitones;
            slot.gainDb           = n.gainDb;
            slot.sibilantDb       = n.sibilantDb;
            slot.rampInRatio      = n.rampInRatio;
            slot.rampOutRatio     = n.rampOutRatio;
            slot.fadeMultiplier   = fadeEnv;
            slot.hasCustomPitch   = timeline[(size_t)h].hasCustomPitch;
            slot.customPitchMidi  = timeline[(size_t)h].customMidi;
            queue.setHopOverride(h, slot);
        }
    }
    queue.commitHopOverrideUpdate();

    // Sync up to 4 active polyphonic chord voice overrides (for Melodyne-style per-note chord tuning):
    for (int v = 0; v < PitchTraceQueue::maxPolyVoices; ++v)
        queue.getPolyVoiceOverride(v).active.store(false, std::memory_order_relaxed);

    if (polyphonicModeEnabled)
    {
        int slot = 0;
        for (const auto& n : notes)
        {
            if (n.isSibilant || !n.userEdited)
                continue;
            if (slot < PitchTraceQueue::maxPolyVoices)
            {
                auto& pov = queue.getPolyVoiceOverride(slot);
                pov.noteId.store(n.id, std::memory_order_relaxed);
                pov.detectedMidiRef.store(n.detectedMidi, std::memory_order_relaxed);
                pov.targetMidiFloat.store(n.effectiveTargetMidi(), std::memory_order_relaxed);
                pov.noteCenter.store(n.noteCenter, std::memory_order_relaxed);
                pov.noteVariation.store(n.noteVariation, std::memory_order_relaxed);
                pov.formantSemitones.store(n.formantSemitones, std::memory_order_relaxed);
                pov.gainDb.store(n.gainDb, std::memory_order_relaxed);
                pov.active.store(true, std::memory_order_relaxed);
                ++slot;
            }
        }
    }
}

void PitchTraceComponent::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    queue.popAll(popScratch);

    bool needsRepaint = false;

    if (isClipMode())
    {
        popScratch.clear(); // live hops don't belong on the clip's timeline
        const auto pos = clipSession->getPlayPosition();
        const int hop = pos >= 0 ? static_cast<int>(pos / juce::jmax(1, clipHopSize)) : (clipSession->isPreviewPlaying() ? clipPlayHop : clipPlayHop);
        if (pos >= 0 && hop != clipPlayHop)
        {
            clipPlayHop = hop;
            if (autoScrollEnabled && (static_cast<float>(hop) > viewStartHop + visibleHopsCount * 0.88f || static_cast<float>(hop) < viewStartHop))
                viewStartHop = juce::jmax(0.0f, static_cast<float>(hop) - visibleHopsCount * 0.12f);
            needsRepaint = true;
        }
        const auto st = clipSession->getStatus();
        if (st == Poly::ClipSession::Status::recording || st == Poly::ClipSession::Status::analysing)
        {
            const auto text = clipSession->getStatusText();
            if (text != currentCoachHint) { currentCoachHint = text; needsRepaint = true; }
        }
        if (needsRepaint) repaint();
        return;
    }

    if (loadedAudioClip.getNumSamples() > 0)
    {
        // A loaded file owns the timeline: live hops (incl. the preview's own) would append to it and
        // push the file off the start. Clear the roll to go back to live capture.
        popScratch.clear();
        const int hop = queue.currentPlayheadHop.load(std::memory_order_relaxed);
        if (hop != lastFilePlayHop)
        {
            lastFilePlayHop = hop;
            if (autoScrollEnabled && (static_cast<float>(hop) > viewStartHop + visibleHopsCount * 0.88f || static_cast<float>(hop) < viewStartHop))
                viewStartHop = juce::jmax(0.0f, static_cast<float>(hop) - visibleHopsCount * 0.12f);
            needsRepaint = true;
        }
    }

    if (!popScratch.empty())
    {
        latestPitch = popScratch.back();

        const double intervalStart = hasLastPopTime ? lastPopTimeSeconds : now;
        const double step = popScratch.size() > 1
            ? (now - intervalStart) / (double)(popScratch.size() - 1)
            : 0.0;
        double t = intervalStart;

        for (const auto& data : popScratch)
        {
            if (data.isVoiced || data.isSibilant || (!timeline.empty() && timeline.back().data.hopIndex > 0))
            {
                appendHopToTimeline(data, t);
                needsRepaint = true;
            }
            t += step;
        }

        if (autoScrollEnabled && static_cast<float>(timeline.size()) > viewStartHop + visibleHopsCount * 0.92f)
            viewStartHop = juce::jmax(0.0f, static_cast<float>(timeline.size()) - visibleHopsCount * 0.85f);
    }

    lastPopTimeSeconds = now;
    hasLastPopTime = true;

    const float curSens = queue.globalSensitivity.load(std::memory_order_relaxed);
    const uint16_t curMask = queue.activeScaleMask.load(std::memory_order_relaxed);
    if (std::abs(curSens - lastAppliedSensitivity) > 0.01f || curMask != lastAppliedScaleMask)
    {
        rebuildSegmentationFromSensitivity();
        notifySelectionChanged();
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

//==============================================================================
// Coordinate Mapping & Pitch Curve Evaluation
//==============================================================================
juce::Rectangle<float> PitchTraceComponent::getPianoRulerArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    b.removeFromTop(timeRulerHeight);
    b.removeFromBottom(coachBarHeight);
    return b.removeFromLeft(pianoRulerWidth);
}

juce::Rectangle<float> PitchTraceComponent::getTimeRulerArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    auto top = b.removeFromTop(timeRulerHeight);
    top.removeFromLeft(pianoRulerWidth);
    return top;
}

juce::Rectangle<float> PitchTraceComponent::getCoachBarArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    return b.removeFromBottom(coachBarHeight);
}

juce::Rectangle<float> PitchTraceComponent::getGridArea() const noexcept
{
    auto b = getLocalBounds().toFloat();
    b.removeFromTop(timeRulerHeight);
    b.removeFromBottom(coachBarHeight);
    b.removeFromLeft(pianoRulerWidth);
    return b;
}

float PitchTraceComponent::midiNoteToY(float midiNote) const noexcept
{
    const auto grid = getGridArea();
    const float lowest  = viewCentreMidiNote - viewHalfRangeSemitones;
    const float highest = viewCentreMidiNote + viewHalfRangeSemitones;
    const float t = (midiNote - lowest) / juce::jmax(1.0f, highest - lowest);
    return grid.getBottom() - t * grid.getHeight();
}

float PitchTraceComponent::yToMidiNote(float y) const noexcept
{
    const auto grid = getGridArea();
    const float lowest  = viewCentreMidiNote - viewHalfRangeSemitones;
    const float highest = viewCentreMidiNote + viewHalfRangeSemitones;
    const float t = (grid.getBottom() - y) / juce::jmax(1.0f, grid.getHeight());
    return lowest + t * (highest - lowest);
}

float PitchTraceComponent::hopToX(float hopIndex, juce::Rectangle<float> grid) const noexcept
{
    return grid.getX() + ((hopIndex - viewStartHop) / juce::jmax(8.0f, visibleHopsCount)) * grid.getWidth();
}

float PitchTraceComponent::xToHop(float x, juce::Rectangle<float> grid) const noexcept
{
    const float norm = (x - grid.getX()) / juce::jmax(1.0f, grid.getWidth());
    return viewStartHop + norm * juce::jmax(8.0f, visibleHopsCount);
}

juce::Rectangle<float> PitchTraceComponent::getNoteRect(const NewtoneNoteBlock& note, juce::Rectangle<float> grid) const noexcept
{
    const float x1 = hopToX(static_cast<float>(note.startHop + note.warpOffsetHops), grid);
    const float x2 = hopToX(static_cast<float>(note.endHop + note.warpOffsetHops), grid);
    const float pitchCenter = note.isSibilant ? note.detectedMidi : note.effectiveTargetMidi();
    const float yTop = midiNoteToY(pitchCenter + 0.46f);
    const float yBot = midiNoteToY(pitchCenter - 0.46f);
    return juce::Rectangle<float>(juce::jmin(x1, x2), juce::jmin(yTop, yBot),
                                  juce::jmax(4.0f, std::abs(x2 - x1)), juce::jmax(6.0f, std::abs(yBot - yTop)));
}

const NewtoneNoteBlock* PitchTraceComponent::findNoteById(int id) const noexcept
{
    for (const auto& n : notes)
        if (n.id == id) return &n;
    return nullptr;
}

NewtoneNoteBlock* PitchTraceComponent::findNoteById(int id) noexcept
{
    for (auto& n : notes)
        if (n.id == id) return &n;
    return nullptr;
}

int PitchTraceComponent::findNoteIndexAtPoint(juce::Point<float> pt, juce::Rectangle<float> grid) const noexcept
{
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
    {
        if (getNoteRect(notes[(size_t)i], grid).expanded(2.0f, 3.0f).contains(pt))
            return i;
    }
    return -1;
}

float PitchTraceComponent::evaluateShapedMidiAtHop(int hopIdx, float globalCenter, float globalVariation, float globalTransition) const noexcept
{
    if (hopIdx < 0 || hopIdx >= static_cast<int>(timeline.size()))
        return 0.0f;

    const auto& th = timeline[(size_t)hopIdx];
    if (!th.data.isVoiced || th.rawMidi <= 0.0f)
        return 0.0f;

    if (th.hasCustomPitch && th.customMidi > 0.0f)
        return th.customMidi;

    const NewtoneNoteBlock* nb = findNoteById(th.noteId);
    if (nb == nullptr || nb->isSibilant)
        return th.rawMidi;

    const float humSlider  = queue.globalHumanizeAmount.load(std::memory_order_relaxed);
    const float hDrive     = (humSlider > 0.001f)
        ? juce::jlimit(0.0f, 2.0f, humSlider * 2.0f)
        : (queue.globalHumanize.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
    const float hGate      = juce::jlimit(0.0f, 1.0f, hDrive);
    const float hExtreme   = juce::jlimit(0.0f, 1.0f, hDrive - 1.0f);

    const float baseCenter = juce::jlimit(0.0f, 1.0f, globalCenter * nb->noteCenter);
    const float effCenter  = baseCenter * (1.0f - 0.06f * hDrive);
    const float rawVar     = juce::jlimit(0.0f, 2.0f, globalVariation * nb->noteVariation);
    const float effVar     = juce::jmax(rawVar, 0.35f * hGate + 0.25f * hExtreme);

    const float targetPitch = nb->effectiveTargetMidi();
    const float centreErr   = targetPitch - nb->detectedMidi;
    const float softDeadband = 0.028f * hDrive;
    const float shapedErr   = (softDeadband > 1.0e-4f)
        ? std::copysign(juce::jmax(0.0f, std::abs(centreErr) - softDeadband * std::tanh(std::abs(centreErr) / softDeadband)), centreErr)
        : centreErr;
    const float noteCentre  = nb->detectedMidi + effCenter * shapedErr;

    const float deviation = th.rawMidi - nb->detectedMidi;
    float intentSlopeScale = 1.0f;
    if (hGate > 0.01f)
    {
        const float pullDelta = noteCentre - nb->detectedMidi;
        float intervalRatio = 1.0f + 0.22f * std::tanh(pullDelta / 1.5f);
        if (nb->startHop > 0 && timeline[(size_t)(nb->startHop - 1)].data.isVoiced)
        {
            if (const NewtoneNoteBlock* prevNb = findNoteById(timeline[(size_t)(nb->startHop - 1)].noteId))
            {
                if (!prevNb->isSibilant)
                {
                    const float prevEffC   = juce::jlimit(0.0f, 1.0f, globalCenter * prevNb->noteCenter) * (1.0f - 0.06f * hDrive);
                    const float prevCentre = prevNb->detectedMidi + prevEffC * (prevNb->effectiveTargetMidi() - prevNb->detectedMidi);
                    const float sungStep   = nb->detectedMidi - prevNb->detectedMidi;
                    const float aimedStep  = noteCentre - prevCentre;
                    if (std::abs(sungStep) > 0.08f)
                        intervalRatio = (std::abs(aimedStep) + 0.55f) / (std::abs(sungStep) + 0.55f);
                }
            }
        }
        const float registerSlope  = std::exp2(juce::jlimit(-12.0f, 12.0f, pullDelta) / 36.0f);
        const float rawIntentSlope = juce::jlimit(0.58f, 1.62f, intervalRatio * registerSlope);
        const float asymDir = (deviation * pullDelta < 0.0f)
            ? (1.0f - (0.35f + 0.25f * hExtreme) * std::tanh(std::abs(pullDelta) / 0.30f))
            : (1.0f + (0.14f + 0.18f * hExtreme) * std::tanh(std::abs(pullDelta) / 0.45f));
        const float baseSlope = juce::jlimit(0.48f, 1.68f, rawIntentSlope * asymDir);
        const float extremeGain = 1.0f + 0.75f * hExtreme;
        intentSlopeScale = juce::jlimit(0.35f, 2.15f, 1.0f + hGate * extremeGain * (baseSlope - 1.0f));
    }

    float shaped = noteCentre + effVar * intentSlopeScale * deviation;

    const int noteLen = juce::jmax(1, nb->endHop - nb->startHop);
    const int relHop  = hopIdx - nb->startHop;
    const float humanizedTrans = juce::jmin(0.68f - 0.14f * hExtreme, globalTransition * (0.72f - 0.14f * hExtreme));
    const float effTrans = globalTransition + hGate * (humanizedTrans - globalTransition);
    const float transitionScale = juce::jlimit(0.0f, 1.8f, (1.0f - effTrans) * (1.15f + 0.30f * hGate + 0.25f * hExtreme));

    if (hGate > 0.01f)
    {
        const float tSec = static_cast<float>(th.timeStampSeconds);
        const float larynDrift = 0.011f * std::sin(juce::MathConstants<float>::twoPi * 1.37f * tSec)
                               + 0.007f * std::sin(juce::MathConstants<float>::twoPi * 2.83f * tSec + 1.7f);
        const float bloom = juce::jlimit(0.0f, 1.0f, (static_cast<float>(relHop) - 16.0f) / 28.0f);
        const float gentleVib = 0.032f * bloom * std::sin(juce::MathConstants<float>::twoPi * 5.40f * tSec);
        const float driftScale = hGate + 0.75f * hExtreme;
        shaped += driftScale * intentSlopeScale * (larynDrift + gentleVib);
    }

    const float rampInHops  = static_cast<float>(noteLen) * nb->rampInRatio * transitionScale;
    const float rampOutHops = static_cast<float>(noteLen) * nb->rampOutRatio * transitionScale;

    if (rampInHops >= 0.5f && static_cast<float>(relHop) < rampInHops && nb->startHop > 0)
    {
        const int prevHop = nb->startHop - 1;
        if (prevHop >= 0 && timeline[(size_t)prevHop].data.isVoiced)
        {
            const NewtoneNoteBlock* prevNote = findNoteById(timeline[(size_t)prevHop].noteId);
            const float prevPitch = (prevNote != nullptr && !prevNote->isSibilant)
                ? (prevNote->detectedMidi + effCenter * (prevNote->effectiveTargetMidi() - prevNote->detectedMidi))
                : timeline[(size_t)prevHop].rawMidi;
            const float phase = juce::jlimit(0.0f, 1.0f, (static_cast<float>(relHop) + 0.5f) / rampInHops);
            const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * phase);
            const float anchor = 0.5f * (prevPitch + shaped);
            shaped = anchor + w * (shaped - anchor);
        }
    }

    const int hopsFromEnd = (nb->endHop - 1) - hopIdx;
    if (rampOutHops >= 0.5f && static_cast<float>(hopsFromEnd) < rampOutHops && nb->endHop < static_cast<int>(timeline.size()))
    {
        const int nextHop = nb->endHop;
        if (nextHop < static_cast<int>(timeline.size()) && timeline[(size_t)nextHop].data.isVoiced)
        {
            const NewtoneNoteBlock* nextNote = findNoteById(timeline[(size_t)nextHop].noteId);
            const float nextPitch = (nextNote != nullptr && !nextNote->isSibilant)
                ? (nextNote->detectedMidi + effCenter * (nextNote->effectiveTargetMidi() - nextNote->detectedMidi))
                : timeline[(size_t)nextHop].rawMidi;
            const float phase = juce::jlimit(0.0f, 1.0f, (static_cast<float>(hopsFromEnd) + 0.5f) / rampOutHops);
            const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * phase);
            const float anchor = 0.5f * (nextPitch + shaped);
            shaped = anchor + w * (shaped - anchor);
        }
    }

    return shaped;
}

float PitchTraceComponent::evaluatePolyShapedMidiAtHop(int hopIdx, int voiceLane, float globalCenter, float globalVariation, float /*globalTransition*/) const noexcept
{
    if (hopIdx < 0 || hopIdx >= static_cast<int>(timeline.size()) || voiceLane < 0 || voiceLane >= PitchTraceQueue::maxPolyVoices)
        return 0.0f;

    const auto& th = timeline[(size_t)hopIdx];
    const float rawM = (voiceLane < th.data.numPolyVoices) ? th.data.polyRawMidi[(size_t)voiceLane] : 0.0f;
    if (!th.data.isVoiced || rawM <= 0.0f)
        return 0.0f;

    const NewtoneNoteBlock* nb = findNoteById(th.polyNoteIds[(size_t)voiceLane]);
    if (nb == nullptr || nb->isSibilant)
        return rawM;

    if (isClipMode()) // the clip renderer moves the note by exactly this amount
        return rawM + (nb->effectiveTargetMidi() - nb->detectedMidi);

    const float humSlider  = queue.globalHumanizeAmount.load(std::memory_order_relaxed);
    const float hDrive     = (humSlider > 0.001f)
        ? juce::jlimit(0.0f, 2.0f, humSlider * 2.0f)
        : (queue.globalHumanize.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
    const float hGate      = juce::jlimit(0.0f, 1.0f, hDrive);
    const float baseCenter = juce::jlimit(0.0f, 1.0f, globalCenter * nb->noteCenter);
    const float effCenter  = baseCenter * (1.0f - 0.08f * hDrive);
    const float rawVar     = juce::jlimit(0.0f, 2.0f, globalVariation * nb->noteVariation);
    const float effVar     = juce::jmax(rawVar, 0.38f * hDrive);

    const float targetPitch = nb->effectiveTargetMidi();
    const float noteCentre  = nb->detectedMidi + effCenter * (targetPitch - nb->detectedMidi);
    const float deviation   = rawM - nb->detectedMidi;
    const float pullDelta   = noteCentre - nb->detectedMidi;
    const float rawPolySlope = juce::jlimit(0.62f, 1.50f, std::exp2(juce::jlimit(-12.0f, 12.0f, pullDelta) / 30.0f) * ((deviation * pullDelta < 0.0f) ? 0.82f : 1.12f));
    const float polySlope   = 1.0f + hDrive * (rawPolySlope - 1.0f);
    float shaped            = noteCentre + effVar * polySlope * deviation;

    if (hGate > 0.01f)
    {
        const float tSec = static_cast<float>(th.timeStampSeconds);
        shaped += 0.012f * hDrive * std::sin(juce::MathConstants<float>::twoPi * (1.25f + 0.2f * voiceLane) * tSec + (float)voiceLane);
    }
    return shaped;
}

//==============================================================================
// Slicing & Gluing Note Blocks
//==============================================================================
void PitchTraceComponent::splitNoteAtHop(int noteIndex, int splitHop)
{
    if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size()))
        return;

    auto orig = notes[(size_t)noteIndex];
    if (splitHop <= orig.startHop + 1 || splitHop >= orig.endHop - 1)
        return;

    pushUndoSnapshot();

    NewtoneNoteBlock right = orig;
    right.id       = nextNoteId++;
    right.startHop = splitHop;
    notes[(size_t)noteIndex].endHop = splitHop;

    auto recomputeMean = [&](NewtoneNoteBlock& b)
    {
        double sum = 0.0, wsum = 0.0;
        for (int k = b.startHop; k < b.endHop && k < static_cast<int>(timeline.size()); ++k)
        {
            timeline[(size_t)k].noteId = b.id;
            if (timeline[(size_t)k].rawMidi > 0.0f)
            {
                const double w = juce::jmax(0.001f, timeline[(size_t)k].data.rms);
                sum += timeline[(size_t)k].rawMidi * w;
                wsum += w;
            }
        }
        if (wsum > 0.0)
        {
            b.detectedMidi = static_cast<float>(sum / wsum);
            b.nearestMidi  = static_cast<int>(std::lround(b.detectedMidi));
            if (!b.userEdited)
                b.targetMidi = snapToScaleEnabled
                    ? snapMidiToScale(b.detectedMidi, queue.activeScaleMask.load(std::memory_order_relaxed))
                    : b.nearestMidi;
        }
    };

    recomputeMean(notes[(size_t)noteIndex]);
    recomputeMean(right);

    notes.insert(notes.begin() + noteIndex + 1, right);
    selectedNoteId = right.id;
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::joinNoteWithNext(int noteIndex)
{
    if (noteIndex < 0 || noteIndex + 1 >= static_cast<int>(notes.size()))
        return;

    auto& first  = notes[(size_t)noteIndex];
    auto& second = notes[(size_t)(noteIndex + 1)];
    if (first.isSibilant != second.isSibilant)
        return;

    pushUndoSnapshot();

    first.endHop     = second.endHop;
    first.userEdited = true;

    for (int k = first.startHop; k < first.endHop && k < static_cast<int>(timeline.size()); ++k)
        timeline[(size_t)k].noteId = first.id;

    notes.erase(notes.begin() + noteIndex + 1);
    selectedNoteId = first.id;
    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

//==============================================================================
// Painting: Piano Roll Ruler, Time Ruler, Note Blocks, Pitch Curves & Coach Bar
//==============================================================================
void PitchTraceComponent::paintPianoRollRuler(juce::Graphics& g, juce::Rectangle<float> area, uint16_t scaleMask)
{
    using namespace HarmonyEngineColours;
    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(area.toNearestInt());

    g.setColour(panel);
    g.fillRect(area);

    const int minMidi = juce::jmax(12, static_cast<int>(std::floor(viewCentreMidiNote - viewHalfRangeSemitones)) - 1);
    const int maxMidi = juce::jmin(108, static_cast<int>(std::ceil(viewCentreMidiNote + viewHalfRangeSemitones)) + 1);

    for (int m = minMidi; m <= maxMidi; ++m)
    {
        const float yTop = midiNoteToY(static_cast<float>(m) + 0.5f);
        const float yBot = midiNoteToY(static_cast<float>(m) - 0.5f);
        const juce::Rectangle<float> keyRect(area.getX(), yTop, area.getWidth() - 1.0f, juce::jmax(2.0f, yBot - yTop));

        const bool black   = isBlackKey(m);
        const bool inScale = isMidiNoteInScale(m, scaleMask);

        // Dark keyboard: white keys are the lighter rows; the key area is inset from the right edge.
        const auto keyArea = black ? keyRect.withTrimmedRight(keyRect.getWidth() * 0.38f) : keyRect;
        g.setColour(black ? background : panelLight.brighter(isLightChassis ? 0.0f : 0.05f));
        g.fillRect(keyArea.withTrimmedRight(4.0f));

        if (inScale)
        {
            g.setColour(mintAccent.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>(keyRect.getRight() - 3.0f, keyRect.getY() + 1.0f, 2.0f, keyRect.getHeight() - 2.0f));
        }

        g.setColour(hairline());
        g.drawHorizontalLine(juce::roundToInt(yBot), area.getX(), area.getRight() - 4.0f);

        const bool isC = (m % 12 == 0);
        if (isC || (keyRect.getHeight() >= 14.0f && ! black))
        {
            g.setFont(HarmonyEngineUI::font(10.0f, isC));
            g.setColour(isC ? textPrimary : textSecondary.withAlpha(inScale ? 0.9f : 0.5f));
            g.drawText(midiNoteName(m), keyRect.reduced(6.0f, 0.0f).withTrimmedRight(2.0f), juce::Justification::centredLeft, false);
        }
    }

    g.setColour(hairline());
    g.drawVerticalLine(juce::roundToInt(area.getRight() - 1.0f), area.getY(), area.getBottom());
}

void PitchTraceComponent::paintTimeRuler(juce::Graphics& g, juce::Rectangle<float> area, juce::Rectangle<float> grid)
{
    using namespace HarmonyEngineColours;
    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(area.toNearestInt());

    g.setColour(panel);
    g.fillRect(area);

    constexpr float hopsPerBeat = 46.875f;
    const int firstBeat = juce::jmax(0, static_cast<int>(std::floor(viewStartHop / hopsPerBeat)));
    const int lastBeat  = static_cast<int>(std::ceil((viewStartHop + visibleHopsCount) / hopsPerBeat));

    g.setFont(HarmonyEngineUI::font(10.5f, true));
    for (int beat = firstBeat; beat <= lastBeat; ++beat)
    {
        const float hop = static_cast<float>(beat) * hopsPerBeat;
        const float x   = hopToX(hop, grid);
        const bool isBar = (beat % 4) == 0;

        g.setColour(isBar ? textSecondary.withAlpha(0.8f) : hairline().withMultipliedAlpha(2.2f));
        g.drawVerticalLine(juce::roundToInt(x), isBar ? area.getY() + 4.0f : area.getBottom() - 6.0f, area.getBottom());

        if (isBar)
        {
            g.setColour(textSecondary);
            g.drawText(juce::String((beat / 4) + 1), juce::Rectangle<float>(x + 5.0f, area.getY(), 40.0f, area.getHeight()),
                       juce::Justification::centredLeft, false);
        }
    }

    // Playhead marker
    const int activePlayHop = playheadHopForPaint();
    if (activePlayHop > 0)
    {
        const float px = hopToX(static_cast<float>(activePlayHop), grid);
        if (px >= area.getX() && px <= area.getRight())
        {
            juce::Path tri;
            tri.addTriangle(px - 5.0f, area.getBottom() - 7.0f, px + 5.0f, area.getBottom() - 7.0f, px, area.getBottom());
            g.setColour(cyanBright);
            g.fillPath(tri);
        }
    }

    g.setColour(hairline());
    g.drawHorizontalLine(juce::roundToInt(area.getBottom() - 1.0f), area.getX(), area.getRight());
}

void PitchTraceComponent::paintCoachBar(juce::Graphics& g, juce::Rectangle<float> area)
{
    using namespace HarmonyEngineColours;
    if (area.isEmpty()) return;

    g.setColour(panel);
    g.fillRect(area);
    g.setColour(hairline());
    g.drawHorizontalLine(juce::roundToInt(area.getY()), area.getX(), area.getRight());

    auto text = area.reduced(12.0f, 0.0f);
    const auto modeText = polyphonicModeEnabled ? juce::String("Poly") : juce::String("Mono");
    g.setFont(HarmonyEngineUI::font(11.0f, true));
    g.setColour(polyphonicModeEnabled ? mintAccent : cyanAccent);
    const float mw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), modeText) + 12.0f;
    g.drawText(modeText, text.removeFromLeft(mw), juce::Justification::centredLeft, false);

    g.setFont(HarmonyEngineUI::font(11.5f, false));
    g.setColour(textSecondary);
    g.drawText(currentCoachHint, text, juce::Justification::centredLeft, true);
}

void PitchTraceComponent::paintGridAndNotes(juce::Graphics& g, juce::Rectangle<float> grid, uint16_t scaleMask)
{
    using namespace HarmonyEngineColours;
    juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(grid.toNearestInt());

    // 1. Horizontal semitone lanes: in-scale rows are lifted, out-of-scale rows recede.
    const int minMidi = juce::jmax(12, static_cast<int>(std::floor(viewCentreMidiNote - viewHalfRangeSemitones)) - 1);
    const int maxMidi = juce::jmin(108, static_cast<int>(std::ceil(viewCentreMidiNote + viewHalfRangeSemitones)) + 1);

    const auto rowIn      = graphBackground.brighter(isLightChassis ? 0.10f : 0.045f);
    const auto rowInBlack = graphBackground.brighter(isLightChassis ? 0.05f : 0.020f);
    const auto rowOut     = graphBackground.darker(0.25f);

    for (int m = minMidi; m <= maxMidi; ++m)
    {
        const float yTop = midiNoteToY(static_cast<float>(m) + 0.5f);
        const float yBot = midiNoteToY(static_cast<float>(m) - 0.5f);
        const juce::Rectangle<float> rowRect(grid.getX(), yTop, grid.getWidth(), juce::jmax(1.0f, yBot - yTop));

        const bool inScale = isMidiNoteInScale(m, scaleMask);
        g.setColour(! inScale ? rowOut : (isBlackKey(m) ? rowInBlack : rowIn));
        g.fillRect(rowRect);

        g.setColour(m % 12 == 0 ? textPrimary.withAlpha(0.10f) : textPrimary.withAlpha(0.035f));
        g.drawHorizontalLine(juce::roundToInt(yBot), grid.getX(), grid.getRight());
    }

    // 2. Vertical bar / beat / sub-beat lines
    constexpr float hopsPerQuarterBeat = 46.875f * 0.25f;
    const int firstSub = juce::jmax(0, static_cast<int>(std::floor(viewStartHop / hopsPerQuarterBeat)));
    const int lastSub  = static_cast<int>(std::ceil((viewStartHop + visibleHopsCount) / hopsPerQuarterBeat));

    if (snapToGridEnabled && timeline.size() > 1)
    {
        // Show the DAW's 1/16 grid the edits snap to. shortcut: bars assume 4/4.
        const int h0 = juce::jmax(1, static_cast<int>(viewStartHop));
        const int h1 = juce::jmin(static_cast<int>(timeline.size()) - 1, static_cast<int>(viewStartHop + visibleHopsCount) + 1);
        for (int h = h0; h <= h1; ++h)
        {
            const auto sub = static_cast<juce::int64>(std::floor(hopToHostPpq(h) / 0.25));
            if (sub == static_cast<juce::int64>(std::floor(hopToHostPpq(h - 1) / 0.25)))
                continue;
            const bool isBar  = (sub % 16) == 0;
            const bool isBeat = (sub % 4) == 0;
            g.setColour(textPrimary.withAlpha(isBar ? 0.12f : (isBeat ? 0.055f : 0.022f)));
            g.drawVerticalLine(juce::roundToInt(hopToX(static_cast<float>(h), grid)), grid.getY(), grid.getBottom());
        }
    }
    else
    for (int sub = firstSub; sub <= lastSub; ++sub)
    {
        const float x = hopToX(static_cast<float>(sub) * hopsPerQuarterBeat, grid);
        const bool isBar  = (sub % 16) == 0;
        const bool isBeat = (sub % 4) == 0;
        g.setColour(textPrimary.withAlpha(isBar ? 0.12f : (isBeat ? 0.055f : 0.022f)));
        g.drawVerticalLine(juce::roundToInt(x), grid.getY(), grid.getBottom());
    }

    // 2a. Fix #8: Draw Imported Reference MIDI Ghost Notes behind vocal notes
    for (const auto& refNote : referenceGhostNotes)
    {
        if (refNote.endHop < viewStartHop || refNote.startHop > viewStartHop + visibleHopsCount)
            continue;
        const float rx1 = hopToX(static_cast<float>(refNote.startHop), grid);
        const float rx2 = hopToX(static_cast<float>(refNote.endHop), grid);
        const float ryT = midiNoteToY(static_cast<float>(refNote.midiNote) + 0.44f);
        const float ryB = midiNoteToY(static_cast<float>(refNote.midiNote) - 0.44f);
        const juce::Rectangle<float> refRect(rx1, ryT, juce::jmax(4.0f, rx2 - rx1), juce::jmax(4.0f, ryB - ryT));
        g.setColour(mintAccent.withAlpha(0.14f));
        g.fillRoundedRectangle(refRect, 3.5f);
        g.setColour(mintAccent.withAlpha(0.55f));
        g.drawRoundedRectangle(refRect, 3.5f, 1.0f);
    }

    // 2b. Upgrade #4: Draw Live MIDI Reference Ghost Notes when MIDI input is active
    const int liveMidiNote = queue.liveMidiTargetNote.load(std::memory_order_relaxed);
    if (liveMidiNote >= 12 && liveMidiNote <= 108)
    {
        const float myTop = midiNoteToY(static_cast<float>(liveMidiNote) + 0.48f);
        const float myBot = midiNoteToY(static_cast<float>(liveMidiNote) - 0.48f);
        const juce::Rectangle<float> midiBar(grid.getX(), myTop, grid.getWidth(), juce::jmax(4.0f, myBot - myTop));
        g.setColour(mintAccent.withAlpha(0.18f));
        g.fillRect(midiBar);
        g.setColour(mintAccent.withAlpha(0.75f));
        g.drawRect(midiBar, 1.2f);
        g.setFont(HarmonyEngineUI::font(10.5f, true));
        g.setColour(mintAccent.brighter(0.3f));
        g.drawText("MIDI " + midiNoteName(liveMidiNote), midiBar.reduced(8.0f, 1.0f), juce::Justification::centredRight, false);
    }

    // 3. Draw NewTone Note Blocks (Ghost Semitone Blocks + Warm Amber Target Note Blocks + Waveform Blobs)
    const float globalCenter = queue.globalCenter.load(std::memory_order_relaxed);
    const float globalVar    = queue.globalVariation.load(std::memory_order_relaxed);
    const float globalTrans  = queue.globalTransition.load(std::memory_order_relaxed);

    for (const auto& note : notes)
    {
        if (note.endHop < viewStartHop - 4.0f || note.startHop > viewStartHop + visibleHopsCount + 4.0f)
            continue;

        const auto rect = getNoteRect(note, grid);
        const bool isSel = note.selected || (note.id == selectedNoteId);
        const bool isHov = (note.id == hoveredNoteId);
        const bool showAdv = (currentToolMode == ToolMode::AdvancedEdit) || (note.id == advancedEditNoteId);

        if (note.isSibilant)
        {
            g.setColour(mintAccent.withAlpha(0.16f));
            g.fillRoundedRectangle(rect, 4.0f);
            g.setColour(mintAccent.withAlpha(0.55f));
            g.drawRoundedRectangle(rect, 4.0f, 1.0f);
            if (rect.getWidth() > 22.0f)
            {
                g.setFont(HarmonyEngineUI::font(10.0f, false));
                g.setColour(mintAccent.brighter(0.25f));
                g.drawText("breath", rect.reduced(2.0f), juce::Justification::centred, false);
            }
            continue;
        }

        // 3a. Ghost Block at nearest originally sung semitone (dashed/faint slate outline)
        if (note.nearestMidi != note.targetMidi || std::abs(note.fineCents) > 1.0f)
        {
            const float gx1   = hopToX(static_cast<float>(note.startHop), grid);
            const float gx2   = hopToX(static_cast<float>(note.endHop), grid);
            const float gyTop = midiNoteToY(static_cast<float>(note.nearestMidi) + 0.44f);
            const float gyBot = midiNoteToY(static_cast<float>(note.nearestMidi) - 0.44f);
            const juce::Rectangle<float> ghostRect(gx1, gyTop, juce::jmax(4.0f, gx2 - gx1), juce::jmax(4.0f, gyBot - gyTop));

            g.setColour(pitchTraceUncorrected.withAlpha(0.16f));
            g.fillRoundedRectangle(ghostRect, 4.0f);
            g.setColour(pitchTraceUncorrected.withAlpha(0.45f));
            g.drawRoundedRectangle(ghostRect, 4.0f, 1.0f);
        }

        // 3b. Target note block: flat tinted fill
        g.setColour(accent.withAlpha(isSel ? 0.34f : (isHov ? 0.26f : 0.18f)));
        g.fillRoundedRectangle(rect, 4.0f);

        // 3c. Internal Waveform Amplitude Blob inside Note Block
        const float gainLin = juce::Decibels::decibelsToGain(note.gainDb);
        juce::Path blobPath;
        const float midY = rect.getCentreY();
        const float maxHalfH = rect.getHeight() * 0.45f;

        const float noteSpan = static_cast<float>(juce::jmax(1, note.endHop - note.startHop));
        auto fadeEnvAt = [&](int h) noexcept -> float
        {
            const float u = (static_cast<float>(h - note.startHop) + 0.5f) / noteSpan;
            float env = 1.0f;
            if (note.fadeInRatio > 0.005f && u < note.fadeInRatio)
                env *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (u / note.fadeInRatio));
            if (note.fadeOutRatio > 0.005f && (1.0f - u) < note.fadeOutRatio)
                env *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * ((1.0f - u) / note.fadeOutRatio));
            return env;
        };

        blobPath.startNewSubPath(rect.getX(), midY);
        for (int h = note.startHop; h < note.endHop && h < static_cast<int>(timeline.size()); ++h)
        {
            const float x = hopToX(static_cast<float>(h + note.warpOffsetHops) + 0.5f, grid);
            const float amp = juce::jlimit(0.04f, 1.0f, timeline[(size_t)h].data.rms * 2.2f * gainLin * fadeEnvAt(h));
            blobPath.lineTo(x, midY - amp * maxHalfH);
        }
        blobPath.lineTo(rect.getRight(), midY);
        for (int h = note.endHop - 1; h >= note.startHop && h < static_cast<int>(timeline.size()); --h)
        {
            const float x = hopToX(static_cast<float>(h + note.warpOffsetHops) + 0.5f, grid);
            const float amp = juce::jlimit(0.04f, 1.0f, timeline[(size_t)h].data.rms * 2.2f * gainLin * fadeEnvAt(h));
            blobPath.lineTo(x, midY + amp * maxHalfH);
        }
        blobPath.closeSubPath();

        g.setColour(accentBright.withAlpha(isSel ? 0.55f : 0.40f));
        g.fillPath(blobPath);

        // 3d. Border, ramp-in / ramp-out slopes (only prominent while shaping)
        g.setColour(isSel ? textPrimary : accent.withAlpha(isHov ? 1.0f : 0.85f));
        g.drawRoundedRectangle(rect.reduced(0.5f), 4.0f, isSel ? 1.6f : 1.0f);

        const float rampInX  = rect.getX() + rect.getWidth() * note.rampInRatio;
        const float rampOutX = rect.getRight() - rect.getWidth() * note.rampOutRatio;
        if (showAdv || isSel)
        {
            g.setColour(cyanBright.withAlpha(showAdv ? 0.8f : 0.35f));
            g.drawLine(rect.getX(), rect.getBottom(), rampInX, rect.getY(), 1.0f);
            g.drawLine(rampOutX, rect.getY(), rect.getRight(), rect.getBottom(), 1.0f);
        }

        // 3e. Note name + cents offset
        if (rect.getWidth() > 26.0f && rect.getHeight() >= 11.0f)
        {
            juce::String label = midiNoteName(note.targetMidi);
            if (std::abs(note.fineCents) >= 1.0f)
                label += (note.fineCents > 0.0f ? " +" : " ") + juce::String(juce::roundToInt(note.fineCents)) + "c";

            g.setFont(HarmonyEngineUI::font(juce::jlimit(9.5f, 11.0f, rect.getHeight() * 0.8f), true));
            g.setColour(isLightChassis ? juce::Colours::white : textPrimary);
            g.drawText(label, rect.reduced(6.0f, 0.0f), juce::Justification::centredLeft, false);
        }

        if (showAdv && rect.getWidth() > 44.0f)
        {
            g.setColour(cyanBright);
            g.fillEllipse(rampInX - 3.5f, rect.getY() - 3.5f, 7.0f, 7.0f);
            g.fillEllipse(rampOutX - 3.5f, rect.getY() - 3.5f, 7.0f, 7.0f);

            juce::String advText = "fmt " + juce::String(note.formantSemitones, 1)
                                 + "   var " + juce::String(juce::roundToInt(note.noteVariation * 100.0f)) + "%"
                                 + "   " + juce::String(note.gainDb, 1) + " dB";
            g.setFont(HarmonyEngineUI::font(10.0f, false));
            g.setColour(cyanBright);
            g.drawText(advText, rect.reduced(5.0f, 1.5f), juce::Justification::bottomRight, false);
        }
    }

    // 4. Continuous Uncorrected (Silver) & Corrected (Electric Cyan) Pitch Curves ON TOP of Note Blocks
    //    (Supports both single-voice Monophonic NewTone mode and 4-voice Polyphonic Melodyne Chord/Choir/String mode)
    if (!timeline.empty())
    {
        juce::Path uncorrectedPath, correctedPath;
        const int startH = juce::jmax(0, static_cast<int>(std::floor(viewStartHop)) - 1);
        const int endH   = juce::jmin(static_cast<int>(timeline.size()) - 1,
                                      static_cast<int>(std::ceil(viewStartHop + visibleHopsCount)) + 1);

        if (polyphonicModeEnabled)
        {
            // Trace each polyphonic note block's own pitch contour continuously across its hop span
            for (const auto& nb : notes)
            {
                if (nb.isSibilant || nb.endHop < startH || nb.startHop > endH)
                    continue;

                bool uncStarted = false, corStarted = false;
                const int hLo = juce::jmax(startH, nb.startHop);
                const int hHi = juce::jmin(endH,   nb.endHop - 1);

                for (int h = hLo; h <= hHi; ++h)
                {
                    const auto& th = timeline[(size_t)h];
                    int matchedSlot = -1;
                    for (int v = 0; v < static_cast<int>(th.data.numPolyVoices) && v < PitchTraceQueue::maxPolyVoices; ++v)
                    {
                        if (th.polyNoteIds[(size_t)v] == nb.id)
                        {
                            matchedSlot = v;
                            break;
                        }
                    }

                    if (matchedSlot >= 0 && th.data.polyRawMidi[(size_t)matchedSlot] > 0.0f)
                    {
                        const float rawM    = th.data.polyRawMidi[(size_t)matchedSlot];
                        const float shapedM = evaluatePolyShapedMidiAtHop(h, matchedSlot, globalCenter, globalVar, globalTrans);
                        const float x       = hopToX(static_cast<float>(h) + 0.5f, grid);
                        const float yRaw    = midiNoteToY(rawM);
                        const float yCor    = midiNoteToY(shapedM);

                        if (!uncStarted) { uncorrectedPath.startNewSubPath(x, yRaw); uncStarted = true; }
                        else             { uncorrectedPath.lineTo(x, yRaw); }

                        if (!corStarted) { correctedPath.startNewSubPath(x, yCor); corStarted = true; }
                        else             { correctedPath.lineTo(x, yCor); }
                    }
                    else
                    {
                        uncStarted = false;
                        corStarted = false;
                    }
                }
            }
        }
        else
        {
            bool uncorrectedStarted = false, correctedStarted = false;
            for (int h = startH; h <= endH; ++h)
            {
                const auto& th = timeline[(size_t)h];
                const float x  = hopToX(static_cast<float>(h) + 0.5f, grid);

                if (th.data.isVoiced && th.rawMidi > 0.0f)
                {
                    const float yRaw = midiNoteToY(th.rawMidi);
                    if (!uncorrectedStarted) { uncorrectedPath.startNewSubPath(x, yRaw); uncorrectedStarted = true; }
                    else                     { uncorrectedPath.lineTo(x, yRaw); }

                    const float shapedMidi = evaluateShapedMidiAtHop(h, globalCenter, globalVar, globalTrans);
                    const float yCor = midiNoteToY(shapedMidi);
                    if (!correctedStarted) { correctedPath.startNewSubPath(x, yCor); correctedStarted = true; }
                    else                   { correctedPath.lineTo(x, yCor); }
                }
                else
                {
                    uncorrectedStarted = false;
                    correctedStarted   = false;
                }
            }
        }

        // Original pitch (thin, neutral) and corrected pitch (accent line with a soft glow)
        g.setColour(pitchTraceUncorrected.withAlpha(0.75f));
        g.strokePath(uncorrectedPath, juce::PathStrokeType(1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(pitchTraceHeroHalo.withAlpha(0.18f));
        g.strokePath(correctedPath, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(pitchTraceHeroGold);
        g.strokePath(correctedPath, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const int activePlayHop = playheadHopForPaint();
        const float playheadX = hopToX(static_cast<float>(activePlayHop), grid);
        if (activePlayHop >= 0 && playheadX >= grid.getX() && playheadX <= grid.getRight())
        {
            g.setColour(cyanBright.withAlpha(0.7f));
            g.drawVerticalLine(juce::roundToInt(playheadX), grid.getY(), grid.getBottom());
        }
    }
}

void PitchTraceComponent::paint(juce::Graphics& g)
{
    using namespace HarmonyEngineColours;
    const auto outer = getLocalBounds().toFloat();
    const float radius = 10.0f;

    // Everything is clipped to the rounded card so rulers and grid share its corners.
    juce::Path card;
    card.addRoundedRectangle(outer, radius);
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(card);
        g.setColour(graphBackground);
        g.fillRect(outer);

        const uint16_t scaleMask = queue.activeScaleMask.load(std::memory_order_relaxed);
        const auto grid       = getGridArea();
        const auto pianoRuler = getPianoRulerArea();
        const auto timeRuler  = getTimeRulerArea();

        paintGridAndNotes(g, grid, scaleMask);
        paintPianoRollRuler(g, pianoRuler, scaleMask);
        paintTimeRuler(g, timeRuler, grid);
        paintCoachBar(g, getCoachBarArea());

        const juce::Rectangle<float> corner(pianoRuler.getX(), timeRuler.getY(), pianoRuler.getWidth(), timeRuler.getHeight());
        g.setColour(panel);
        g.fillRect(corner);
        g.setColour(hairline());
        g.drawHorizontalLine(juce::roundToInt(corner.getBottom() - 1.0f), corner.getX(), corner.getRight());
        g.drawVerticalLine(juce::roundToInt(corner.getRight() - 1.0f), corner.getY(), corner.getBottom());

        // Loading screen while a new sample is read / analysed
        const bool polyLoading = isClipMode() && clipSession->getStatus() == Poly::ClipSession::Status::analysing;
        if (loadingSample || polyLoading)
        {
            g.setColour(graphBackground.withAlpha(0.82f));
            g.fillRect(outer);
            const auto box = juce::Rectangle<float>(260.0f, 76.0f).withCentre(outer.getCentre());
            g.setColour(panel);
            g.fillRoundedRectangle(box, 8.0f);
            g.setColour(hairline());
            g.drawRoundedRectangle(box, 8.0f, 1.0f);
            g.setColour(textPrimary);
            g.setFont(HarmonyEngineUI::font(14.0f, true));
            g.drawText("Loading sample...", polyLoading ? box.withTrimmedBottom(34.0f) : box,
                       polyLoading ? juce::Justification::centredBottom : juce::Justification::centred);
            // Poly analyses in the background and reports progress; Mono loads in one blocking step (no bar).
            if (polyLoading)
            {
            const float p = juce::jlimit(0.0f, 1.0f, clipSession->getProgress());
            const auto bar = juce::Rectangle<float>(box.getWidth() - 40.0f, 6.0f).withCentre({ box.getCentreX(), box.getBottom() - 22.0f });
            g.setColour(textPrimary.withAlpha(0.12f));
            g.fillRoundedRectangle(bar, 3.0f);
            g.setColour(accent);
            g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * p), 3.0f);
            }
        }
    }

    g.setColour(hairline());
    g.drawRoundedRectangle(outer.reduced(0.5f), radius, 1.0f);
}

//==============================================================================
// Interactive Mouse & Keyboard Handling
//==============================================================================
bool PitchTraceComponent::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code  = key.getKeyCode();

    if (mods.isCommandDown() || mods.isCtrlDown())
    {
        // Ctrl+T -> Tune / Move Mode
        if (code == 't' || code == 'T')
        {
            setToolMode(ToolMode::StandardEdit);
            if (onToolModeChanged) onToolModeChanged(ToolMode::StandardEdit);
            return true;
        }
        // Ctrl+S -> Slice / Glue Mode
        if (code == 's' || code == 'S')
        {
            setToolMode(ToolMode::CutAndGlue);
            if (onToolModeChanged) onToolModeChanged(ToolMode::CutAndGlue);
            return true;
        }
        // Ctrl+A -> Select All Voiced Notes on the Piano Roll
        if (code == 'a' || code == 'A')
        {
            selectAllNotes();
            return true;
        }
        // Ctrl+E -> Note Shaper / Advanced Edit Mode (Formant, Variation, Gain, Ramp)
        if (code == 'e' || code == 'E')
        {
            setToolMode(ToolMode::AdvancedEdit);
            if (onToolModeChanged) onToolModeChanged(ToolMode::AdvancedEdit);
            return true;
        }
        // Ctrl+P -> Toggle Monophonic (NewTone) vs Polyphonic (Melodyne Chord/Choir/String) Mode
        if (code == 'p' || code == 'P')
        {
            setPolyphonicMode(!polyphonicModeEnabled);
            if (onTogglePolyphonicShortcut) onTogglePolyphonicShortcut();
            return true;
        }
        // Ctrl+M -> Toggle Instrument MIDI Clean & Denoise Mode
        if (code == 'm' || code == 'M')
        {
            setMidiCleanMode(!midiCleanModeEnabled);
            if (onToggleMidiCleanShortcut) onToggleMidiCleanShortcut();
            return true;
        }
        // Ctrl+H -> Toggle Humanize Mode
        if (code == 'h' || code == 'H')
        {
            if (onToggleHumanizeShortcut) onToggleHumanizeShortcut();
            return true;
        }
        // Ctrl+Q -> Auto-Snap All Notes to Active Scale
        if (code == 'q' || code == 'Q')
        {
            snapAllNotesToScale();
            return true;
        }
        // Ctrl+F -> Freehand Draw Curve Mode (4. DRAW)
        if (code == 'f' || code == 'F')
        {
            setToolMode(ToolMode::FreehandDraw);
            if (onToolModeChanged) onToolModeChanged(ToolMode::FreehandDraw);
            return true;
        }
        // Ctrl+K -> 1-Click Auto-Key Chromagram Detector
        if (code == 'k' || code == 'K')
        {
            autoDetectKeyAndApply();
            return true;
        }
        // Ctrl+L -> 1-Click Note Leveler Macro
        if (code == 'l' || code == 'L')
        {
            levelAllNotesVolume(0.72f);
            return true;
        }
        // Ctrl+W -> Bounce & Export Tuned Vocal Audio (.wav)
        if (code == 'w' || code == 'W')
        {
            promptExportWavFile();
            return true;
        }
        // Ctrl+D -> Reset Selected Note to Default
        if (code == 'd' || code == 'D')
        {
            resetSelectedNote();
            return true;
        }
        // Ctrl+B -> Cycle Lower Bar & Dock Layout
        if (code == 'b' || code == 'B')
        {
            if (onCycleDockShortcut) onCycleDockShortcut();
            return true;
        }
        // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y -> Undo / Redo
        if (code == 'z' || code == 'Z')
        {
            if (mods.isShiftDown()) redo();
            else                    undo();
            return true;
        }
        if (code == 'y' || code == 'Y')
        {
            redo();
            return true;
        }
    }
    else if (!mods.isAltDown())
    {
        // Single-key 1 / 2 / 3 tool mode hotkeys
        if (code == '1')
        {
            setToolMode(ToolMode::StandardEdit);
            if (onToolModeChanged) onToolModeChanged(ToolMode::StandardEdit);
            return true;
        }
        if (code == '2')
        {
            setToolMode(ToolMode::CutAndGlue);
            if (onToolModeChanged) onToolModeChanged(ToolMode::CutAndGlue);
            return true;
        }
        if (code == '3')
        {
            setToolMode(ToolMode::AdvancedEdit);
            if (onToolModeChanged) onToolModeChanged(ToolMode::AdvancedEdit);
            return true;
        }
        if (code == '4')
        {
            setToolMode(ToolMode::FreehandDraw);
            if (onToolModeChanged) onToolModeChanged(ToolMode::FreehandDraw);
            return true;
        }
    }

    if (key == juce::KeyPress::escapeKey)
    {
        deselectAllNotes();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        resetSelectedNote();
        return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        if (mods.isAltDown())
        {
            if (const auto* sel = getSelectedNote())
                setSelectedNoteFineCents(sel->fineCents + 5.0f);
        }
        else
        {
            transposeSelectedNote(mods.isShiftDown() ? 12 : 1);
        }
        return true;
    }
    if (key == juce::KeyPress::downKey)
    {
        if (mods.isAltDown())
        {
            if (const auto* sel = getSelectedNote())
                setSelectedNoteFineCents(sel->fineCents - 5.0f);
        }
        else
        {
            transposeSelectedNote(mods.isShiftDown() ? -12 : -1);
        }
        return true;
    }
    if (key == juce::KeyPress::leftKey)
    {
        selectAdjacentNote(-1);
        return true;
    }
    if (key == juce::KeyPress::rightKey)
    {
        selectAdjacentNote(1);
        return true;
    }
    return false;
}

void PitchTraceComponent::mouseMove(const juce::MouseEvent& e)
{
    const auto pt         = e.position;
    const auto grid       = getGridArea();
    const auto pianoRuler = getPianoRulerArea();

    const auto timeRuler = getTimeRulerArea();
    if (timeRuler.contains(pt))
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        currentCoachHint = "Click or drag on the ruler to move the playhead.";
        repaint(getCoachBarArea().toNearestInt());
        return;
    }

    if (pianoRuler.contains(pt))
    {
        const int hoveredMidi = juce::jlimit(12, 108, static_cast<int>(std::lround(yToMidiNote(pt.y))));
        currentCoachHint = "Piano Key " + midiNoteName(hoveredMidi) + ": Click to snap the currently selected vocal note straight to " + midiNoteName(hoveredMidi) + ".";
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        repaint(getCoachBarArea().toNearestInt());
        return;
    }

    const int idx = grid.contains(pt) ? findNoteIndexAtPoint(pt, grid) : -1;
    const int newHoverId = (idx >= 0) ? notes[(size_t)idx].id : -1;

    if (newHoverId != hoveredNoteId)
    {
        hoveredNoteId = newHoverId;
        repaint();
    }

    if (idx >= 0)
    {
        const auto& note = notes[(size_t)idx];
        const auto rect  = getNoteRect(note, grid);
        const juce::String noteStr = midiNoteName(note.targetMidi);

        if (currentToolMode == ToolMode::CutAndGlue || e.mods.isShiftDown())
        {
            setMouseCursor(juce::MouseCursor::IBeamCursor);
            currentCoachHint = "Note " + noteStr + ": Click to slice at cursor, or hold Ctrl + Click to glue with the next note.";
        }
        else if (currentToolMode == ToolMode::AdvancedEdit || note.id == advancedEditNoteId)
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            currentCoachHint = "Shaping Note " + noteStr + ": Drag top = Formant (" + juce::String(note.formantSemitones, 1)
                             + " st) | mid = Variation (" + juce::String(juce::roundToInt(note.noteVariation * 100.0f))
                             + "%) | bottom = Gain (" + juce::String(note.gainDb, 1) + " dB).";
        }
        else if (std::abs(pt.x - rect.getX()) < 6.0f || std::abs(pt.x - rect.getRight()) < 6.0f)
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            currentCoachHint = "Note " + noteStr + " Boundary: Drag left/right to adjust note timing, or drag up/down in the center to tune pitch.";
        }
        else
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            currentCoachHint = "Note " + noteStr + ": Drag up/down to change semitones | Alt+Drag for fine cents | Right-Click to snap to scale | Double-Click to shape.";
        }
    }
    else
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        currentCoachHint = "Drag empty space to pan. Drop a WAV/MP3 here to edit it.";
    }
    repaint(getCoachBarArea().toNearestInt());
}

void PitchTraceComponent::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const auto pt   = e.position;
    const auto grid = getGridArea();
    const auto pianoRuler = getPianoRulerArea();
    const uint16_t scaleMask = queue.activeScaleMask.load(std::memory_order_relaxed);

    currentDragAction = DragAction::None;
    dragStartPos = pt;

    const auto timeRuler = getTimeRulerArea();
    if (timeRuler.contains(pt))
    {
        currentDragAction = DragAction::ScrubRuler;
        const int scrubH = juce::jlimit(0, juce::jmax(0, static_cast<int>(timeline.size()) - 1),
                                        static_cast<int>(std::lround(xToHop(pt.x, grid))));
        queue.currentPlayheadHop.store(scrubH, std::memory_order_relaxed);
        if (! polyphonicModeEnabled)
            queue.monoPreview.pos.store(static_cast<double>(scrubH) * queue.monoPreview.hopSamples);
        if (isClipMode())
        {
            clipPlayHop = scrubH;
            clipSession->seekPreview(static_cast<juce::int64>(scrubH) * clipHopSize);
        }
        if (scrubH < static_cast<int>(timeline.size()) && timeline[(size_t)scrubH].noteId > 0)
        {
            selectedNoteId = timeline[(size_t)scrubH].noteId;
            syncActiveOverrideToEngine();
            notifySelectionChanged();
        }
        repaint();
        return;
    }

    if (pianoRuler.contains(pt))
    {
        const int clickedMidi = juce::jlimit(12, 108, static_cast<int>(std::lround(yToMidiNote(pt.y))));
        if (getSelectedNoteCount() > 0)
        {
            pushUndoSnapshot();
            for (auto& n : notes)
            {
                if (!n.isSibilant && (n.selected || n.id == selectedNoteId))
                {
                    n.targetMidi = clickedMidi;
                    n.fineCents  = 0.0f;
                    n.userEdited = true;
                }
            }
            syncActiveOverrideToEngine();
            notifySelectionChanged();
            repaint();
        }
        return;
    }

    if (e.mods.isMiddleButtonDown() || (e.mods.isCtrlDown() && e.mods.isAltDown()))
    {
        currentDragAction = DragAction::PanCanvas;
        dragStartViewHop  = viewStartHop;
        dragStartViewMidi = viewCentreMidiNote;
        return;
    }

    if (!grid.contains(pt))
        return;

    if (currentToolMode == ToolMode::FreehandDraw && !e.mods.isPopupMenu())
    {
        pushUndoSnapshot();
        currentDragAction = DragAction::DrawFreehandCurve;
        lastDrawPos = pt;
        paintFreehandStrokeBetween(pt, pt, grid);
        syncActiveOverrideToEngine();
        notifySelectionChanged();
        repaint();
        return;
    }

    const int noteIdx = findNoteIndexAtPoint(pt, grid);
    if (noteIdx < 0)
    {
        if (!e.mods.isCommandDown() && !e.mods.isCtrlDown())
        {
            for (auto& n : notes)
                n.selected = false;
            notifySelectionChanged();
        }
        currentDragAction = DragAction::PanCanvas;
        dragStartViewHop  = viewStartHop;
        dragStartViewMidi = viewCentreMidiNote;
        repaint();
        return;
    }

    auto& note = notes[(size_t)noteIdx];

    if (currentToolMode != ToolMode::CutAndGlue && !e.mods.isShiftDown() && !e.mods.isPopupMenu())
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            note.selected = !note.selected;
            selectedNoteId = note.id;
            notifySelectionChanged();
            repaint();
            return;
        }
        else if (!note.selected)
        {
            for (auto& n : notes)
                n.selected = false;
            note.selected = true;
        }
    }

    selectedNoteId = note.id;
    notifySelectionChanged();

    if (e.mods.isPopupMenu())
    {
        pushUndoSnapshot();
        for (auto& n : notes)
        {
            if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                continue;
            if (e.mods.isCtrlDown())
            {
                n.targetMidi       = n.nearestMidi;
                n.fineCents        = 0.0f;
                n.noteVariation    = 1.0f;
                n.formantSemitones = 0.0f;
                n.gainDb           = 0.0f;
                n.userEdited       = false;
            }
            else
            {
                n.targetMidi = snapMidiToScale(n.detectedMidi, scaleMask);
                n.fineCents  = 0.0f;
                n.userEdited = true;
            }
        }
        syncActiveOverrideToEngine();
        notifySelectionChanged();
        repaint();
        return;
    }

    if (currentToolMode == ToolMode::CutAndGlue || e.mods.isShiftDown())
    {
        if (e.mods.isCtrlDown() || e.mods.isAltDown())
            joinNoteWithNext(noteIdx);
        else
            splitNoteAtHop(noteIdx, snapHopToHostGrid(static_cast<int>(std::lround(xToHop(pt.x, grid)))));
        return;
    }

    pushUndoSnapshot();
    dragStartNotesSnapshot = notes;

    const auto rect = getNoteRect(note, grid);
    dragStartTargetMidi = note.targetMidi;
    dragStartFineCents  = note.fineCents;
    dragStartHopLeft    = note.startHop;
    dragStartHopRight   = note.endHop;
    dragStartVariation  = note.noteVariation;
    dragStartFormant    = note.formantSemitones;
    dragStartGainDb     = note.gainDb;
    dragStartRampIn     = note.rampInRatio;
    dragStartRampOut    = note.rampOutRatio;

    const bool inAdv = (currentToolMode == ToolMode::AdvancedEdit) || (note.id == advancedEditNoteId);
    if (inAdv)
    {
        const float relX = (pt.x - rect.getX()) / juce::jmax(1.0f, rect.getWidth());
        const float relY = (pt.y - rect.getY()) / juce::jmax(1.0f, rect.getHeight());

        dragStartFadeIn  = note.fadeInRatio;
        dragStartFadeOut = note.fadeOutRatio;
        if (relX < 0.18f && relY < 0.45f)
            currentDragAction = DragAction::AdjustRampIn;
        else if (relX > 0.82f && relY < 0.45f)
            currentDragAction = DragAction::AdjustRampOut;
        else if (relX < 0.18f && relY > 0.58f)
            currentDragAction = DragAction::AdjustFadeIn;
        else if (relX > 0.82f && relY > 0.58f)
            currentDragAction = DragAction::AdjustFadeOut;
        else if (relY < 0.33f)
            currentDragAction = DragAction::AdjustNoteFormant;
        else if (relY < 0.66f)
            currentDragAction = DragAction::AdjustNoteVariation;
        else
            currentDragAction = DragAction::AdjustNoteGain;

        syncActiveOverrideToEngine();
        repaint();
        return;
    }

    if (std::abs(pt.x - rect.getX()) < 6.0f)
        currentDragAction = DragAction::TrimNoteLeft;
    else if (std::abs(pt.x - rect.getRight()) < 6.0f)
        currentDragAction = DragAction::TrimNoteRight;
    else if (e.mods.isAltDown())
        currentDragAction = DragAction::MoveNoteFineCents;
    else
        currentDragAction = DragAction::MoveNotePitch;

    syncActiveOverrideToEngine();
    repaint();
}

void PitchTraceComponent::mouseDrag(const juce::MouseEvent& e)
{
    const auto pt   = e.position;
    const auto grid = getGridArea();
    const float dy  = dragStartPos.y - pt.y;
    const float dx  = pt.x - dragStartPos.x;

    if (currentDragAction == DragAction::PanCanvas)
    {
        const float hopDelta  = (dx / juce::jmax(1.0f, grid.getWidth())) * visibleHopsCount;
        const float midiDelta = (dy / juce::jmax(1.0f, grid.getHeight())) * (2.0f * viewHalfRangeSemitones);
        viewStartHop       = juce::jmax(0.0f, dragStartViewHop - hopDelta);
        viewCentreMidiNote = juce::jlimit(24.0f, 96.0f, dragStartViewMidi - midiDelta);
        repaint();
        return;
    }

    if (currentDragAction == DragAction::DrawFreehandCurve)
    {
        paintFreehandStrokeBetween(lastDrawPos, pt, grid);
        lastDrawPos = pt;
        syncActiveOverrideToEngine();
        repaint();
        return;
    }

    if (currentDragAction == DragAction::ScrubRuler)
    {
        const int scrubH = juce::jlimit(0, juce::jmax(0, static_cast<int>(timeline.size()) - 1),
                                        static_cast<int>(std::lround(xToHop(pt.x, grid))));
        queue.currentPlayheadHop.store(scrubH, std::memory_order_relaxed);
        if (! polyphonicModeEnabled)
            queue.monoPreview.pos.store(static_cast<double>(scrubH) * queue.monoPreview.hopSamples);
        if (isClipMode())
        {
            clipPlayHop = scrubH;
            clipSession->seekPreview(static_cast<juce::int64>(scrubH) * clipHopSize);
        }
        if (scrubH < static_cast<int>(timeline.size()) && timeline[(size_t)scrubH].noteId > 0)
        {
            selectedNoteId = timeline[(size_t)scrubH].noteId;
            syncActiveOverrideToEngine();
            notifySelectionChanged();
        }
        repaint();
        return;
    }

    auto* note = findNoteById(selectedNoteId);
    if (note == nullptr || note->isSibilant)
        return;

    const uint16_t scaleMask = queue.activeScaleMask.load(std::memory_order_relaxed);
    const bool hasSnapshot = (dragStartNotesSnapshot.size() == notes.size());

    switch (currentDragAction)
    {
        case DragAction::MoveNotePitch:
        {
            const float hoveredMidi = yToMidiNote(pt.y);
            const int newPrimaryMidi = snapToScaleEnabled
                ? snapMidiToScale(hoveredMidi, scaleMask)
                : juce::jlimit(12, 108, static_cast<int>(std::lround(hoveredMidi)));
            const int deltaMidi = newPrimaryMidi - dragStartTargetMidi;

            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const int startM = hasSnapshot ? dragStartNotesSnapshot[i].targetMidi : n.targetMidi;
                const int rawTarget = juce::jlimit(12, 108, startM + deltaMidi);
                n.targetMidi = snapToScaleEnabled ? snapMidiToScale(static_cast<float>(rawTarget), scaleMask) : rawTarget;
                n.userEdited = true;
            }
            break;
        }
        case DragAction::MoveNoteFineCents:
        {
            const float centsDelta = dy * 1.5f;
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const int startM   = hasSnapshot ? dragStartNotesSnapshot[i].targetMidi : dragStartTargetMidi;
                const float startC = hasSnapshot ? dragStartNotesSnapshot[i].fineCents  : dragStartFineCents;
                float totalCents   = startC + centsDelta;
                int semitoneCarry  = 0;
                while (totalCents > 50.0f)  { totalCents -= 100.0f; ++semitoneCarry; }
                while (totalCents < -50.0f) { totalCents += 100.0f; --semitoneCarry; }
                n.targetMidi = juce::jlimit(12, 108, startM + semitoneCarry);
                n.fineCents  = juce::jlimit(-50.0f, 50.0f, totalCents);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::TrimNoteLeft:
        {
            const int oldStart = note->startHop;
            const int newStart = juce::jlimit(0, note->endHop - 2, snapHopToHostGrid(static_cast<int>(std::lround(xToHop(pt.x, grid)))));
            // Problem #3 Fix: Linked Boundary Time-Warping (Alt + Left-Edge Drag stretches/compresses previous note):
            if (e.mods.isAltDown())
            {
                for (auto& prev : notes)
                {
                    if (prev.id != note->id && prev.endHop == oldStart && newStart > prev.startHop + 2)
                    {
                        prev.endHop = newStart;
                        prev.userEdited = true;
                        break;
                    }
                }
            }
            note->startHop   = newStart;
            note->userEdited = true;
            relinkTimelineToNotes();
            break;
        }
        case DragAction::TrimNoteRight:
        {
            const int maxH   = static_cast<int>(timeline.size());
            const int oldEnd = note->endHop;
            const int newEnd = juce::jlimit(note->startHop + 2, maxH, snapHopToHostGrid(static_cast<int>(std::lround(xToHop(pt.x, grid)))));
            // Problem #3 Fix: Linked Boundary Time-Warping (Alt + Right-Edge Drag stretches/compresses next note):
            if (e.mods.isAltDown())
            {
                for (auto& next : notes)
                {
                    if (next.id != note->id && next.startHop == oldEnd && newEnd < next.endHop - 2)
                    {
                        next.startHop = newEnd;
                        next.userEdited = true;
                        break;
                    }
                }
            }
            note->endHop     = newEnd;
            note->userEdited = true;
            relinkTimelineToNotes();
            break;
        }
        case DragAction::AdjustNoteFormant:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseF = hasSnapshot ? dragStartNotesSnapshot[i].formantSemitones : dragStartFormant;
                n.formantSemitones = juce::jlimit(-12.0f, 12.0f, baseF + dy * 0.12f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustNoteVariation:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseV = hasSnapshot ? dragStartNotesSnapshot[i].noteVariation : dragStartVariation;
                n.noteVariation = juce::jlimit(0.0f, 2.0f, baseV + dy * 0.012f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustNoteGain:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseG = hasSnapshot ? dragStartNotesSnapshot[i].gainDb : dragStartGainDb;
                n.gainDb = juce::jlimit(-18.0f, 12.0f, baseG + dy * 0.18f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustRampIn:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseR = hasSnapshot ? dragStartNotesSnapshot[i].rampInRatio : dragStartRampIn;
                n.rampInRatio = juce::jlimit(0.02f, 0.48f, baseR + dx * 0.004f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustRampOut:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseR = hasSnapshot ? dragStartNotesSnapshot[i].rampOutRatio : dragStartRampOut;
                n.rampOutRatio = juce::jlimit(0.02f, 0.48f, baseR - dx * 0.004f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustFadeIn:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseF = hasSnapshot ? dragStartNotesSnapshot[i].fadeInRatio : dragStartFadeIn;
                n.fadeInRatio = juce::jlimit(0.0f, 0.45f, baseF + dx * 0.004f);
                n.userEdited = true;
            }
            break;
        }
        case DragAction::AdjustFadeOut:
        {
            for (size_t i = 0; i < notes.size(); ++i)
            {
                auto& n = notes[i];
                if (n.isSibilant || !(n.selected || n.id == selectedNoteId))
                    continue;
                const float baseF = hasSnapshot ? dragStartNotesSnapshot[i].fadeOutRatio : dragStartFadeOut;
                n.fadeOutRatio = juce::jlimit(0.0f, 0.45f, baseF - dx * 0.004f);
                n.userEdited = true;
            }
            break;
        }
        default:
            break;
    }

    syncActiveOverrideToEngine();
    notifySelectionChanged();
    repaint();
}

void PitchTraceComponent::mouseUp(const juce::MouseEvent&)
{
    currentDragAction = DragAction::None;
}

void PitchTraceComponent::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto grid = getGridArea();
    const int idx = grid.contains(e.position) ? findNoteIndexAtPoint(e.position, grid) : -1;
    if (idx >= 0)
    {
        const int id = notes[(size_t)idx].id;
        advancedEditNoteId = (advancedEditNoteId == id) ? -1 : id;
        selectedNoteId = id;
        notifySelectionChanged();
        repaint();
    }
    else
    {
        advancedEditNoteId = -1;
        repaint();
    }
}

void PitchTraceComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown())
    {
        const float factor = wheel.deltaY > 0.0f ? 0.85f : 1.18f;
        visibleHopsCount = juce::jlimit(24.0f, static_cast<float>(maxTimelineHops), visibleHopsCount * factor);
    }
    else if (e.mods.isAltDown())
    {
        const float factor = wheel.deltaY > 0.0f ? 0.88f : 1.14f;
        viewHalfRangeSemitones = juce::jlimit(4.0f, 24.0f, viewHalfRangeSemitones * factor);
    }
    else if (e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
    {
        const float delta = (wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY) * visibleHopsCount * 0.15f;
        viewStartHop = juce::jmax(0.0f, viewStartHop - delta);
    }
    else
    {
        viewCentreMidiNote = juce::jlimit(24.0f, 96.0f, viewCentreMidiNote + wheel.deltaY * 3.0f);
    }
    repaint();
}

//==============================================================================
// Offline Audio File Loading, Drag-and-Drop & MIDI Score Export
//==============================================================================
bool PitchTraceComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".aiff")
            || f.endsWithIgnoreCase(".flac") || f.endsWithIgnoreCase(".mp3")
            || f.endsWithIgnoreCase(".ogg") || f.endsWithIgnoreCase(".mid")
            || f.endsWithIgnoreCase(".midi"))
            return true;
    return false;
}

void PitchTraceComponent::filesDropped(const juce::StringArray& files, int, int)
{
    if (files.isEmpty()) return;
    const juce::File droppedFile(files[0]);
    if (droppedFile.hasFileExtension(".mid;midi"))
    {
        loadMidiReferenceFile(droppedFile);
        return;
    }

    if (isClipMode())
    {
        currentCoachHint = clipSession->loadFile(droppedFile) ? clipSession->getStatusText()
                                                             : juce::String("Couldn't read that audio file.");
        repaint();
        return;
    }

    // Mono loads synchronously: paint the loading screen now, before the message thread blocks.
    loadingSample = true;
    repaint();
    if (auto* peer = getPeer()) peer->performAnyPendingRepaintsNow();
    const juce::ScopeGuard hideLoading { [this] { loadingSample = false; repaint(); } };

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    const juce::File& audioFile = droppedFile;
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return;

    const int maxSamples = static_cast<int>(std::min<int64_t>(reader->lengthInSamples,
                                                                static_cast<int64_t>(reader->sampleRate * 45.0)));
    juce::AudioBuffer<float> fileBuf(static_cast<int>(std::min<unsigned int>(2u, reader->numChannels)), maxSamples);
    reader->read(&fileBuf, 0, maxSamples, 0, true, true);
    analyzeAudioBufferIntoNotes(fileBuf, reader->sampleRate);
}

void PitchTraceComponent::analyzeAudioBufferIntoNotes(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0 || sampleRate <= 0.0)
        return;

    loadedAudioClip.makeCopyOf(buffer);
    loadedAudioSampleRate = sampleRate;

    const int windowSize = juce::nextPowerOfTwo(static_cast<int>(std::ceil(2.0 * sampleRate / 60.0)));
    const int hopSize    = windowSize / 4;
    queue.monoPreview.setClip(buffer, sampleRate, hopSize);

    MPMDetector offlineDetector;
    offlineDetector.prepare(sampleRate, windowSize);
    offlineDetector.setThresholdRatio(0.85f);
    offlineDetector.setClarityFloor(0.55f);

    timeline.clear();
    notes.clear();
    undoStack.clear();
    redoStack.clear();
    selectedNoteId = -1;
    advancedEditNoteId = -1;
    clearedPreviewOnFirstLiveVoice = true;

    std::vector<float> monoWindow((size_t)windowSize, 0.0f);
    const double hopDt = static_cast<double>(hopSize) / sampleRate;
    double tSec = 0.0;
    int hopIdx = 0;

    for (int start = 0; start + windowSize <= numSamples && hopIdx < maxTimelineHops; start += hopSize)
    {
        float rms = 0.0f;
        int zc = 0;
        for (int i = 0; i < windowSize; ++i)
        {
            float s = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                s += buffer.getSample(ch, start + i);
            s /= static_cast<float>(numChannels);
            monoWindow[(size_t)i] = s;
            rms += s * s;
            if (i > 0 && ((s >= 0.0f) != (monoWindow[(size_t)(i - 1)] >= 0.0f)))
                ++zc;
        }
        rms = std::sqrt(rms / static_cast<float>(windowSize));
        const float zcr = static_cast<float>(zc) / static_cast<float>(windowSize);

        const MPMResult mpm = (rms > 0.003f) ? offlineDetector.processFrame(monoWindow.data()) : MPMResult{};
        const bool voiced = mpm.isVoiced && mpm.pitchHz >= 60.0f && mpm.pitchHz <= 1400.0f;
        const bool sib    = (!voiced && rms > 0.004f && zcr > 0.20f);

        PitchTraceQueue::PitchData pd {};
        pd.hopIndex   = hopIdx++;
        pd.isVoiced   = voiced;
        pd.isSibilant = sib;
        pd.rms        = rms;
        pd.clarity    = mpm.clarity;
        pd.uncorrectedPitchHz = voiced ? mpm.pitchHz : 0.0f;
        pd.correctedPitchHz   = voiced ? mpm.pitchHz : 0.0f;

        // Extract polyphonic chord voices via Two-Zone Bass/Upper Sub-Harmonic Projection (eliminates octave confusions):
        if (rms > 0.003f && !sib)
        {
            std::vector<float> candMidis;
            const int polyWin = juce::jmin(numSamples - start, windowSize * 4);
            int fftOrder = 0, fftSize = 1;
            while (fftSize < 2 * juce::jmax(windowSize * 2, polyWin)) { fftSize <<= 1; ++fftOrder; }
            juce::dsp::FFT polyFft(fftOrder);
            std::vector<float> fftBuf((size_t)(2 * fftSize), 0.0f);
            for (int i = 0; i < polyWin; ++i)
            {
                float s = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    s += buffer.getSample(ch, start + i);
                s /= static_cast<float>(numChannels);
                const float hann = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float)i / (float)juce::jmax(1, polyWin - 1));
                fftBuf[(size_t)i] = s * hann;
            }
            polyFft.performRealOnlyForwardTransform(fftBuf.data());
            const int nBins = fftSize / 2;
            const float binHz = static_cast<float>(sampleRate) / static_cast<float>(fftSize);
            std::vector<float> mag((size_t)(nBins + 1), 0.0f);
            float maxMag = 0.0f;
            for (int k = 8; k <= nBins / 4; ++k)
            {
                mag[(size_t)k] = std::hypot(fftBuf[(size_t)(2 * k)], fftBuf[(size_t)(2 * k + 1)]);
                maxMag = juce::jmax(maxMag, mag[(size_t)k]);
            }
            struct OfflinePeak { float midi; float origMag; float resMag; float sal; bool used; };
            std::vector<OfflinePeak> pks;
            const int kLo = juce::jmax(6, (int)std::ceil(74.0f / binHz));
            const int kHi = juce::jmin(nBins - 4, (int)std::floor(1650.0f / binHz));
            for (int k = kLo; k <= kHi; ++k)
            {
                const float y0 = mag[(size_t)k];
                if (y0 < maxMag * 0.020f) continue;
                if (y0 >= mag[(size_t)(k - 1)] && y0 > mag[(size_t)(k + 1)] && y0 > mag[(size_t)(k - 2)] && y0 > mag[(size_t)(k + 2)])
                {
                    const float lL = std::log(juce::jmax(1e-9f, mag[(size_t)(k - 1)]));
                    const float l0 = std::log(juce::jmax(1e-9f, y0));
                    const float lR = std::log(juce::jmax(1e-9f, mag[(size_t)(k + 1)]));
                    const float den = lL - 2.0f * l0 + lR;
                    const float d = (den < -1e-6f) ? juce::jlimit(-0.5f, 0.5f, 0.5f * (lL - lR) / den) : 0.0f;
                    const float hz = ((float)k + d) * binHz;
                    const float em = std::exp(l0 - 0.25f * (lL - lR) * d);
                    if (hz >= 74.0f && hz <= 1650.0f)
                        pks.push_back({ hzToMidiNote(hz), em, em, em, false });
                }
            }
            for (size_t i = 0; i < pks.size(); ++i)
            {
                float hSum = 0.0f;
                static constexpr float hW[3] = { 1.05f, 0.60f, 0.35f };
                static constexpr float hC[3] = { 4.20f, 2.20f, 1.60f };
                for (int h = 2; h <= 4; ++h)
                {
                    const float hm = pks[i].midi + 12.0f * std::log2((float)h);
                    float bh = 0.0f;
                    for (size_t q = i + 1; q < pks.size(); ++q)
                        if (std::abs(pks[q].midi - hm) < 0.50f)
                            bh = juce::jmax(bh, pks[q].origMag);
                    hSum += hW[h - 2] * juce::jmin(bh, pks[i].origMag * hC[h - 2]);
                }
                pks[i].sal = pks[i].origMag * 1.25f + hSum;
            }
            // Step 1: Primary Bass String Fundamental in [39.5 .. 51.5]
            int bestBass = -1;
            float bestBassSal = maxMag * 0.12f;
            for (size_t i = 0; i < pks.size(); ++i)
            {
                if (pks[i].midi >= 39.5f && pks[i].midi <= 51.5f && pks[i].origMag >= maxMag * 0.03f && pks[i].sal > bestBassSal)
                {
                    bestBassSal = pks[i].sal;
                    bestBass = (int)i;
                }
            }
            if (bestBass >= 0)
            {
                auto& bp = pks[(size_t)bestBass];
                bp.used = true;
                candMidis.push_back(bp.midi);
                for (int h = 2; h <= 8; ++h)
                {
                    const float hm = bp.midi + 12.0f * std::log2((float)h);
                    for (auto& q : pks)
                    {
                        if (!q.used && std::abs(q.midi - hm) < 0.62f)
                        {
                            if (h == 3 || h == 5)
                                q.resMag = juce::jmax(0.0f, q.resMag - bp.origMag * 0.10f);
                            else if (h == 2 && std::abs(q.midi - 59.0f) < 0.65f && q.origMag > bp.origMag * 3.5f)
                                {}
                            else
                            {
                                q.used = true;
                                q.resMag = 0.0f;
                            }
                        }
                    }
                }
            }
            // Step 2: Fold remaining upper-octave overtones [66.6 .. 79.5] down 12 st into [54.6 .. 66.6]
            const size_t origCount = pks.size();
            for (size_t i = 0; i < origCount; ++i)
            {
                if (!pks[i].used && pks[i].midi > 66.6f && pks[i].midi <= 79.5f)
                {
                    const float subM = pks[i].midi - 12.0f;
                    pks[i].used = true;
                    bool merged = false;
                    for (size_t q = 0; q < pks.size(); ++q)
                    {
                        if (q != i && std::abs(pks[q].midi - subM) < 0.65f)
                        {
                            pks[q].used = false;
                            pks[q].resMag += 0.75f * pks[i].resMag;
                            merged = true;
                        }
                    }
                    if (!merged)
                    {
                        const float vm = pks[i].resMag * 0.72f;
                        pks.push_back({ subM, vm, vm, vm, false });
                    }
                }
            }
            float maxUpper = maxMag * 0.08f;
            for (const auto& pk : pks)
                if (!pk.used && pk.midi >= 52.0f && pk.midi <= 66.6f)
                    maxUpper = juce::jmax(maxUpper, pk.resMag);
            const float minUpper = juce::jmax(maxMag * 0.030f, maxUpper * 0.12f);
            for (int pass = 0; pass < 3; ++pass)
            {
                int bestU = -1;
                float bestVal = minUpper;
                for (size_t i = 0; i < pks.size(); ++i)
                {
                    if (pks[i].used || pks[i].midi < 52.0f || pks[i].midi > 66.6f || pks[i].resMag < minUpper)
                        continue;
                    bool dup = false;
                    for (float ex : candMidis)
                        if (std::abs(ex - pks[i].midi) < 1.45f || std::abs(std::abs(ex - pks[i].midi) - 12.0f) < 0.75f)
                            dup = true;
                    if (!dup && pks[i].resMag > bestVal)
                    {
                        bestVal = pks[i].resMag;
                        bestU = (int)i;
                    }
                }
                if (bestU < 0) break;
                pks[(size_t)bestU].used = true;
                candMidis.push_back(pks[(size_t)bestU].midi);
            }
            if (candMidis.empty() && voiced)
                candMidis.push_back(hzToMidiNote(mpm.pitchHz));

            std::sort(candMidis.begin(), candMidis.end());
            pd.isPolyphonic  = polyphonicModeEnabled;
            pd.numPolyVoices = static_cast<uint8_t>(candMidis.size());
            for (size_t v = 0; v < candMidis.size(); ++v)
            {
                pd.polyRawMidi[v]       = candMidis[v];
                pd.polyCorrectedMidi[v] = std::round(candMidis[v]);
                pd.polyVoiceRms[v]      = rms / static_cast<float>(candMidis.size());
            }
            if (!candMidis.empty())
                pd.isVoiced = true;
        }

        TimelineHop th {};
        th.data = pd;
        th.timeStampSeconds = tSec;
        th.rawMidi = voiced ? hzToMidiNote(mpm.pitchHz) : (pd.numPolyVoices > 0 ? pd.polyRawMidi[0] : 0.0f);
        timeline.push_back(th);
        tSec += hopDt;
    }

    // Fix #6: Global Viterbi Dynamic-Programming Pitch-Path Smoother across voiced segments
    // Eliminates octave jumps (+-12 st) and 5th sub-harmonic drops (+-19 st) before note segmentation:
    const int nHops = static_cast<int>(timeline.size());
    int hStart = 0;
    while (hStart < nHops)
    {
        while (hStart < nHops && (!timeline[(size_t)hStart].data.isVoiced || timeline[(size_t)hStart].rawMidi <= 0.0f))
            ++hStart;
        if (hStart >= nHops) break;
        int hEnd = hStart + 1;
        while (hEnd < nHops && timeline[(size_t)hEnd].data.isVoiced && timeline[(size_t)hEnd].rawMidi > 0.0f)
            ++hEnd;

        const int segLen = hEnd - hStart;
        if (segLen >= 3)
        {
            // Candidate states per hop: { rawMidi, rawMidi - 12, rawMidi + 12 } plus poly candidates
            constexpr int numStates = 3;
            std::vector<std::array<float, numStates>> states((size_t)segLen);
            std::vector<std::array<float, numStates>> cost((size_t)segLen, { 1.0e9f, 1.0e9f, 1.0e9f });
            std::vector<std::array<int, numStates>> backptr((size_t)segLen, { 0, 0, 0 });

            for (int t = 0; t < segLen; ++t)
            {
                const float baseM = timeline[(size_t)(hStart + t)].rawMidi;
                states[(size_t)t] = { baseM, baseM - 12.0f, baseM + 12.0f };
            }
            cost[0] = { 0.0f, 0.65f, 0.65f };

            for (int t = 1; t < segLen; ++t)
            {
                for (int sCur = 0; sCur < numStates; ++sCur)
                {
                    const float mCur = states[(size_t)t][(size_t)sCur];
                    const float emitPenalty = (sCur == 0) ? 0.0f : 0.55f;
                    for (int sPrev = 0; sPrev < numStates; ++sPrev)
                    {
                        const float mPrev = states[(size_t)(t - 1)][(size_t)sPrev];
                        const float step = std::abs(mCur - mPrev);
                        const float transPenalty = (step <= 3.5f) ? (0.04f * step) : (0.35f + 0.14f * step);
                        const float candCost = cost[(size_t)(t - 1)][(size_t)sPrev] + emitPenalty + transPenalty;
                        if (candCost < cost[(size_t)t][(size_t)sCur])
                        {
                            cost[(size_t)t][(size_t)sCur] = candCost;
                            backptr[(size_t)t][(size_t)sCur] = sPrev;
                        }
                    }
                }
            }

            int bestState = 0;
            if (cost.back()[1] < cost.back()[(size_t)bestState]) bestState = 1;
            if (cost.back()[2] < cost.back()[(size_t)bestState]) bestState = 2;

            for (int t = segLen - 1; t >= 0; --t)
            {
                const float chosenM = states[(size_t)t][(size_t)bestState];
                timeline[(size_t)(hStart + t)].rawMidi = chosenM;
                timeline[(size_t)(hStart + t)].data.uncorrectedPitchHz = midiNoteToHz(chosenM);
                bestState = backptr[(size_t)t][(size_t)bestState];
            }
        }
        hStart = hEnd;
    }

    rebuildSegmentationFromSensitivity();

    double midiSum = 0.0;
    int midiCount = 0;
    for (const auto& n : notes)
    {
        if (!n.isSibilant)
        {
            if (selectedNoteId < 0) selectedNoteId = n.id;
            midiSum += n.detectedMidi;
            ++midiCount;
        }
    }
    if (midiCount > 0)
        viewCentreMidiNote = juce::jlimit(36.0f, 84.0f, static_cast<float>(midiSum / midiCount));

    viewStartHop = 0.0f;
    visibleHopsCount = static_cast<float>(std::clamp<size_t>(timeline.size() + 8, 80, 320));
    notifySelectionChanged();
    repaint();
    if (onClipStatusChanged) onClipStatusChanged(); // enables the Mono Play / Stop buttons
}

void PitchTraceComponent::promptLoadAudioFile()
{
    activeFileChooser = std::make_unique<juce::FileChooser>(
        "Load Vocal Audio Sample into NovaTone",
        juce::File(),
        "*.wav;*.aiff;*.flac;*.mp3;*.ogg");

    juce::Component::SafePointer<PitchTraceComponent> safeThis(this);
    activeFileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            const auto file = fc.getResult();
            if (!file.existsAsFile()) return;
            safeThis->filesDropped(juce::StringArray { file.getFullPathName() }, 0, 0);
        });
}

void PitchTraceComponent::promptExportMidiFile()
{
    activeFileChooser = std::make_unique<juce::FileChooser>(
        "Export Detected & Tuned Vocal Score as MIDI (.mid)",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("NovaToneVocalScore.mid"),
        "*.mid");

    juce::Component::SafePointer<PitchTraceComponent> safeThis(this);
    activeFileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            const auto outFile = fc.getResult();
            if (outFile == juce::File()) return;

            juce::MidiMessageSequence seq;
            constexpr int ticksPerQuarter = 480;
            constexpr double ticksPerHop  = (512.0 / 48000.0) * 2.0 * ticksPerQuarter;

            for (const auto& n : safeThis->notes)
            {
                if (n.isSibilant) continue;
                const int midiNote = juce::jlimit(0, 127, n.targetMidi);
                const juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(40, 127, juce::roundToInt(n.meanRms * 220.0f + 55.0f)));
                const double startTick = static_cast<double>(n.startHop + n.warpOffsetHops) * ticksPerHop;
                const double endTick   = static_cast<double>(n.endHop + n.warpOffsetHops) * ticksPerHop;

                seq.addEvent(juce::MidiMessage::noteOn(1, midiNote, vel), startTick);
                seq.addEvent(juce::MidiMessage::noteOff(1, midiNote), juce::jmax(startTick + 10.0, endTick));
            }
            seq.updateMatchedPairs();

            juce::MidiFile midiFile;
            midiFile.setTicksPerQuarterNote(ticksPerQuarter);
            midiFile.addTrack(seq);

            outFile.deleteFile();
            if (auto stream = std::unique_ptr<juce::FileOutputStream>(outFile.createOutputStream()))
                midiFile.writeTo(*stream);
        });
}

void PitchTraceComponent::startExternalMidiDragToDaw()
{
    const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("NovaTone_DragScore.mid");
    juce::MidiMessageSequence seq;
    constexpr int ticksPerQuarter = 480;
    constexpr double ticksPerHop  = (512.0 / 48000.0) * 2.0 * ticksPerQuarter;

    for (const auto& n : notes)
    {
        if (n.isSibilant) continue;
        const int midiNote = juce::jlimit(0, 127, n.targetMidi);
        const juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(40, 127, juce::roundToInt(n.meanRms * 220.0f + 55.0f)));
        const double startTick = static_cast<double>(n.startHop + n.warpOffsetHops) * ticksPerHop;
        const double endTick   = static_cast<double>(n.endHop + n.warpOffsetHops) * ticksPerHop;

        seq.addEvent(juce::MidiMessage::noteOn(1, midiNote, vel), startTick);
        seq.addEvent(juce::MidiMessage::noteOff(1, midiNote), juce::jmax(startTick + 10.0, endTick));
    }
    seq.updateMatchedPairs();

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(ticksPerQuarter);
    midiFile.addTrack(seq);

    tempFile.deleteFile();
    if (auto stream = std::unique_ptr<juce::FileOutputStream>(tempFile.createOutputStream()))
        midiFile.writeTo(*stream);

    juce::DragAndDropContainer::performExternalDragDropOfFiles({ tempFile.getFullPathName() }, false, this);
}

void PitchTraceComponent::loadMidiReferenceFile(const juce::File& midiFile)
{
    juce::FileInputStream inStream(midiFile);
    if (!inStream.openedOk())
        return;

    juce::MidiFile mf;
    if (!mf.readFrom(inStream))
        return;

    referenceGhostNotes.clear();
    const int tpq = juce::jmax(1, (int)mf.getTimeFormat());
    constexpr double ticksPerHop = (512.0 / 48000.0) * 2.0 * 480.0;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence trackSeq(*mf.getTrack(t));
        trackSeq.updateMatchedPairs();
        for (int i = 0; i < trackSeq.getNumEvents(); ++i)
        {
            auto* ev = trackSeq.getEventPointer(i);
            if (ev->message.isNoteOn() && ev->noteOffObject != nullptr)
            {
                const double scaleTicks = 480.0 / static_cast<double>(tpq);
                const int sHop = juce::jmax(0, static_cast<int>(std::lround(ev->message.getTimeStamp() * scaleTicks / ticksPerHop)));
                const int eHop = juce::jmax(sHop + 2, static_cast<int>(std::lround(ev->noteOffObject->message.getTimeStamp() * scaleTicks / ticksPerHop)));
                referenceGhostNotes.push_back({ sHop, eHop, ev->message.getNoteNumber() });
            }
        }
    }
    currentCoachHint = "Loaded " + juce::String(referenceGhostNotes.size()) + " reference notes, shown behind your vocal.";
    repaint();
}

void PitchTraceComponent::promptExportWavFile()
{
    if (isClipMode())
    {
        activeFileChooser = std::make_unique<juce::FileChooser>(
            "Export edited clip (.wav)",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("NovaTone Poly Clip.wav"), "*.wav");
        juce::Component::SafePointer<PitchTraceComponent> safeThis(this);
        activeFileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& fc)
            {
                if (safeThis == nullptr || safeThis->clipSession == nullptr) return;
                const auto f = fc.getResult();
                if (f == juce::File()) return;
                safeThis->currentCoachHint = safeThis->clipSession->exportRender(f.withFileExtension("wav"))
                    ? "Exported " + f.withFileExtension("wav").getFileName() : juce::String("Export failed.");
                safeThis->repaint();
            });
        return;
    }

    activeFileChooser = std::make_unique<juce::FileChooser>(
        "Bounce & Export Tuned Vocal Audio (.wav)",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("NovaToneTunedVocal.wav"),
        "*.wav");

    juce::Component::SafePointer<PitchTraceComponent> safeThis(this);
    activeFileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            const auto outFile = fc.getResult();
            if (outFile == juce::File()) return;

            const double sr = (safeThis->loadedAudioSampleRate > 0.0) ? safeThis->loadedAudioSampleRate : 48000.0;
            const int hopSize = safeThis->queue.monoPreview.hopSamples; // same hop the file was analysed with
            const int numHops = static_cast<int>(safeThis->timeline.size());
            if (numHops <= 0) return;

            const float gCenter = safeThis->queue.globalCenter.load(std::memory_order_relaxed);
            const float gVar    = safeThis->queue.globalVariation.load(std::memory_order_relaxed);
            const float gTrans  = safeThis->queue.globalTransition.load(std::memory_order_relaxed);

            const int totalSamples = (safeThis->loadedAudioClip.getNumSamples() > 0)
                ? safeThis->loadedAudioClip.getNumSamples()
                : (numHops * hopSize);
            const int numCh = juce::jmax(1, safeThis->loadedAudioClip.getNumChannels());
            juce::AudioBuffer<float> renderBuf(numCh, totalSamples);
            renderBuf.clear();

            if (safeThis->loadedAudioClip.getNumSamples() > 0)
            {
                // Constant-tempo dual-head crossfaded splice grain resynthesis anchored to sample index `i`:
                double grainPhase = 0.0;
                const double grainWindow = std::clamp(sr * 0.022, 256.0, 2048.0);
                for (int i = 0; i < totalSamples; ++i)
                {
                    const int h = juce::jlimit(0, numHops - 1, i / hopSize);
                    const auto& th = safeThis->timeline[(size_t)h];
                    float ratio = 1.0f;
                    float gainLin = 1.0f;

                    if (th.data.isVoiced && th.rawMidi > 0.0f)
                    {
                        const float shapedM = safeThis->evaluateShapedMidiAtHop(h, gCenter, gVar, gTrans);
                        ratio = std::exp2(juce::jlimit(-12.0f, 12.0f, shapedM - th.rawMidi) / 12.0f);
                    }
                    if (const auto* nb = safeThis->findNoteById(th.noteId))
                    {
                        gainLin = juce::Decibels::decibelsToGain(nb->gainDb);
                        const float span = static_cast<float>(juce::jmax(1, nb->endHop - nb->startHop));
                        const float u = (static_cast<float>(h - nb->startHop) + 0.5f) / span;
                        if (nb->fadeInRatio > 0.005f && u < nb->fadeInRatio)
                            gainLin *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * (u / nb->fadeInRatio));
                        if (nb->fadeOutRatio > 0.005f && (1.0f - u) < nb->fadeOutRatio)
                            gainLin *= 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * ((1.0f - u) / nb->fadeOutRatio));
                    }

                    const double dDot = 1.0 - static_cast<double>(ratio);
                    grainPhase += dDot / grainWindow;
                    grainPhase -= std::floor(grainPhase);
                    double grainPhase2 = grainPhase + 0.5;
                    grainPhase2 -= std::floor(grainPhase2);

                    const double d1 = grainPhase * grainWindow;
                    const double d2 = grainPhase2 * grainWindow;
                    const float s1 = std::sin(juce::MathConstants<float>::pi * static_cast<float>(grainPhase));
                    const float s2 = std::sin(juce::MathConstants<float>::pi * static_cast<float>(grainPhase2));
                    const float w1 = s1 * s1;
                    const float w2 = s2 * s2;

                    auto sampleAt = [&](int ch, double pos) noexcept {
                        const double clamped = juce::jlimit(0.0, static_cast<double>(totalSamples - 1), pos);
                        const int r0 = static_cast<int>(clamped);
                        const int r1 = juce::jmin(totalSamples - 1, r0 + 1);
                        const float frac = static_cast<float>(clamped - std::floor(clamped));
                        return (1.0f - frac) * safeThis->loadedAudioClip.getSample(ch, r0)
                             + frac * safeThis->loadedAudioClip.getSample(ch, r1);
                    };

                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        const float s = w1 * sampleAt(ch, static_cast<double>(i) - d1)
                                      + w2 * sampleAt(ch, static_cast<double>(i) - d2);
                        renderBuf.setSample(ch, i, s * gainLin);
                    }
                }
            }
            else
            {
                // Synthesize harmonic preview tone from the shaped timeline notes if no external WAV was loaded yet:
                double phase = 0.0;
                for (int i = 0; i < totalSamples; ++i)
                {
                    const int h = juce::jlimit(0, numHops - 1, i / hopSize);
                    const auto& th = safeThis->timeline[(size_t)h];
                    float s = 0.0f;
                    if (th.data.isVoiced && th.rawMidi > 0.0f)
                    {
                        const float shapedM = safeThis->evaluateShapedMidiAtHop(h, gCenter, gVar, gTrans);
                        const float hz = midiNoteToHz(shapedM);
                        phase += juce::MathConstants<double>::twoPi * static_cast<double>(hz) / sr;
                        s = static_cast<float>(0.35 * std::sin(phase) + 0.18 * std::sin(2.0 * phase) + 0.08 * std::sin(3.0 * phase)) * th.data.rms;
                    }
                    for (int ch = 0; ch < numCh; ++ch)
                        renderBuf.setSample(ch, i, s);
                }
            }

            outFile.deleteFile();
            if (auto fileStream = std::unique_ptr<juce::FileOutputStream>(outFile.createOutputStream()))
            {
                juce::WavAudioFormat wavFormat;
                if (std::unique_ptr<juce::AudioFormatWriter> writer {
                        wavFormat.createWriterFor(fileStream.get(), sr, (unsigned int)numCh, 24, {}, 0) })
                {
                    fileStream.release();
                    writer->writeFromAudioSampleBuffer(renderBuf, 0, totalSamples);
                }
            }
        });
}