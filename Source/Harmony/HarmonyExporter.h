#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <optional>

/**
    "Export harmonies": records only the harmony voices (no lead) to a WAV while the host plays.

    Arm it from the GUI, press play in the DAW, stop when done. The file starts at the song
    position where playback started (plugin latency removed), so a drop at that position lines up.
    Recording also stops on its own when the host stops or jumps (loop / relocate).

    Audio thread: push() only, which writes into a juce ThreadedWriter FIFO (no allocation or
    disk I/O there) under a try-lock. Message thread: arm / stop / cancel, and a timer that
    finalises the file.
*/
class HarmonyExporter : private juce::Timer
{
public:
    enum class State { idle, armed, recording, finishing, ready, error };

    HarmonyExporter() = default;
    ~HarmonyExporter() override { stopTimer(); closeWriter(); }

    void prepare (double sr, int maxBlock)
    {
        sampleRate.store (sr);
        zeros.assign ((size_t) juce::jmax (1, maxBlock) * 4, 0.0f);
    }

    static juce::File exportFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("NovaTone").getChildFile ("Harmonies");
    }

    //==========================================================================
    // Message thread
    bool arm()
    {
        cancel();
        auto folder = exportFolder();
        if (! folder.createDirectory()) { setError ("Can't create " + folder.getFullPathName()); return false; }
        auto f = folder.getNonexistentChildFile ("Harmonies " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), ".wav", false);
        auto stream = std::make_unique<juce::FileOutputStream> (f);
        if (! stream->openedOk()) { setError ("Can't write " + f.getFullPathName()); return false; }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), sampleRate.load(), 2, 24, {}, 0));
        if (w == nullptr) { setError ("Can't create the WAV writer"); return false; }
        stream.release(); // owned by the writer now
        if (! diskThread.isThreadRunning()) diskThread.startThread();
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), diskThread, juce::roundToInt (sampleRate.load() * 4.0));
            samplesWritten.store (0);
            startSample.store (0);
            file = f;
            state.store (State::armed);
        }
        startTimerHz (10);
        return true;
    }

    /** Stop recording (keeps what was recorded) or disarm. */
    void stop()
    {
        auto s = state.load();
        if (s == State::recording) state.store (State::finishing);
        else if (s == State::armed) cancel();
    }

    /** Disarm / forget the current file (the file itself stays on disk if it was finished). */
    void cancel()
    {
        const bool wasPending = state.load() == State::armed;
        closeWriter();
        if (wasPending) file.deleteFile();
        state.store (State::idle);
    }

    State getState() const noexcept { return state.load(); }
    juce::File getFile() const { return file; }
    juce::String getError() const { return error; }
    double getSecondsRecorded() const noexcept { return (double) samplesWritten.load() / sampleRate.load(); }
    double getStartSeconds() const noexcept { return (double) startSample.load() / sampleRate.load(); }

    //==========================================================================
    // Audio thread. l / r = harmony-only output for this block (nullptr = silence).
    void push (const float* l, const float* r, int n, std::optional<juce::int64> hostSample, bool playing, int latency) noexcept
    {
        const auto s = state.load (std::memory_order_relaxed);
        if (s != State::armed && s != State::recording) return;
        const juce::SpinLock::ScopedTryLockType tl (lock);
        if (! tl.isLocked() || writer == nullptr) return;

        if (s == State::armed)
        {
            if (! playing) return;
            startSample.store (hostSample.value_or (0));
            expectedNext = hostSample.has_value() ? *hostSample : -1;
            skip = juce::jmax (0, latency); // the first `latency` output samples belong before the start
            state.store (State::recording);
        }
        const bool jumped = hostSample.has_value() && expectedNext >= 0 && std::llabs (*hostSample - expectedNext) > (juce::int64) n;
        if (! playing || jumped) { state.store (State::finishing); return; }
        expectedNext = hostSample.has_value() ? *hostSample + n : -1;

        const int off = juce::jmin (skip, n);
        skip -= off;
        const int count = n - off;
        if (count <= 0 || (size_t) n > zeros.size()) return;
        const float* ch[2] = { l != nullptr ? l + off : zeros.data(), r != nullptr ? r + off : (l != nullptr ? l + off : zeros.data()) };
        if (writer->write (ch, count))
            samplesWritten.fetch_add (count);
    }

private:
    void timerCallback() override
    {
        if (state.load() == State::finishing)
        {
            closeWriter(); // flushes the FIFO and finalises the WAV header
            state.store (samplesWritten.load() > 0 ? State::ready : State::idle);
            if (state.load() == State::idle) file.deleteFile();
        }
        const auto s = state.load();
        if (s != State::armed && s != State::recording && s != State::finishing) stopTimer();
    }

    void closeWriter()
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> old;
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            old = std::move (writer);
        }
        old.reset();
    }

    void setError (const juce::String& e) { error = e; state.store (State::error); }

    std::atomic<double> sampleRate { 48000.0 };
    std::vector<float> zeros;
    juce::TimeSliceThread diskThread { "NovaTone harmony export" };
    juce::SpinLock lock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    std::atomic<State> state { State::idle };
    std::atomic<juce::int64> samplesWritten { 0 }, startSample { 0 };
    juce::int64 expectedNext = -1;
    int skip = 0;
    juce::File file;
    juce::String error;
};
