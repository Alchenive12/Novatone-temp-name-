#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <deque>
#include <functional>
#include <vector>
#include "PitchTraceQueue.h"
#include "HarmonyEngineColours.h"

namespace Poly { class ClipSession; }

/**
    NovaTone Interactive 2D Piano Roll & Pitch/Time Note Graph Editor.

    Includes all 4 Flagship Performance Upgrades:
      1. Clarity-Squared Perceptual Pitch Center + Independent Drift vs. Vibrato + Per-Note Sibilant Balance.
      2. 1-Click AUTO-KEY Chromagram Scale Detector (Ctrl+K) + Melodyne-Style Note LEVELER Macro (Ctrl+L).
      3. 4. DRAW CURVE Freehand Pitch Brush Mode (Ctrl+F / 4) for painting custom pitch contours & vibrato.
      4. Real-Time MIDI Target Note Input & MIDI Ghost Note Reference Overlay on the Piano Roll.
*/
class PitchTraceComponent final : public juce::Component,
                                  public juce::FileDragAndDropTarget,
                                  private juce::Timer
{
public:
    enum class ToolMode
    {
        StandardEdit = 0, ///< 1. Move pitch (semitones/cents), drag note edges, right-click snap.
        CutAndGlue   = 1, ///< 2. Slice notes (click) or join/glue adjacent notes (Ctrl+click).
        AdvancedEdit = 2, ///< 3. Per-note Formant, Variation, Gain (dB), Sibilant Balance, and Ramp-In/Out handles.
        FreehandDraw = 3  ///< 4. Freehand Pitch Curve Brush (Auto-Tune Pro 11 Graph / RePitch 2 Draw style).
    };

    explicit PitchTraceComponent(PitchTraceQueue& queueToDrain);
    ~PitchTraceComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void setFrequencyRange(float minHz, float maxHz) noexcept;

    /** Single-reader latest pitch accessor used by the live Tuner badge. */
    PitchTraceQueue::PitchData getLatestPitch() const noexcept { return latestPitch; }

    //==============================================================================
    // NovaTone Editor Tool, Undo/Redo, Auto-Key, Leveler & Action API
    //==============================================================================
    void setToolMode(ToolMode mode) noexcept;
    ToolMode getToolMode() const noexcept { return currentToolMode; }

    void setSnapToScale(bool shouldSnap) noexcept;
    bool isSnapToScale() const noexcept { return snapToScaleEnabled; }

    void setAutoScroll(bool shouldFollow) noexcept;
    bool isAutoScroll() const noexcept { return autoScrollEnabled; }

    /** Switches between Monophonic (NewTone solo vocal) and Polyphonic (Melodyne chord/choir/string) detection & editing. */
    void setPolyphonicMode(bool polyEnabled);
    bool isPolyphonicMode() const noexcept { return polyphonicModeEnabled; }

    /** Enables Instrument MIDI Clean & Denoise Mode: suppresses pick/fret/drum transients,
        bridges multi-hop dropouts, stitches fragmented same-pitch notes, and quantizes MIDI blocks. */
    void setMidiCleanMode(bool cleanEnabled);
    bool isMidiCleanMode() const noexcept { return midiCleanModeEnabled; }

    void snapAllNotesToScale();
    void resetAllNotes();
    void clearTimeline();

    /** Upgrade #2A: Krumhansl-Kessler 12-Pitch-Class Chromagram Key & Scale Detector (Auto-Key style). */
    bool detectKeyAndScaleFromTimeline(int& outRoot, int& outMode, float& outConfidence) const noexcept;
    void autoDetectKeyAndApply();

    /** Upgrade #2B: Melodyne 5-style Note Leveling Macro — automatically balances quiet & loud notes toward phrase RMS. */
    void levelAllNotesVolume(float levelingAmount0to1 = 0.72f);

    void loadPhrasePreview();          ///< Populates a monophonic vocal phrase (NewTone style).
    void loadPolyphonicChordPreview(); ///< Populates a multi-voice Choir & String chord progression (Melodyne DNA style).
    void promptLoadAudioFile();        ///< Opens an async WAV/AIFF/FLAC file chooser and segments into notes.
    void promptExportMidiFile();       ///< Exports the current vocal note blocks to a standard .mid file.
    void promptExportWavFile();        ///< Renders and exports the tuned vocal audio clip as a .wav file (Ctrl+W).
    void startExternalMidiDragToDaw(); ///< Drag-out tuned MIDI score directly into DAW timeline/Piano Roll.
    void loadMidiReferenceFile(const juce::File& midiFile); ///< Loads a .mid reference melody as Ghost Notes.

    // Undo / Redo support
    void pushUndoSnapshot();
    bool canUndo() const noexcept { return !undoStack.empty(); }
    bool canRedo() const noexcept { return !redoStack.empty(); }
    void undo();
    void redo();

    // Selected-Note & Multi-Select (Ctrl+A) Inspector API
    int getNumNotes() const noexcept { return static_cast<int>(notes.size()); }
    int getSelectedNoteId() const noexcept { return selectedNoteId; }
    int getSelectedNoteCount() const noexcept;
    void selectAllNotes();
    void deselectAllNotes();
    const NewtoneNoteBlock* getSelectedNote() const noexcept;
    void transposeSelectedNote(int semitoneDelta);
    void setSelectedNoteFineCents(float cents);
    void setSelectedNoteFormant(float semitones);
    void setSelectedNoteVariation(float variation0to2);
    void setSelectedNoteGainDb(float gainDb);
    void setSelectedNoteSibilantDb(float sibilantDb);
    void resetSelectedNote();

    /** Fired on the message thread whenever the selected note or its properties change. */
    std::function<void()> onNoteSelectionChanged;

    /** Fired when a keyboard shortcut inside the Piano Roll changes the active ToolMode (Ctrl+T / Ctrl+S / Ctrl+E / Ctrl+F). */
    std::function<void(ToolMode)> onToolModeChanged;

    /** Fired when Auto-Key detects the optimal Root (0..11) and Scale Mode (0=Major, 1=Minor). */
    std::function<void(int root, int mode)> onAutoKeyDetected;

    /** Fired when Ctrl+H is pressed inside the Piano Roll to toggle Humanize mode. */
    std::function<void()> onToggleHumanizeShortcut;

    /** Fired when Ctrl+B is pressed inside the Piano Roll to cycle the lower bar layout. */
    std::function<void()> onCycleDockShortcut;

    /** Fired when Ctrl+P is pressed inside the Piano Roll to toggle Monophonic / Polyphonic (Melodyne) mode. */
    std::function<void()> onTogglePolyphonicShortcut;
    std::function<void()> onToggleMidiCleanShortcut;

    void selectAdjacentNote(int direction);

    //==============================================================================
    // Poly clip (Melodyne-style): in Poly mode the roll shows and edits the session's clip
    void setClipSession(Poly::ClipSession* session);
    bool isClipMode() const noexcept { return polyphonicModeEnabled && clipSession != nullptr; }
    /** Fired when the clip's status changes (recording, analysing, ready...). */
    std::function<void()> onClipStatusChanged;

    static juce::String midiNoteName(int midiNote);

    //==============================================================================
    // Mouse, Keyboard & File Drag-and-Drop Overrides
    //==============================================================================
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    struct ReferenceGhostNote
    {
        int startHop = 0;
        int endHop   = 1;
        int midiNote = 60;
    };

    struct TimelineHop
    {
        PitchTraceQueue::PitchData data;
        double timeStampSeconds = 0.0;
        float  rawMidi          = 0.0f;  ///< Continuous detected MIDI note (0 if unvoiced).
        int    noteId           = -1;    ///< ID of owning NewtoneNoteBlock in Mono mode (-1 if unvoiced).
        bool   hasCustomPitch   = false; ///< True if painted by 4. DRAW CURVE Freehand Brush.
        float  customMidi       = 0.0f;  ///< Custom freehand-drawn MIDI pitch at this hop.
        std::array<int, PitchTraceQueue::maxPolyVoices> polyNoteIds {}; ///< Per-voice note IDs in Polyphonic mode.
    };

    enum class DragAction
    {
        None = 0,
        MoveNotePitch,
        MoveNoteFineCents,
        TrimNoteLeft,
        TrimNoteRight,
        AdjustRampIn,
        AdjustRampOut,
        AdjustFadeIn,
        AdjustFadeOut,
        AdjustNoteFormant,
        AdjustNoteVariation,
        AdjustNoteGain,
        DrawFreehandCurve,
        PanCanvas,
        ScrubRuler
    };

    void timerCallback() override;
    void appendHopToTimeline(const PitchTraceQueue::PitchData& hopData, double timeSec);
    void rebuildSegmentationFromSensitivity();
    void relinkTimelineToNotes() noexcept;
    void syncActiveOverrideToEngine();
    void notifySelectionChanged();
    void analyzeAudioBufferIntoNotes(const juce::AudioBuffer<float>& buffer, double sampleRate);
    void paintFreehandStrokeBetween(juce::Point<float> p0, juce::Point<float> p1, juce::Rectangle<float> grid);

    void importClipFromSession();
    void handleClipSessionChanged();
    void pushNotesToClipSession();
    int  playheadHopForPaint() const noexcept;

    static float hzToMidiNote(float hz) noexcept;
    static float midiNoteToHz(float midi) noexcept;
    static bool isBlackKey(int midiNote) noexcept;
    bool isMidiNoteInScale(int midiNote, uint16_t scaleMask) const noexcept;
    int  snapMidiToScale(float midiNote, uint16_t scaleMask) const noexcept;

    // Coordinate conversions inside the grid canvas area:
    juce::Rectangle<float> getGridArea() const noexcept;
    juce::Rectangle<float> getPianoRulerArea() const noexcept;
    juce::Rectangle<float> getTimeRulerArea() const noexcept;
    juce::Rectangle<float> getCoachBarArea() const noexcept;

    float midiNoteToY(float midiNote) const noexcept;
    float yToMidiNote(float y) const noexcept;
    float hopToX(float hopIndex, juce::Rectangle<float> grid) const noexcept;
    float xToHop(float x, juce::Rectangle<float> grid) const noexcept;
    juce::Rectangle<float> getNoteRect(const NewtoneNoteBlock& note, juce::Rectangle<float> grid) const noexcept;

    float evaluateShapedMidiAtHop(int hopIdx, float globalCenter, float globalVariation, float globalTransition) const noexcept;
    float evaluatePolyShapedMidiAtHop(int hopIdx, int voiceSlot, float globalCenter, float globalVariation, float globalTransition) const noexcept;
    int   findNoteIndexAtPoint(juce::Point<float> pt, juce::Rectangle<float> grid) const noexcept;
    NewtoneNoteBlock* findNoteById(int id) noexcept;
    const NewtoneNoteBlock* findNoteById(int id) const noexcept;

    void splitNoteAtHop(int noteIndex, int splitHop);
    void joinNoteWithNext(int noteIndex);

    void paintPianoRollRuler(juce::Graphics& g, juce::Rectangle<float> area, uint16_t scaleMask);
    void paintTimeRuler(juce::Graphics& g, juce::Rectangle<float> area, juce::Rectangle<float> grid);
    void paintGridAndNotes(juce::Graphics& g, juce::Rectangle<float> grid, uint16_t scaleMask);
    void paintCoachBar(juce::Graphics& g, juce::Rectangle<float> area);

    PitchTraceQueue& queue;
    std::vector<PitchTraceQueue::PitchData> popScratch;
    PitchTraceQueue::PitchData latestPitch;

    std::vector<TimelineHop> timeline;
    std::vector<NewtoneNoteBlock> notes;
    std::vector<ReferenceGhostNote> referenceGhostNotes;
    std::vector<std::vector<NewtoneNoteBlock>> undoStack;
    std::vector<std::vector<NewtoneNoteBlock>> redoStack;
    int nextNoteId = 1;

    static constexpr int   maxTimelineHops  = 4096; // ~45 seconds at 11 ms hop
    static constexpr size_t maxUndoSteps    = 32;
    static constexpr float pianoRulerWidth  = 48.0f;
    static constexpr float timeRulerHeight  = 24.0f;
    static constexpr float coachBarHeight   = 24.0f;

    // Viewport state (vertical MIDI range and horizontal hop window):
    float viewCentreMidiNote     = 60.0f; // C4
    float viewHalfRangeSemitones = 10.0f; // 20 semitones visible by default
    float viewStartHop           = 0.0f;
    float visibleHopsCount       = 180.0f;

    ToolMode   currentToolMode       = ToolMode::StandardEdit;
    DragAction currentDragAction     = DragAction::None;
    bool       snapToScaleEnabled    = true;
    bool       autoScrollEnabled     = true;
    bool       polyphonicModeEnabled = false;
    bool       midiCleanModeEnabled  = false;

    int   selectedNoteId     = -1;
    int   hoveredNoteId      = -1;
    int   advancedEditNoteId = -1;
    juce::String currentCoachHint;

    // Drag-start snapshot (supports single note, Ctrl+A multi-note group drag, and Freehand Draw):
    std::vector<NewtoneNoteBlock> dragStartNotesSnapshot;
    juce::Point<float> dragStartPos {};
    juce::Point<float> lastDrawPos  {};
    int   dragStartTargetMidi = 60;
    float dragStartFineCents  = 0.0f;
    int   dragStartHopLeft    = 0;
    int   dragStartHopRight   = 1;
    float dragStartVariation  = 1.0f;
    float dragStartFormant    = 0.0f;
    float dragStartGainDb     = 0.0f;
    float dragStartRampIn     = 0.18f;
    float dragStartRampOut    = 0.18f;
    float dragStartFadeIn     = 0.0f;
    float dragStartFadeOut    = 0.0f;
    float dragStartViewHop    = 0.0f;
    float dragStartViewMidi   = 60.0f;

    float lastAppliedSensitivity = 0.65f;
    uint16_t lastAppliedScaleMask = 0x0AB5;

    double lastPopTimeSeconds = 0.0;
    bool   hasLastPopTime     = false;
    bool   clearedPreviewOnFirstLiveVoice = false;

    Poly::ClipSession* clipSession = nullptr;
    const void* lastImportedAnalysis = nullptr;
    int  clipHopSize  = 512;
    int  clipPlayHop  = -1;
    int  lastClipStatus = -1;

    juce::AudioBuffer<float> loadedAudioClip;
    double loadedAudioSampleRate = 48000.0;
    std::unique_ptr<juce::FileChooser> activeFileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchTraceComponent)
};