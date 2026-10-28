#pragma once

#include "PolyAnalyzer.h"
#include "PolyRenderer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace Poly
{
    /** One note as the piano roll currently has it (the session's source of truth for edits).
        Split pieces keep the `sourceId` of the analysed note they came from. */
    struct ClipNote
    {
        int   id           = 0;
        int   sourceId     = 0;
        int   lane         = 0;
        int   startFrame   = 0;
        int   endFrame     = 0;
        float detectedMidi = 60.0f;
        int   targetMidi   = 60;
        float fineCents    = 0.0f;
        float gainDb       = 0.0f;
        float formantSemitones = 0.0f;
        bool  userEdited   = false;

        float semitoneShift() const noexcept { return (float) targetMidi + 0.01f * fineCents - detectedMidi; }
    };

    /**
        Melodyne-style clip for polyphonic editing.

        - Record: captures the track's audio while the host plays (positions taken from the host
          timeline), into a pre-allocated chunk pool - the audio thread never allocates or locks.
        - Load: reads an audio file (anchored at the start of the timeline).
        - A background worker analyses the clip, then re-renders whenever the notes change
          (stale renders are cancelled).
        - Playback: while the host plays inside the clip's range, the rendered clip replaces the
          live input (latency-compensated). Rendered buffers are swapped lock-free (hazard pointer).
        - State: analysis + notes go into the plugin state; audio is stored as FLAC next to it
          (app-data folder) and referenced by file name.
    */
    class ClipSession : private juce::Thread,
                        private juce::AsyncUpdater
    {
    public:
        enum class Status { empty, recording, analysing, ready, error };

        ClipSession();
        ~ClipSession() override;

        //==================================================================
        // Audio thread
        void prepare (double sampleRate, int maxBlockSize, int numChannels);

        /** Call with the post-input-gain buffer before any processing (captures when recording). */
        void capture (const juce::AudioBuffer<float>& input, std::optional<juce::int64> hostSample, bool hostPlaying) noexcept;

        /** Replaces `io` with the rendered clip where the timeline is inside the clip. `latency` is the
            plugin's reported latency (output at block position P is heard at timeline P - latency).
            Returns true if any sample was replaced. */
        bool playback (juce::AudioBuffer<float>& io, std::optional<juce::int64> hostSample, bool hostPlaying, int latency) noexcept;

        //==================================================================
        // Message thread
        void startRecording();
        void stopRecording();
        /** timelineStart: song position (session-rate samples) where the clip plays; 0 = song start. */
        bool loadFile (const juce::File& file, juce::int64 timelineStart = 0);
        void clear();
        bool exportRender (const juce::File& file);

        void setPreviewPlaying (bool shouldPlay, juce::int64 fromClipSample = -1);
        /** Moves the preview start (or the running preview) to a clip-relative sample. */
        void seekPreview (juce::int64 clipSample) { previewPos.store (std::max<juce::int64> (0, clipSample)); seekPending.store (true); }
        bool isPreviewPlaying() const noexcept { return previewPlaying.load(); }

        void setNotes (const std::vector<ClipNote>& notes);   ///< from the piano roll; triggers a re-render
        std::vector<ClipNote> getNotes() const;
        std::shared_ptr<const Analysis> getAnalysis() const;

        Status getStatus() const noexcept { return status.load(); }
        float  getProgress() const noexcept { return progress.load(); }
        juce::String getStatusText() const;
        bool   hasClip() const noexcept { return clipLength.load() > 0; }
        double getClipSampleRate() const noexcept { return clipRate.load(); }
        juce::int64 getClipLength() const noexcept { return clipLength.load(); }

        /** True once the audible render reflects the latest notes. */
        bool isRenderCurrent() const
        {
            const std::lock_guard<std::mutex> g (dataLock);
            auto* rb = active.load();
            return analysis != nullptr && rb != nullptr && rb->generation == notesGeneration;
        }

        /** Clip-relative sample currently playing (host or preview), or -1. */
        juce::int64 getPlayPosition() const noexcept { return playPosition.load(); }

        /** Fired on the message thread when analysis finishes or the clip changes. */
        std::function<void()> onClipChanged;

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree& tree);

        static constexpr int maxChannels = 2;

    private:
        struct RenderBuffer
        {
            juce::AudioBuffer<float> audio;
            juce::int64 generation = 0;
        };

        void run() override;                 // worker thread
        void handleAsyncUpdate() override;   // notify UI
        void requestWork();
        bool assembleRecording();
        void publish (std::unique_ptr<RenderBuffer> rb);
        static juce::File clipFolder();

        // ---- capture (chunk pool) ----
        static constexpr int maxChunks = 1800;  // 1-second chunks: 30 minutes
        std::array<std::atomic<float*>, maxChunks> chunks {};
        std::vector<std::unique_ptr<float[]>> chunkStorage; // owned on the message thread
        int chunkLen = 48000;
        std::atomic<bool> recording { false };
        std::atomic<juce::int64> recordStart { -1 };
        std::atomic<juce::int64> recordedLength { 0 };
        std::atomic<bool> recordOverflow { false };
        void ensureChunks (int count);
        struct ChunkTopUp : juce::Timer { ClipSession& s; explicit ChunkTopUp (ClipSession& o) : s (o) {} void timerCallback() override; } chunkTopUp { *this };

        // ---- clip ----
        using ClipAudio = std::shared_ptr<const juce::AudioBuffer<float>>;
        ClipAudio clip;                         // immutable once published; replaced under dataLock
        std::atomic<double> clipRate { 48000.0 };
        std::atomic<juce::int64> clipLength { 0 };
        std::atomic<juce::int64> clipStart { 0 };  // timeline sample of clip sample 0
        juce::String clipFileName;
        double processRate = 48000.0;

        // ---- analysis + notes (guarded by dataLock; never touched by the audio thread) ----
        mutable std::mutex dataLock;
        std::shared_ptr<const Analysis> analysis;
        std::vector<ClipNote> notes;
        juce::int64 notesGeneration = 0;
        bool analysisRequested = false;

        // ---- rendered audio (audio thread reads via hazard pointer) ----
        std::atomic<RenderBuffer*> active { nullptr };
        std::atomic<RenderBuffer*> inUse  { nullptr };
        std::unique_ptr<RenderBuffer> ownedActive, retired;

        std::atomic<Status> status { Status::empty };
        std::atomic<float> progress { 0.0f };
        std::atomic<juce::int64> playPosition { -1 };
        std::atomic<bool> previewPlaying { false };
        std::atomic<juce::int64> previewPos { 0 };
        std::atomic<bool> seekPending { false };
        juce::WaitableEvent workEvent;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipSession)
    };
}
