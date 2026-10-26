#include "PolyClipSession.h"
#include "BasicPitch.h"
#include <algorithm>
#include <cmath>

namespace Poly
{
namespace
{
    /** Assigns display lanes so concurrent notes never share one (max 16). */
    std::vector<ClipNote> notesFromAnalysis (const Analysis& an)
    {
        std::vector<ClipNote> out;
        std::array<int, 16> laneEnd {};
        laneEnd.fill (-1);
        for (const auto& n : an.notes)
        {
            int lane = 0;
            for (int l = 0; l < 16; ++l)
                if (laneEnd[(size_t) l] <= n.startFrame) { lane = l; break; }
            laneEnd[(size_t) lane] = n.endFrame;

            ClipNote c;
            c.id = n.id;
            c.sourceId = n.id;
            c.lane = lane;
            c.startFrame = n.startFrame;
            c.endFrame = n.endFrame;
            c.detectedMidi = n.midi;
            c.targetMidi = (int) std::lround (n.midi);
            c.fineCents = (n.midi - (float) c.targetMidi) * 100.0f; // unedited = exactly as sung
            out.push_back (c);
        }
        return out;
    }

    /** The analysis the renderer sees: one note per piano-roll note (split pieces sliced from
        their source note), so edits apply to exactly the time range the user sees. */
    Analysis renderAnalysisFor (const Analysis& base, const std::vector<ClipNote>& notes, std::vector<NoteEdit>& edits)
    {
        Analysis ra;
        ra.sampleRate = base.sampleRate;
        ra.hopSize = base.hopSize;
        ra.numFrames = base.numFrames;
        for (const auto& cn : notes)
        {
            const Note* src = nullptr;
            for (const auto& n : base.notes) if (n.id == cn.sourceId) { src = &n; break; }
            if (src == nullptr) continue;
            const int s = juce::jmax (cn.startFrame, src->startFrame), e = juce::jmin (cn.endFrame, src->endFrame);
            if (e - s < 2) continue;
            Note n;
            n.id = cn.id;
            n.startFrame = s;
            n.endFrame = e;
            n.midi = cn.detectedMidi;
            double lv = 0.0;
            for (int f = s; f < e; ++f)
            {
                n.pitchMidi.push_back (src->pitchMidi[(size_t) (f - src->startFrame)]);
                n.amp.push_back (src->amp[(size_t) (f - src->startFrame)]);
                lv += n.amp.back();
            }
            n.level = (float) (lv / (e - s));
            ra.notes.push_back (std::move (n));

            const float shift = cn.semitoneShift();
            if (std::abs (shift) > 1.0e-3f || std::abs (cn.gainDb) > 1.0e-3f || std::abs (cn.formantSemitones) > 1.0e-3f)
                edits.push_back ({ cn.id, shift, cn.gainDb, cn.formantSemitones });
        }
        return ra;
    }

    juce::String encodeAnalysis (const Analysis& an)
    {
        juce::MemoryOutputStream os;
        os.writeInt (1); // version
        os.writeDouble (an.sampleRate);
        os.writeInt (an.hopSize);
        os.writeInt (an.numFrames);
        os.writeInt ((int) an.notes.size());
        for (const auto& n : an.notes)
        {
            os.writeInt (n.id); os.writeInt (n.startFrame); os.writeInt (n.endFrame);
            os.writeFloat (n.midi); os.writeFloat (n.level);
            for (size_t i = 0; i < n.pitchMidi.size(); ++i) { os.writeFloat (n.pitchMidi[i]); os.writeFloat (n.amp[i]); }
        }
        return os.getMemoryBlock().toBase64Encoding();
    }

    std::shared_ptr<Analysis> decodeAnalysis (const juce::String& b64)
    {
        juce::MemoryBlock mb;
        if (! mb.fromBase64Encoding (b64)) return nullptr;
        juce::MemoryInputStream is (mb, false);
        if (is.readInt() != 1) return nullptr;
        auto an = std::make_shared<Analysis>();
        an->sampleRate = is.readDouble();
        an->hopSize = is.readInt();
        an->numFrames = is.readInt();
        const int num = is.readInt();
        for (int k = 0; k < num && ! is.isExhausted(); ++k)
        {
            Note n;
            n.id = is.readInt(); n.startFrame = is.readInt(); n.endFrame = is.readInt();
            n.midi = is.readFloat(); n.level = is.readFloat();
            const int len = juce::jmax (0, n.endFrame - n.startFrame);
            for (int i = 0; i < len; ++i) { n.pitchMidi.push_back (is.readFloat()); n.amp.push_back (is.readFloat()); }
            an->notes.push_back (std::move (n));
        }
        return an;
    }
}

//==============================================================================
ClipSession::ClipSession() : juce::Thread ("NovaTone poly clip")
{
    startThread (juce::Thread::Priority::low);
}

ClipSession::~ClipSession()
{
    chunkTopUp.stopTimer();
    signalThreadShouldExit();
    workEvent.signal();
    stopThread (5000);
    cancelPendingUpdate();
    active.store (nullptr);
}

juce::File ClipSession::clipFolder()
{
   #if JUCE_MAC
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
   #else
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #endif
    return base.getChildFile ("Alchenive").getChildFile ("NovaTone").getChildFile ("Clips");
}

//==============================================================================
// Audio thread
void ClipSession::prepare (double sampleRate, int, int)
{
    processRate = sampleRate;
    if (! recording.load())
        chunkLen = juce::roundToInt (sampleRate);
}

void ClipSession::capture (const juce::AudioBuffer<float>& input, std::optional<juce::int64> hostSample, bool hostPlaying) noexcept
{
    if (! recording.load (std::memory_order_acquire) || ! hostPlaying || ! hostSample.has_value())
        return;

    juce::int64 start = recordStart.load();
    if (start < 0) { start = *hostSample; recordStart.store (start); }

    const int n = input.getNumSamples();
    const int chans = juce::jmin (maxChannels, input.getNumChannels());
    const juce::int64 base = *hostSample - start;
    juce::int64 maxIdx = recordedLength.load();
    for (int i = 0; i < n; ++i)
    {
        const juce::int64 idx = base + i;
        if (idx < 0) continue;
        const juce::int64 c = idx / chunkLen;
        if (c >= maxChunks) { recordOverflow.store (true); break; }
        float* p = chunks[(size_t) c].load (std::memory_order_acquire);
        if (p == nullptr) { recordOverflow.store (true); continue; }
        const int off = (int) (idx % chunkLen) * 2;
        p[off]     = input.getSample (0, i);
        p[off + 1] = input.getSample (chans > 1 ? 1 : 0, i);
        maxIdx = juce::jmax (maxIdx, idx + 1);
    }
    recordedLength.store (maxIdx);
}

bool ClipSession::playback (juce::AudioBuffer<float>& io, std::optional<juce::int64> hostSample, bool hostPlaying, int latency) noexcept
{
    RenderBuffer* rb = nullptr;
    do { rb = active.load(); inUse.store (rb); } while (rb != active.load());

    bool replaced = false;
    const int n = io.getNumSamples();
    if (rb != nullptr && rb->audio.getNumSamples() > 0 && std::abs (clipRate.load() - processRate) < 0.5 && ! recording.load())
    {
        juce::int64 k0 = 0;
        bool hasPos = false;
        if (hostPlaying && hostSample.has_value())
        {
            k0 = *hostSample - latency - clipStart.load();
            hasPos = true;
            previewPlaying.store (false);
        }
        else if (previewPlaying.load())
        {
            k0 = previewPos.load();
            previewPos.store (k0 + n);
            if (k0 >= rb->audio.getNumSamples()) previewPlaying.store (false);
            hasPos = true;
        }

        if (hasPos)
        {
            const int len = rb->audio.getNumSamples();
            const int srcCh = rb->audio.getNumChannels();
            const int a = (int) std::clamp<juce::int64> (-k0, 0, n);
            const int b = (int) std::clamp<juce::int64> ((juce::int64) len - k0, 0, n);
            if (b > a)
            {
                for (int ch = 0; ch < io.getNumChannels(); ++ch)
                {
                    const int sc = juce::jmin (ch, srcCh - 1);
                    io.copyFrom (ch, a, rb->audio, sc, (int) (k0 + a), b - a);
                }
                replaced = true;
            }
            playPosition.store (k0 + n / 2 >= 0 && k0 + n / 2 < len ? k0 + n / 2 : -1);
        }
        else
            playPosition.store (-1);
    }
    else
        playPosition.store (-1);

    inUse.store (nullptr);
    return replaced;
}

//==============================================================================
// Message thread
void ClipSession::ensureChunks (int count)
{
    count = juce::jmin (count, maxChunks);
    while ((int) chunkStorage.size() < count)
    {
        auto mem = std::make_unique<float[]> ((size_t) chunkLen * 2);
        std::fill (mem.get(), mem.get() + (size_t) chunkLen * 2, 0.0f);
        chunks[chunkStorage.size()].store (mem.get(), std::memory_order_release);
        chunkStorage.push_back (std::move (mem));
    }
}

void ClipSession::ChunkTopUp::timerCallback()
{
    if (s.recording.load())
        s.ensureChunks ((int) (s.recordedLength.load() / s.chunkLen) + 12);
    s.triggerAsyncUpdate();
}

void ClipSession::startRecording()
{
    if (recording.load()) return;
    setPreviewPlaying (false);
    // Drop any previous capture memory (audio thread doesn't touch chunks while not recording).
    for (auto& c : chunks) c.store (nullptr);
    chunkStorage.clear();
    chunkLen = juce::roundToInt (processRate);
    ensureChunks (12);
    recordStart.store (-1);
    recordedLength.store (0);
    recordOverflow.store (false);
    status.store (Status::recording);
    recording.store (true, std::memory_order_release);
    chunkTopUp.startTimerHz (10);
    if (onClipChanged) onClipChanged();
}

void ClipSession::stopRecording()
{
    if (! recording.load()) return;
    recording.store (false, std::memory_order_release);
    chunkTopUp.stopTimer();
    juce::Thread::sleep (30); // let an in-flight audio block finish writing

    if (! assembleRecording())
    {
        status.store (hasClip() ? Status::ready : Status::empty);
        if (onClipChanged) onClipChanged();
        return;
    }
    for (auto& c : chunks) c.store (nullptr);
    chunkStorage.clear();
}

bool ClipSession::assembleRecording()
{
    const juce::int64 len = recordedLength.load();
    if (len < (juce::int64) (processRate * 0.25)) return false; // nothing useful recorded

    auto audio = std::make_shared<juce::AudioBuffer<float>> (2, (int) len);
    for (juce::int64 i = 0; i < len; ++i)
    {
        const float* p = chunks[(size_t) (i / chunkLen)].load();
        const int off = (int) (i % chunkLen) * 2;
        audio->setSample (0, (int) i, p != nullptr ? p[off] : 0.0f);
        audio->setSample (1, (int) i, p != nullptr ? p[off + 1] : 0.0f);
    }

    const std::lock_guard<std::mutex> g (dataLock);
    notesGeneration++;            // cancels any running analysis/render
    clip = audio;
    clipRate.store (processRate);
    clipStart.store (recordStart.load());
    clipLength.store (len);
    clipFileName = {};
    analysis.reset();
    notes.clear();
    analysisRequested = true;
    status.store (Status::analysing);
    progress.store (0.0f);
    requestWork();
    if (onClipChanged) onClipChanged();
    return true;
}

bool ClipSession::loadFile (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) return false;

    const int chans = juce::jmin (2, (int) reader->numChannels);
    const auto maxLen = (juce::int64) (reader->sampleRate * 60.0 * 30.0);
    const int len = (int) juce::jmin (reader->lengthInSamples, maxLen);
    juce::AudioBuffer<float> raw (chans, len);
    reader->read (&raw, 0, len, 0, true, chans > 1);

    // Resample to the session rate so playback lines up with the host timeline.
    juce::AudioBuffer<float> res;
    if (std::abs (reader->sampleRate - processRate) > 0.5)
    {
        const double ratio = reader->sampleRate / processRate;
        const int outLen = (int) std::floor (len / ratio);
        res.setSize (chans, outLen);
        for (int ch = 0; ch < chans; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, raw.getReadPointer (ch), res.getWritePointer (ch), outLen);
        }
    }
    else
        res = std::move (raw);

    setPreviewPlaying (false);
    const std::lock_guard<std::mutex> g (dataLock);
    notesGeneration++;
    clip = std::make_shared<juce::AudioBuffer<float>> (std::move (res));
    clipRate.store (processRate);
    clipStart.store (0);
    clipLength.store (clip->getNumSamples());
    clipFileName = {};
    analysis.reset();
    notes.clear();
    analysisRequested = true;
    status.store (Status::analysing);
    progress.store (0.0f);
    requestWork();
    if (onClipChanged) onClipChanged();
    return true;
}

void ClipSession::clear()
{
    if (recording.load()) { recording.store (false); chunkTopUp.stopTimer(); juce::Thread::sleep (30); }
    setPreviewPlaying (false);
    publish (nullptr);
    {
        const std::lock_guard<std::mutex> g (dataLock);
        notesGeneration++;
        clip.reset();
        clipLength.store (0);
        clipFileName = {};
        analysis.reset();
        notes.clear();
        analysisRequested = false;
    }
    status.store (Status::empty);
    if (onClipChanged) onClipChanged();
}

bool ClipSession::exportRender (const juce::File& file)
{
    RenderBuffer* rb = active.load();
    if (rb == nullptr) return false;
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), clipRate.load(),
                                                                      (unsigned int) rb->audio.getNumChannels(), 24, {}, 0));
    if (w == nullptr) return false;
    return w->writeFromAudioSampleBuffer (rb->audio, 0, rb->audio.getNumSamples());
}

void ClipSession::setPreviewPlaying (bool shouldPlay, juce::int64 fromClipSample)
{
    if (shouldPlay)
    {
        if (fromClipSample >= 0) previewPos.store (fromClipSample);
        else if (! seekPending.load() && previewPos.load() >= clipLength.load()) previewPos.store (0);
        seekPending.store (false);
    }
    previewPlaying.store (shouldPlay && hasClip());
}

void ClipSession::setNotes (const std::vector<ClipNote>& newNotes)
{
    {
        const std::lock_guard<std::mutex> g (dataLock);
        notes = newNotes;
        notesGeneration++;
    }
    requestWork();
}

std::vector<ClipNote> ClipSession::getNotes() const
{
    const std::lock_guard<std::mutex> g (dataLock);
    return notes;
}

std::shared_ptr<const Analysis> ClipSession::getAnalysis() const
{
    const std::lock_guard<std::mutex> g (dataLock);
    return analysis;
}

juce::String ClipSession::getStatusText() const
{
    auto mmss = [] (double s) { const int t = (int) std::lround (s); return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
    switch (status.load())
    {
        case Status::recording:
            return recordStart.load() < 0 ? "Armed: start playback in your DAW to record"
                                          : "Recording " + mmss ((double) recordedLength.load() / processRate)
                                                + (recordOverflow.load() ? " (memory full)" : "");
        case Status::analysing: return "Analysing clip " + juce::String (juce::roundToInt (progress.load() * 100.0f)) + "%";
        case Status::ready:
        {
            if (std::abs (clipRate.load() - processRate) > 0.5) return "Clip was recorded at a different sample rate";
            const std::lock_guard<std::mutex> g (dataLock);
            return "Clip " + mmss ((double) clipLength.load() / clipRate.load()) + ", " + juce::String ((int) notes.size()) + " notes";
        }
        case Status::error: return "Couldn't load the saved clip audio";
        case Status::empty:
        default: return "Record or load audio to edit chords";
    }
}

void ClipSession::requestWork() { workEvent.signal(); }

void ClipSession::handleAsyncUpdate()
{
    if (onClipChanged) onClipChanged();
}

//==============================================================================
// Rendered-buffer swap (worker or message thread; never the audio thread)
void ClipSession::publish (std::unique_ptr<RenderBuffer> rb)
{
    static std::mutex publishLock;
    const std::lock_guard<std::mutex> g (publishLock);
    auto old = std::move (ownedActive);
    ownedActive = std::move (rb);
    active.store (ownedActive.get());
    if (old != nullptr)
        while (inUse.load() == old.get())
            juce::Thread::sleep (1);
}

//==============================================================================
// Worker
void ClipSession::run()
{
    juce::int64 renderedGeneration = -1;

    while (! threadShouldExit())
    {
        workEvent.wait (500);
        if (threadShouldExit()) break;

        // ---- analysis ----
        bool doAnalysis = false;
        ClipAudio source;
        juce::int64 gen = 0;
        {
            const std::lock_guard<std::mutex> g (dataLock);
            if (analysisRequested && clip != nullptr && clip->getNumSamples() > 0)
            {
                doAnalysis = true;
                analysisRequested = false;
                gen = notesGeneration;
                source = clip;
            }
        }
        juce::AudioBuffer<float> mono;
        if (doAnalysis)
        {
            mono.setSize (1, source->getNumSamples());
            const int ns = source->getNumSamples();
            mono.copyFrom (0, 0, source->getReadPointer (0), ns, source->getNumChannels() > 1 ? 0.5f : 1.0f);
            if (source->getNumChannels() > 1) mono.addFrom (0, 0, source->getReadPointer (1), ns, 0.5f);
        }
        if (doAnalysis)
        {
            auto stale = [&] { const std::lock_guard<std::mutex> g (dataLock); return notesGeneration != gen || threadShouldExit(); };
            auto cancelled = [&] { return threadShouldExit() || [&] { const std::lock_guard<std::mutex> g (dataLock); return notesGeneration != gen; }(); };
            // Learned prior: Basic Pitch (C++) weights candidate F0s and drops octave / 12th ghosts it doesn't hear.
            // Measured (tools/bptrain/README.md): piano F1 0.54 -> 0.65, synthetic octave errors 1055 -> 7.
            progress.store (0.0f); triggerAsyncUpdate();
            const auto prior = basicPitchPrior (mono.getReadPointer (0), mono.getNumSamples(), clipRate.load(), cancelled);
            AnalyzerSettings settings;
            if (prior.frames > 0) { settings.prior = &prior; settings.priorFloor = 0.3f; settings.priorNoteGate = 0.3f; }
            auto an = std::make_shared<Analysis> (analyse (mono.getReadPointer (0), mono.getNumSamples(), clipRate.load(), settings,
                cancelled, [&] (float p) { progress.store (p); triggerAsyncUpdate(); }));
            if (stale()) continue;

            // Persist the clip audio (FLAC) for the project's saved state.
            const juce::String fileName = juce::Uuid().toString() + ".flac";
            bool saved = false;
            {
                auto dir = clipFolder();
                dir.createDirectory();
                juce::FlacAudioFormat flac;
                if (auto* os = dir.getChildFile (fileName).createOutputStream().release())
                {
                    std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (os, clipRate.load(), (unsigned int) source->getNumChannels(), 24, {}, 0));
                    saved = w != nullptr && w->writeFromAudioSampleBuffer (*source, 0, source->getNumSamples());
                }
            }
            {
                const std::lock_guard<std::mutex> g (dataLock);
                if (notesGeneration != gen) continue;
                analysis = an;
                notes = notesFromAnalysis (*an);
                notesGeneration++;
                if (saved) clipFileName = fileName;
            }
            status.store (Status::ready);
            renderedGeneration = -1;
        }

        // ---- render (whenever the notes changed) ----
        std::shared_ptr<const Analysis> base;
        std::vector<ClipNote> snapshot;
        juce::int64 generation = 0;
        ClipAudio clipRef;
        {
            const std::lock_guard<std::mutex> g (dataLock);
            if (analysis == nullptr || notesGeneration == renderedGeneration || clip == nullptr || clip->getNumSamples() == 0) continue;
            base = analysis;
            snapshot = notes;
            generation = notesGeneration;
            clipRef = clip;
        }

        std::vector<NoteEdit> edits;
        const auto ra = renderAnalysisFor (*base, snapshot, edits);
        auto rb = std::make_unique<RenderBuffer>();
        rb->generation = generation;
        bool cancelled = false;
        render (*clipRef, clipRate.load(), ra, edits, rb->audio,
                [&] { const std::lock_guard<std::mutex> g (dataLock); cancelled = notesGeneration != generation || threadShouldExit(); return cancelled; });
        if (cancelled) { workEvent.signal(); continue; }
        publish (std::move (rb));
        renderedGeneration = generation;
        triggerAsyncUpdate();
    }
}

//==============================================================================
// State
juce::ValueTree ClipSession::toValueTree() const
{
    juce::ValueTree t ("PolyClip");
    const std::lock_guard<std::mutex> g (dataLock);
    if (clipLength.load() <= 0 || analysis == nullptr || clipFileName.isEmpty()) return t;
    t.setProperty ("file", clipFileName, nullptr);
    t.setProperty ("rate", clipRate.load(), nullptr);
    t.setProperty ("start", (juce::int64) clipStart.load(), nullptr);
    t.setProperty ("length", (juce::int64) clipLength.load(), nullptr);
    t.setProperty ("analysis", encodeAnalysis (*analysis), nullptr);
    juce::ValueTree ns ("Notes");
    for (const auto& n : notes)
    {
        juce::ValueTree c ("N");
        c.setProperty ("id", n.id, nullptr);       c.setProperty ("src", n.sourceId, nullptr);
        c.setProperty ("lane", n.lane, nullptr);   c.setProperty ("s", n.startFrame, nullptr);
        c.setProperty ("e", n.endFrame, nullptr);  c.setProperty ("det", n.detectedMidi, nullptr);
        c.setProperty ("tgt", n.targetMidi, nullptr); c.setProperty ("ct", n.fineCents, nullptr);
        c.setProperty ("g", n.gainDb, nullptr);    c.setProperty ("fm", n.formantSemitones, nullptr);
        c.setProperty ("ed", n.userEdited, nullptr);
        ns.appendChild (c, nullptr);
    }
    t.appendChild (ns, nullptr);
    return t;
}

void ClipSession::fromValueTree (const juce::ValueTree& t)
{
    const auto fileName = t.getProperty ("file").toString();
    auto an = decodeAnalysis (t.getProperty ("analysis").toString());
    if (fileName.isEmpty() || an == nullptr) { clear(); return; }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (clipFolder().getChildFile (fileName)));
    if (reader == nullptr) { clear(); status.store (Status::error); if (onClipChanged) onClipChanged(); return; }

    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);

    std::vector<ClipNote> restored;
    for (const auto& c : t.getChildWithName ("Notes"))
    {
        ClipNote n;
        n.id = c["id"]; n.sourceId = c["src"]; n.lane = c["lane"]; n.startFrame = c["s"]; n.endFrame = c["e"];
        n.detectedMidi = (float) c["det"]; n.targetMidi = c["tgt"]; n.fineCents = (float) c["ct"];
        n.gainDb = (float) c["g"]; n.formantSemitones = (float) c["fm"]; n.userEdited = c["ed"];
        restored.push_back (n);
    }

    {
        const std::lock_guard<std::mutex> g (dataLock);
        notesGeneration++;
        clip = std::make_shared<juce::AudioBuffer<float>> (std::move (audio));
        clipRate.store ((double) t.getProperty ("rate", reader->sampleRate));
        clipStart.store ((juce::int64) t.getProperty ("start", 0));
        clipLength.store (clip->getNumSamples());
        clipFileName = fileName;
        analysis = an;
        notes = restored.empty() ? notesFromAnalysis (*an) : restored;
        analysisRequested = false;
        notesGeneration++;
    }
    status.store (Status::ready);
    requestWork();
    if (onClipChanged) onClipChanged();
}
} // namespace Poly
