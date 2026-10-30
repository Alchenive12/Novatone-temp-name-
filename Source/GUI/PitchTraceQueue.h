#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/**
    Represents one segmented vocal or instrument note block in the NovaTone Piano Roll editor.
    Supports both Monophonic (NewTone single-voice) and Polyphonic (Melodyne DNA multi-voice
    chord / choir / string) segmentation with no 4-note limit (up to 16 simultaneous voices),
    plus Melodyne 5-style per-note Sibilant Balance, curved Fade-In/Out, and Freehand Curve overrides.
*/
struct NewtoneNoteBlock
{
    int   id               = 0;      ///< Unique note identifier.
    int   sourceId         = -1;     ///< Poly clip: analysed note this block (or split piece) came from.
    int   voiceLane        = 0;      ///< 0 in Mono mode; 0..15 in Polyphonic Melodyne mode.
    int   startHop         = 0;      ///< Inclusive start hop index on the timeline.
    int   endHop           = 1;      ///< Exclusive end hop index on the timeline.
    int   warpOffsetHops   = 0;      ///< Horizontal time-warp shift in hops.

    float detectedMidi     = 60.0f;  ///< Clarity-weighted perceptual MIDI pitch center of raw detection.
    int   nearestMidi      = 60;     ///< Nearest integer semitone of raw detection (ghost block).
    int   targetMidi       = 60;     ///< Quantized or user-dragged integer MIDI note.
    float fineCents        = 0.0f;   ///< Fine pitch offset in cents [-100, +100] (Alt + drag).

    // Per-note Advanced Edit parameters (Note Shaper Mode):
    float noteCenter       = 1.0f;   ///< Per-note Center multiplier [0..1], default 1.0.
    float noteVariation    = 1.0f;   ///< Per-note Pitch Variation / Vibrato scaling [0..2], default 1.0.
    float formantSemitones = 0.0f;   ///< Per-note Formant shift in semitones [-12..+12].
    float gainDb           = 0.0f;   ///< Per-note Volume trim in dB [-18..+12].
    float sibilantDb       = 0.0f;   ///< Per-note Sibilant/Breath Balance in dB [-18..+12] (Melodyne 5 style).
    float rampInRatio      = 0.18f;  ///< Entry pitch transition ramp fraction of note width [0.02..0.48].
    float rampOutRatio     = 0.18f;  ///< Exit pitch transition ramp fraction of note width [0.02..0.48].
    float fadeInRatio      = 0.0f;   ///< Per-note amplitude Fade-In fraction [0..0.45].
    float fadeOutRatio     = 0.0f;   ///< Per-note amplitude Fade-Out fraction [0..0.45].

    float meanRms          = 0.05f;  ///< Mean RMS amplitude of the note (for blob rendering & Leveler macro).
    bool  isSibilant       = false;  ///< True if this block represents a protected unvoiced consonant.
    bool  userEdited       = false;  ///< True once manually edited by the user in the Piano Roll.
    bool  selected         = false;  ///< GUI selection state (Ctrl+A / Ctrl+Click).

    float effectiveTargetMidi() const noexcept
    {
        return static_cast<float>(targetMidi) + 0.01f * fineCents;
    }
};

/**
    Lock-free command packet sent from the Piano Roll GUI (message thread)
    to TuneEngine (audio thread) when the user edits notes or transport state.
*/
struct NewtoneEditCommand
{
    enum Type
    {
        None = 0,
        SetNotePitch,
        SetNoteAdvanced,
        SetNoteBounds,
        SplitNote,
        JoinNotes,
        ResetNote,
        SnapAllToScale,
        ClearTimeline,
        SetPlayheadHop
    };

    Type     type             = None;
    int      noteId           = -1;
    int      secondNoteId     = -1;
    int      targetMidi       = 60;
    int      startHop         = 0;
    int      endHop           = 1;
    int      splitHop         = 0;
    uint16_t scaleMask        = 0x0FFF;
    float    fineCents        = 0.0f;
    float    noteVariation    = 1.0f;
    float    formantSemitones = 0.0f;
    float    gainDb           = 0.0f;
    float    sibilantDb       = 0.0f;
    float    rampInRatio      = 0.18f;
    float    rampOutRatio     = 0.18f;
};

/**
    Single-producer / single-consumer lock-free bridge between TuneEngine (audio thread)
    and PitchTraceComponent / TuneTab (message thread), supporting both Monophonic
    (NewTone) and Multi-Voice Polyphonic (Melodyne Chord/Choir/String) operation,
    plus live MIDI reference ghost notes and freehand pitch curve overrides.
*/
class PitchTraceQueue
{
public:
    static constexpr int maxPolyVoices = 16;

    struct PitchData
    {
        float   uncorrectedPitchHz = 0.0f;
        float   correctedPitchHz   = 0.0f;
        bool    isVoiced           = false;
        float   rms                = 0.0f;   ///< Frame RMS amplitude for waveform blobs.
        float   clarity            = 0.0f;   ///< MPM NSDF clarity [0..1].
        bool    isSibilant         = false;  ///< True on high-ZCR unvoiced consonants (protected).
        int     hopIndex           = 0;      ///< Monotonic or timeline-synced hop index.
        int     noteId             = -1;     ///< ID of the active NewtoneNoteBlock (-1 if none).
        double  ppqPosition        = 0.0;    ///< Host DAW PPQ position when available.
        double  bpm                = 120.0;  ///< Host DAW tempo in BPM.

        // Live MIDI Target / Reference Ghost Note state at this hop:
        int      activeMidiInputNote = -1;   ///< Held MIDI note number (0..127), or -1 if none.
        uint16_t midiInputMask       = 0;    ///< 12-bit pitch-class mask of held MIDI notes.

        // Polyphonic (Melodyne-style Chord / Choir / String) per-hop detection:
        bool    isPolyphonic       = false;
        uint8_t numPolyVoices      = 0;
        std::array<float, maxPolyVoices> polyRawMidi       {};
        std::array<float, maxPolyVoices> polyCorrectedMidi {};
        std::array<float, maxPolyVoices> polyVoiceRms      {};
    };

    static constexpr int capacity = 4096;
    static constexpr int cmdCapacity = 256;

    PitchTraceQueue() : fifo(capacity), cmdFifo(cmdCapacity) {}

    //==============================================================================
    // Audio Thread -> GUI: Pitch & Envelope Hop Stream
    //==============================================================================
    void push(PitchData data) noexcept
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0) buffer[(size_t)start1] = data;
        else if (size2 > 0) buffer[(size_t)start2] = data;

        fifo.finishedWrite(size1 + size2);
    }

    void popAll(std::vector<PitchData>& outputBuffer)
    {
        outputBuffer.clear();

        const int numReady = fifo.getNumReady();
        if (numReady <= 0) return;

        int start1, size1, start2, size2;
        fifo.prepareToRead(numReady, start1, size1, start2, size2);

        outputBuffer.reserve((size_t)(size1 + size2));

        for (int i = 0; i < size1; ++i) outputBuffer.push_back(buffer[(size_t)(start1 + i)]);
        for (int i = 0; i < size2; ++i) outputBuffer.push_back(buffer[(size_t)(start2 + i)]);

        fifo.finishedRead(size1 + size2);
    }

    //==============================================================================
    // GUI -> Audio Thread: Lock-Free Note Edit Commands
    //==============================================================================
    void pushCommand(const NewtoneEditCommand& cmd) noexcept
    {
        int start1, size1, start2, size2;
        cmdFifo.prepareToWrite(1, start1, size1, start2, size2);

        if (size1 > 0) cmdBuffer[(size_t)start1] = cmd;
        else if (size2 > 0) cmdBuffer[(size_t)start2] = cmd;

        cmdFifo.finishedWrite(size1 + size2);
    }

    bool popCommand(NewtoneEditCommand& outCmd) noexcept
    {
        if (cmdFifo.getNumReady() <= 0)
            return false;

        int start1, size1, start2, size2;
        cmdFifo.prepareToRead(1, start1, size1, start2, size2);

        if (size1 > 0) outCmd = cmdBuffer[(size_t)start1];
        else if (size2 > 0) outCmd = cmdBuffer[(size_t)start2];

        cmdFifo.finishedRead(1);
        return true;
    }

    //==============================================================================
    // Real-Time Active Note Override (Monophonic + Multi-Voice Polyphonic Chord Overrides)
    //==============================================================================
    struct LiveNoteOverride
    {
        std::atomic<bool>  active           { false };
        std::atomic<int>   noteId           { -1 };
        std::atomic<float> detectedMidiRef  { 60.0f }; ///< Reference pitch of the note being overridden.
        std::atomic<float> targetMidiFloat  { 60.0f };
        std::atomic<float> noteCenter       { 1.0f };
        std::atomic<float> noteVariation    { 1.0f };
        std::atomic<float> formantSemitones { 0.0f };
        std::atomic<float> gainDb           { 0.0f };
        std::atomic<float> sibilantDb       { 0.0f };  ///< Per-note Sibilant/Breath Balance in dB [-18..+12].
        std::atomic<float> rampInRatio      { 0.18f };
        std::atomic<float> rampOutRatio     { 0.18f };
        std::atomic<float> fadeInRatio      { 0.0f };
        std::atomic<float> fadeOutRatio     { 0.0f };
        std::atomic<float> fadeMultiplier   { 1.0f };  ///< Instantaneous curved fade-in/out gain [0..1].
        std::atomic<bool>  hasCustomPitch   { false }; ///< True if 4. DRAW CURVE painted a custom pitch at this hop.
        std::atomic<float> customPitchMidi  { 60.0f }; ///< Custom drawn pitch in MIDI semitones.
    };


    //==============================================================================
    // Problem #2 Fix: Lock-Free Hop-Indexed Timeline Override Table
    // Allows EVERY edited note on the timeline (from SNAP ALL, LEVELER, 4. DRAW,
    // or multi-note edits) to apply its own pitch, formant, variation, gain,
    // sibilant trim, and curved fade envelope during timeline playback/streaming!
    //==============================================================================
    static constexpr int maxTimelineHops = 4096;

    struct HopOverrideSlot
    {
        bool  active           = false;
        float targetMidi       = 60.0f;
        float noteCenter       = 1.0f;
        float noteVariation    = 1.0f;
        float formantSemitones = 0.0f;
        float gainDb           = 0.0f;
        float sibilantDb       = 0.0f;
        float rampInRatio      = 0.18f;
        float rampOutRatio     = 0.18f;
        float fadeMultiplier   = 1.0f;
        bool  hasCustomPitch   = false;
        float customPitchMidi  = 60.0f;
    };

    /** Begins a lock-free GUI-thread update in the inactive back-buffer. */
    void beginHopOverrideUpdate() noexcept
    {
        stagingHopBufferIdx = 1 - activeHopBufferIdx.load(std::memory_order_relaxed);
        stagingHasAny = false;
        for (auto& slot : hopOverrideBuffers[(size_t)stagingHopBufferIdx])
            slot = HopOverrideSlot{};
    }

    /** Writes a hop override into the staging back-buffer (published atomically by commitHopOverrideUpdate). */
    void setHopOverride(int hopIdx, const HopOverrideSlot& slot) noexcept
    {
        if (hopIdx < 0 || hopIdx >= maxTimelineHops)
            return;
        hopOverrideBuffers[(size_t)stagingHopBufferIdx][(size_t)hopIdx] = slot;
        if (slot.active)
            stagingHasAny = true;
    }

    /** Atomically swaps the staging back-buffer to become the active audio-thread buffer. */
    void commitHopOverrideUpdate() noexcept
    {
        activeHopBufferIdx.store(stagingHopBufferIdx, std::memory_order_release);
        hasAnyHopOverride.store(stagingHasAny, std::memory_order_release);
    }

    void clearHopOverrides() noexcept
    {
        beginHopOverrideUpdate();
        commitHopOverrideUpdate();
    }

    /** Lock-free, tear-free audio-thread lookup from the active front-buffer. */
    bool getHopOverride(int hopIdx, HopOverrideSlot& outSlot) const noexcept
    {
        if (!hasAnyHopOverride.load(std::memory_order_acquire) || hopIdx < 0 || hopIdx >= maxTimelineHops)
            return false;
        const int idx = activeHopBufferIdx.load(std::memory_order_acquire) & 1;
        const auto& s = hopOverrideBuffers[(size_t)idx][(size_t)hopIdx];
        if (!s.active)
            return false;
        outSlot = s;
        return true;
    }

    LiveNoteOverride& getLiveOverride() noexcept { return liveOverride; }
    const LiveNoteOverride& getLiveOverride() const noexcept { return liveOverride; }

    LiveNoteOverride& getPolyVoiceOverride(int voiceIdx) noexcept
    {
        return polyOverrides[(size_t)juce::jlimit(0, maxPolyVoices - 1, voiceIdx)];
    }
    const LiveNoteOverride& getPolyVoiceOverride(int voiceIdx) const noexcept
    {
        return polyOverrides[(size_t)juce::jlimit(0, maxPolyVoices - 1, voiceIdx)];
    }

    // Global macro snapshot for the GUI curve renderer (written by audio/processor, read by GUI):
    std::atomic<float> globalCenter      { 0.85f };
    std::atomic<float> globalVariation   { 1.00f };
    std::atomic<float> globalTransition  { 0.65f };
    std::atomic<float> globalSensitivity { 0.65f };
    std::atomic<float> globalFormant     { 0.0f };
    std::atomic<bool>  globalHumanize       { false };
    std::atomic<float> globalHumanizeAmount { 0.50f }; ///< Continuous Humanize slider [0..1]: 0.50 (50%) = Standard ON, 1.00 (100%) = Extreme Humanize
    std::atomic<bool>  globalPolyphonic    { false }; ///< False = Mono (NewTone), True = Polyphonic (Melodyne Chord/Choir/Strings)
    std::atomic<bool>  globalMidiCleanMode { false }; ///< True = Instrument MIDI Clean & Denoise Mode
    std::atomic<uint16_t> activeScaleMask { 0x0AB5 }; // C Major default
    std::atomic<bool>  snapToScale       { true };
    std::atomic<double> hostBpm          { 120.0 }; ///< Latest host tempo (clip-mode DAW grid).
    std::atomic<bool>  autoScroll        { true };
    std::atomic<bool>  replayMode        { false };
    std::atomic<int>   currentPlayheadHop { 0 };

    /** Mono mode: plays the dropped/loaded file inside the plugin (Play / Stop), replacing the input. */
    struct MonoPreview
    {
        juce::SpinLock lock;                         // GUI swaps the clip; audio thread only try-locks
        std::shared_ptr<const juce::AudioBuffer<float>> clip;
        double clipRate = 48000.0;
        int    hopSamples = 512;                     // clip-rate samples per roll hop
        std::atomic<bool>   playing { false };
        std::atomic<double> pos { 0.0 };             // clip-rate samples

        void setClip (const juce::AudioBuffer<float>& b, double rate, int hop)
        {
            auto c = std::make_shared<const juce::AudioBuffer<float>> (b);
            playing.store (false);
            const juce::SpinLock::ScopedLockType sl (lock);
            clip = std::move (c); clipRate = rate; hopSamples = hop; pos.store (0.0);
        }
        void clear() { playing.store (false); const juce::SpinLock::ScopedLockType sl (lock); clip.reset(); }
        bool hasClip() { const juce::SpinLock::ScopedLockType sl (lock); return clip != nullptr; }
    };
    MonoPreview monoPreview;
    /** Clip preview plays from its own playhead and ignores the host's transport and position. */
    std::atomic<bool> independentPlay { false };
    std::atomic<int>   vocalRangeMode     { 0 }; ///< 0=Full, 1=Soprano, 2=Alto, 3=Tenor, 4=Bass

    // Live MIDI Input state (written by audio thread when MIDI notes arrive, read by GUI/Engine):
    std::atomic<int>      liveMidiTargetNote { -1 };
    std::atomic<uint16_t> liveMidiNoteMask   { 0 };

private:
    juce::AbstractFifo fifo;
    std::array<PitchData, (size_t)capacity> buffer{};

    juce::AbstractFifo cmdFifo;
    std::array<NewtoneEditCommand, (size_t)cmdCapacity> cmdBuffer{};

    LiveNoteOverride liveOverride;
    std::atomic<bool> hasAnyHopOverride  { false };
    std::atomic<int>  activeHopBufferIdx { 0 };
    int               stagingHopBufferIdx = 1;
    bool              stagingHasAny       = false;
    std::array<std::array<HopOverrideSlot, maxTimelineHops>, 2> hopOverrideBuffers {};
    std::array<LiveNoteOverride, maxPolyVoices> polyOverrides{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchTraceQueue)
};