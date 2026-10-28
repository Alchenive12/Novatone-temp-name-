#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>

/**
    Choir: a mixer over pre-made AI singer voices, played in time with the host.

    Documents/NovaTone/Choir/<group>/            a group = every recording ("take") of one song / section set
        group.json                               choir level, dip
        <take>/parts.wav                         9 channels from the Harmony tab: lead, then harmony voices 1..8 (the "parts")
        <take>/session.json                      sample rate, song position of sample 0, parts on (register, note range)
        <take>/mix.json                          this take's singers (voice, level, pan, mute, solo)
        <take>/requests/<part>_<voice>.req       picked up by the Choir helper (tools/choir/choir_helper.py)
        <take>/voices/<part>_<voice>.wav|.fail   converted voice (same length / rate as parts.wav) or failed voice check
    All takes of the open group play together, each at its own song position. Takes can move between groups.
    Swapping a strip's voice loads the cached file at once if it exists, otherwise asks the helper and keeps
    playing the old voice until the new one is ready.

    Audio thread: render() only (try-lock, no allocation). Everything else: message thread.
*/
class ChoirSession : private juce::Timer
{
public:
    static constexpr int maxStrips = 32, maxTakes = 8, numParts = 8;

    /** reg: 0 low male, 1 mid male, 2 mid female, 3 high female, -1 original. lo / hi: recommended MIDI range. */
    struct VoiceInfo { const char* id; const char* name; const char* type; int reg, lo, hi; };
    static const std::vector<VoiceInfo>& catalog()
    {
        static const std::vector<VoiceInfo> c {
            { "original",     "Original",  "your voice, no AI", -1,  0, 127 },
            { "male_low3",    "Adam",   "Low male",     0, 40, 62 },   // E2-D4, sits around E3
            { "male_low4",    "Ben",    "Low male",     0, 40, 62 },
            { "male_low",     "Caleb",  "Low male",     0, 38, 60 },   // D2-C4
            { "male_low2",    "Daniel", "Low male",     0, 38, 60 },
            { "male_mid",     "Eli",    "Mid male",     1, 48, 67 },   // C3-G4
            { "male_mid2",    "Felix",  "Mid male",     1, 48, 67 },
            { "fmid_a",       "Gali",   "Mid female",   2, 55, 76 },   // G3-E5, sits around F4
            { "fmid_b",       "Hila",   "Mid female",   2, 55, 76 },
            { "fmid_c",       "Inbar",  "Mid female",   2, 55, 76 },
            { "female_mid",   "Julia",  "Mid female",   2, 55, 76 },
            { "female_mid2",  "Keren",  "Mid female",   2, 55, 76 },
            { "fhigh_a",      "Lior",   "High female",  3, 60, 81 },   // C4-A5, sits around G4
            { "fhigh_b",      "Maya",   "High female",  3, 60, 81 },
            { "female_high",  "Noa",    "High female",  3, 60, 81 },
            { "female_high2", "Ofri",   "High female",  3, 60, 81 } };
        return c;
    }
    static const VoiceInfo* info (const juce::String& id)
    {
        for (auto& v : catalog()) if (id == v.id) return &v;
        return nullptr;
    }
    static juce::String noteName (int m) { return juce::MidiMessage::getMidiNoteName (m, true, true, 4); }
    static juce::String voiceLabel (const juce::String& id)
    {
        if (auto* v = info (id)) return v->reg < 0 ? juce::String ("Original") : juce::String (v->name) + " (" + v->type + ")";
        return id;
    }
    static juce::String rangeText (const juce::String& id)
    {
        if (auto* v = info (id); v != nullptr && v->reg >= 0) return noteName (v->lo) + "-" + noteName (v->hi);
        return {};
    }

    struct Take
    {
        juce::File folder;
        double rate = 48000.0;
        juce::int64 start = 0, length = 0;
        std::vector<int> parts;
        std::array<int, numParts> reg {}, lo {}, hi {};   // per part (1-based index - 1); lo/hi 0 = unknown
    };

    struct Strip
    {
        std::atomic<bool> used { false }, mute { false }, solo { false };
        std::atomic<float> levelDb { 0.0f }, pan { 0.0f };
        int take = 0, part = 1, offsetMs = 0;
        juce::String voice = "original", pendingVoice, status;   // message thread
        std::shared_ptr<const std::vector<float>> audio;           // swapped under `lock`
    };

    static juce::File rootFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("NovaTone").getChildFile ("Choir");
    }
    static juce::StringArray groupNames()
    {
        juce::StringArray g;
        for (auto& d : rootFolder().findChildFiles (juce::File::findDirectories, false))
            if (! d.getChildFile ("session.json").existsAsFile()) g.add (d.getFileName());
        g.sortNatural();
        return g;
    }

    ChoirSession() { startTimer (700); }
    ~ChoirSession() override { stopTimer(); }

    void prepare (double sampleRate, int maxBlock)
    {
        hostRate = sampleRate;
        tmpL.assign ((size_t) maxBlock, 0.0f); tmpR.assign ((size_t) maxBlock, 0.0f);
        const auto c = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 3000.0f, 0.7f, juce::Decibels::decibelsToGain (-4.0f));
        dipL.coefficients = c; dipR.coefficients = c; dipL.reset(); dipR.reset();
    }

    //==========================================================================
    // Message thread
    bool isOpen() const { return folder.isDirectory(); }
    juce::File getFolder() const { return folder; }          ///< the open group
    juce::String getMessage() const { return message; }
    int getNumTakes() const { return (int) takes.size(); }
    const Take& take (int t) const { return takes[(size_t) t]; }
    Strip& strip (int i) { return strips[(size_t) i]; }

    /** Voices whose recommended range covers the part's notes (all of the part's register if the range is unknown). */
    bool fits (int t, int part, const VoiceInfo& v) const
    {
        if (v.reg < 0) return true;
        const auto& tk = takes[(size_t) t];
        const int lo = tk.lo[(size_t) part - 1], hi = tk.hi[(size_t) part - 1];
        if (lo <= 0 || hi <= 0) return v.reg == tk.reg[(size_t) part - 1];
        return lo >= v.lo && hi <= v.hi;
    }

    /** New group folder (made unique), opened. */
    bool newGroup (const juce::String& name)
    {
        auto d = rootFolder().getNonexistentChildFile (juce::File::createLegalFileName (name.isNotEmpty() ? name : "Group"), {}, false);
        return d.createDirectory() && open (d);
    }

    /** After the Choir tab recorded parts.wav into a take folder inside the open group: write session.json + default mix, reopen. */
    bool createFromRecording (const juce::File& takeDir, double sr, juce::int64 startSample, const std::vector<int>& onParts,
                              const std::vector<int>& regs, const std::vector<int>& los = {}, const std::vector<int>& his = {})
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("sampleRate", sr);
        o->setProperty ("startSample", startSample);
        juce::Array<juce::var> ps;
        for (size_t k = 0; k < onParts.size(); ++k)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty ("part", onParts[k]); p->setProperty ("register", regs[k]);
            p->setProperty ("lo", k < los.size() ? los[k] : 0); p->setProperty ("hi", k < his.size() ? his[k] : 0);
            ps.add (juce::var (p.get()));
        }
        o->setProperty ("parts", ps);
        takeDir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
        takeDir.getChildFile ("mix.json").deleteFile();
        saveMix();
        return open (takeDir.getParentDirectory());
    }

    /** Opens a group folder (every take in it). A take folder opens its group. */
    bool open (juce::File dir)
    {
        if (dir.getChildFile ("session.json").existsAsFile()) dir = dir.getParentDirectory();
        if (! dir.isDirectory()) { message = "Choir folder not found: " + dir.getFullPathName(); return false; }
        const bool wasOn = enabled.load() || ! isOpen();
        close();
        folder = dir;
        const auto g = juce::JSON::parse (dir.getChildFile ("group.json"));
        if (g.isObject()) { busDb.store ((float) g["bus"]); dip.store ((bool) g["dip"]); }
        auto dirs = dir.findChildFiles (juce::File::findDirectories, false);
        std::sort (dirs.begin(), dirs.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
        std::vector<Take> found;
        for (auto& d : dirs)
        {
            if ((int) found.size() >= maxTakes) break;
            const auto js = juce::JSON::parse (d.getChildFile ("session.json"));
            if (! js.isObject() || ! d.getChildFile ("parts.wav").existsAsFile()) continue;
            Take tk; tk.folder = d; tk.rate = (double) js["sampleRate"]; tk.start = (juce::int64) js["startSample"];
            if (auto* a = js["parts"].getArray())
                for (auto& p : *a)
                {
                    const int part = (int) p["part"];
                    if (part < 1 || part > numParts) continue;
                    tk.parts.push_back (part);
                    tk.reg[(size_t) part - 1] = (int) p["register"]; tk.lo[(size_t) part - 1] = (int) p["lo"]; tk.hi[(size_t) part - 1] = (int) p["hi"];
                }
            juce::WavAudioFormat wav;
            if (std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (d.getChildFile ("parts.wav").createInputStream().release(), true)); rd != nullptr)
                tk.length = rd->lengthInSamples;
            found.push_back (tk);
        }
        { const juce::SpinLock::ScopedLockType sl (lock); takes = std::move (found); }
        bool anyNewMix = false, anyLoaded = false;
        for (int t = 0; t < (int) takes.size(); ++t)
            if (loadMix (t)) anyLoaded = true;
            else { defaultMix (t); anyNewMix = true; }
        if (anyNewMix && ! anyLoaded) balance();   // a fresh group gets the default balance; existing mixes are kept
        for (int i = 0; i < maxStrips; ++i)
            if (strips[(size_t) i].used.load()) { auto& s = strips[(size_t) i]; const auto v = s.voice; s.voice = {}; setVoice (i, v); }
        enabled.store (wasOn);
        message = {};
        return true;
    }

    void close()
    {
        enabled.store (false);
        const juce::SpinLock::ScopedLockType sl (lock);
        for (auto& s : strips) { s.used.store (false); s.audio.reset(); s.pendingVoice = {}; s.status = {}; }
        takes.clear();
        folder = juce::File();
    }

    /** Moves a take (with its voices and mix) to another group; the open group reloads. */
    bool moveTake (int t, const juce::String& toGroup)
    {
        if (t < 0 || t >= (int) takes.size()) return false;
        saveMix();
        const auto src = takes[(size_t) t].folder;
        auto dstGroup = rootFolder().getChildFile (juce::File::createLegalFileName (toGroup));
        if (dstGroup == folder) return false;
        dstGroup.createDirectory();
        const auto dst = dstGroup.getNonexistentChildFile (src.getFileName(), {}, false);
        const auto group = folder;
        close();
        const bool ok = src.moveFileTo (dst);
        open (group);
        if (! ok) message = "Couldn't move " + src.getFileName();
        return ok;
    }

    int addStrip (int t, int part, const juce::String& voice)
    {
        for (int i = 0; i < maxStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (s.used.load()) continue;
            s.take = t; s.part = part; s.levelDb.store (0.0f); s.pan.store (0.0f); s.mute.store (false); s.solo.store (false);
            s.offsetMs = (i * 7) % 23; s.voice = {}; s.pendingVoice = {}; s.status = {};
            s.used.store (true);
            setVoice (i, voice);
            return i;
        }
        return -1;
    }

    void removeStrip (int i)
    {
        auto& s = strips[(size_t) i];
        s.used.store (false);
        const juce::SpinLock::ScopedLockType sl (lock);
        s.audio.reset();
        saveMix();
    }

    /** Cached voice -> instant. Otherwise ask the helper; the old voice keeps playing until the new one is ready. */
    void setVoice (int i, const juce::String& voice)
    {
        auto& s = strips[(size_t) i];
        if (! isOpen() || voice == s.voice || s.take >= (int) takes.size()) return;
        const auto& tk = takes[(size_t) s.take];
        if (voice == "original" || voiceFile (tk, s.part, voice).existsAsFile())
        {
            if (auto a = load (tk, s.part, voice))
            {
                { const juce::SpinLock::ScopedLockType sl (lock); s.audio = a; }
                s.voice = voice; s.pendingVoice = {}; s.status = {};
            }
            else s.status = "Can't read the " + voiceLabel (voice) + " file";
        }
        else if (failFile (tk, s.part, voice).existsAsFile())
            s.status = voiceLabel (voice) + " didn't pass the voice check";
        else
        {
            request (tk, s.part, voice);
            s.pendingVoice = voice;
            s.status = "Waiting for the Choir helper...";
            if (s.audio == nullptr) s.voice = voice;
        }
        saveMix();
    }

    /** True when switching this strip to `voice` would be instant. */
    bool isReady (int i, const juce::String& voice) const
    {
        const auto& s = strips[(size_t) i];
        return s.take < (int) takes.size() && (voice == "original" || voiceFile (takes[(size_t) s.take], s.part, voice).existsAsFile());
    }

    /** Asks the helper for every voice that fits each part of the open group (so trying them later is instant). Returns how many. */
    int prepareFittingVoices()
    {
        int n = 0;
        for (int t = 0; t < (int) takes.size(); ++t)
            for (int part : takes[(size_t) t].parts)
                for (auto& v : catalog())
                {
                    const auto& tk = takes[(size_t) t];
                    if (v.reg < 0 || ! fits (t, part, v) || voiceFile (tk, part, v.id).existsAsFile() || failFile (tk, part, v.id).existsAsFile()) continue;
                    request (tk, part, v.id); ++n;
                }
        return n;
    }

    void saveMix()
    {
        if (! isOpen()) return;
        juce::DynamicObject::Ptr g = new juce::DynamicObject();
        g->setProperty ("bus", busDb.load()); g->setProperty ("dip", dip.load());
        folder.getChildFile ("group.json").replaceWithText (juce::JSON::toString (juce::var (g.get())));
        for (int t = 0; t < (int) takes.size(); ++t)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            juce::Array<juce::var> ss;
            for (auto& s : strips)
            {
                if (! s.used.load() || s.take != t) continue;
                juce::DynamicObject::Ptr d = new juce::DynamicObject();
                d->setProperty ("part", s.part);
                d->setProperty ("voice", s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice);
                d->setProperty ("level", s.levelDb.load()); d->setProperty ("pan", s.pan.load());
                d->setProperty ("mute", s.mute.load()); d->setProperty ("solo", s.solo.load());
                ss.add (juce::var (d.get()));
            }
            o->setProperty ("strips", ss);
            takes[(size_t) t].folder.getChildFile ("mix.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
        }
    }

    /** Choir about 7.5 dB under the lead: every voice is loudness-matched to its part by the helper. */
    void balance()
    {
        int n = 0, maxPerTake = 0;
        for (int t = 0; t < (int) takes.size(); ++t)
        {
            n = 0;
            for (auto& s : strips) if (s.used.load() && s.take == t) { ++n; s.levelDb.store (0.0f); }
            maxPerTake = juce::jmax (maxPerTake, n);
        }
        busDb.store (-7.5f - 10.0f * std::log10 ((float) juce::jmax (1, maxPerTake)));
        saveMix();
    }

    /** Offline mix of the whole group as heard (levels, pans, mutes, solos, dip, choir level), at the takes' rate.
        Writes `file` (stereo 24-bit) and returns the song position (samples at that rate) of its first sample, or -1. */
    juce::int64 renderGroup (const juce::File& file, double& rateOut)
    {
        if (takes.empty()) return -1;
        juce::int64 first = std::numeric_limits<juce::int64>::max(), last = 0;
        for (auto& t : takes) { first = std::min (first, t.start); last = std::max (last, t.start + t.length); }
        rateOut = takes[0].rate;
        const auto len = (int) std::min<juce::int64> (last - first + (juce::int64) (0.05 * rateOut), (juce::int64) (rateOut * 60 * 30));
        juce::AudioBuffer<float> out (2, len); out.clear();
        bool anySolo = false;
        for (auto& s : strips) anySolo = anySolo || (s.used.load() && s.solo.load());
        for (auto& s : strips)
        {
            if (! s.used.load() || s.audio == nullptr || s.mute.load() || (anySolo && ! s.solo.load())) continue;
            const auto& a = *s.audio;
            const float g = juce::Decibels::decibelsToGain (s.levelDb.load()), p = s.pan.load();
            const float gl = g * std::cos ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f), gr = g * std::sin ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const auto off = takes[(size_t) s.take].start - first + (juce::int64) (s.offsetMs * 0.001 * rateOut);
            for (size_t k = 0; k < a.size(); ++k)
            {
                const auto j = off + (juce::int64) k;
                if (j < 0 || j >= len) continue;
                out.addSample (0, (int) j, gl * a[k]); out.addSample (1, (int) j, gr * a[k]);
            }
        }
        if (dip.load())
        {
            juce::dsp::IIR::Filter<float> f[2];
            for (int c = 0; c < 2; ++c)
            {
                f[(size_t) c].coefficients = juce::dsp::IIR::Coefficients<float>::makePeakFilter (rateOut, 3000.0f, 0.7f, juce::Decibels::decibelsToGain (-4.0f));
                auto* d = out.getWritePointer (c);
                for (int i = 0; i < len; ++i) d[i] = f[(size_t) c].processSample (d[i]);
            }
        }
        out.applyGain (juce::Decibels::decibelsToGain (busDb.load()));
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), rateOut, 2, 24, {}, 0));
        if (w == nullptr || ! w->writeFromAudioSampleBuffer (out, 0, len)) return -1;
        return first;
    }

    std::atomic<bool> enabled { false }, dip { true };
    std::atomic<float> busDb { -12.0f };

    //==========================================================================
    // Audio thread: adds the choir to l / r (song-aligned like the rest of the plugin output).
    void render (float* l, float* r, int n, std::optional<juce::int64> hostSample, bool playing, int latency) noexcept
    {
        if (! enabled.load (std::memory_order_relaxed) || ! playing || ! hostSample.has_value() || (size_t) n > tmpL.size()) return;
        const juce::SpinLock::ScopedTryLockType tl (lock);
        if (! tl.isLocked()) return;
        bool anySolo = false;
        for (auto& s : strips) anySolo = anySolo || (s.used.load (std::memory_order_relaxed) && s.solo.load (std::memory_order_relaxed));
        std::fill (tmpL.begin(), tmpL.begin() + n, 0.0f);
        std::fill (tmpR.begin(), tmpR.begin() + n, 0.0f);
        bool any = false;
        for (auto& s : strips)
        {
            if (! s.used.load (std::memory_order_relaxed) || s.audio == nullptr || s.mute.load (std::memory_order_relaxed)) continue;
            if (anySolo && ! s.solo.load (std::memory_order_relaxed)) continue;
            if (s.take >= (int) takes.size()) continue;
            const auto& tk = takes[(size_t) s.take];
            const auto& a = *s.audio;
            const double ratio = tk.rate / hostRate;
            const float g = juce::Decibels::decibelsToGain (s.levelDb.load (std::memory_order_relaxed));
            const float p = juce::jlimit (-1.0f, 1.0f, s.pan.load (std::memory_order_relaxed));
            const float gl = g * std::cos ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const float gr = g * std::sin ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const double first = (double) (*hostSample - latency) * ratio - (double) tk.start - s.offsetMs * 0.001 * tk.rate;
            for (int i = 0; i < n; ++i)
            {
                const double x = first + i * ratio;
                const auto k = (juce::int64) std::floor (x);
                if (k < 0 || k + 1 >= (juce::int64) a.size()) continue;
                const float f = (float) (x - (double) k);
                const float v = a[(size_t) k] + f * (a[(size_t) k + 1] - a[(size_t) k]);
                tmpL[(size_t) i] += gl * v; tmpR[(size_t) i] += gr * v;
                any = true;
            }
        }
        if (! any) return;
        if (dip.load (std::memory_order_relaxed))
            for (int i = 0; i < n; ++i) { tmpL[(size_t) i] = dipL.processSample (tmpL[(size_t) i]); tmpR[(size_t) i] = dipR.processSample (tmpR[(size_t) i]); }
        const float bus = juce::Decibels::decibelsToGain (busDb.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i) { l[i] += bus * tmpL[(size_t) i]; if (r != l) r[i] += bus * tmpR[(size_t) i]; }
    }

private:
    static juce::String key (int part, const juce::String& voice) { return juce::String (part) + "_" + voice; }
    static juce::File voiceFile (const Take& t, int part, const juce::String& v) { return t.folder.getChildFile ("voices").getChildFile (key (part, v) + ".wav"); }
    static juce::File failFile (const Take& t, int part, const juce::String& v) { return t.folder.getChildFile ("voices").getChildFile (key (part, v) + ".fail"); }
    static juce::File reqFile (const Take& t, int part, const juce::String& v) { return t.folder.getChildFile ("requests").getChildFile (key (part, v) + ".req"); }
    static void request (const Take& t, int part, const juce::String& v)
    {
        t.folder.getChildFile ("requests").createDirectory();
        reqFile (t, part, v).replaceWithText (juce::String (part) + " " + v);
    }

    static std::shared_ptr<const std::vector<float>> load (const Take& t, int part, const juce::String& voice)
    {
        juce::WavAudioFormat wav;
        const auto f = voice == "original" ? t.folder.getChildFile ("parts.wav") : voiceFile (t, part, voice);
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
        if (rd == nullptr || rd->lengthInSamples <= 0) return {};
        const int ch = voice == "original" ? part : 0;    // parts.wav: channel 0 = lead, 1..8 = parts
        if (ch >= (int) rd->numChannels) return {};
        auto v = std::make_shared<std::vector<float>> ((size_t) rd->lengthInSamples, 0.0f);
        std::vector<float*> dest ((size_t) rd->numChannels, nullptr);
        dest[(size_t) ch] = v->data();
        rd->read (dest.data(), (int) rd->numChannels, 0, (int) rd->lengthInSamples);
        return v;
    }

    int freeStrip() const { for (int i = 0; i < maxStrips; ++i) if (! strips[(size_t) i].used.load()) return i; return -1; }

    bool loadMix (int t)
    {
        const auto js = juce::JSON::parse (takes[(size_t) t].folder.getChildFile ("mix.json"));
        auto* a = js["strips"].getArray();
        if (a == nullptr) return false;
        for (auto& d : *a)
        {
            const int i = freeStrip();
            if (i < 0) break;
            auto& s = strips[(size_t) i];
            s.take = t; s.part = (int) d["part"]; s.voice = d["voice"].toString(); s.pendingVoice = {}; s.status = {};
            s.levelDb.store ((float) d["level"]); s.pan.store ((float) d["pan"]);
            s.mute.store ((bool) d["mute"]); s.solo.store ((bool) d["solo"]); s.offsetMs = (i * 7) % 23;
            s.used.store (true);
        }
        return true;
    }

    /** Two singers per part, panned apart, from voices that fit the part. */
    void defaultMix (int t)
    {
        for (int part : takes[(size_t) t].parts)
        {
            int k = 0;
            for (auto& v : catalog())
            {
                const int i = freeStrip();
                if (v.reg < 0 || k >= 2 || i < 0 || ! fits (t, part, v)) continue;
                auto& s = strips[(size_t) i];
                s.take = t; s.part = part; s.voice = v.id; s.pendingVoice = {}; s.status = {};
                s.levelDb.store (0.0f); s.pan.store ((k == 0 ? -1.0f : 1.0f) * (0.25f + 0.15f * (float) v.reg));
                s.mute.store (false); s.solo.store (false); s.offsetMs = (i * 7) % 23;
                s.used.store (true); ++k;
            }
            if (k == 0) addStrip (t, part, "original");   // nothing fits the part's range: keep it audible
        }
    }

    void timerCallback() override
    {
        if (! isOpen()) return;
        for (int i = 0; i < maxStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (! s.used.load() || s.pendingVoice.isEmpty() || s.take >= (int) takes.size()) continue;
            const auto& tk = takes[(size_t) s.take];
            const auto v = s.pendingVoice;
            if (voiceFile (tk, s.part, v).existsAsFile() && ! reqFile (tk, s.part, v).existsAsFile())
            {
                s.voice = {}; setVoice (i, v);
            }
            else if (failFile (tk, s.part, v).existsAsFile())
            {
                s.pendingVoice = {}; s.status = voiceLabel (v) + " didn't pass the voice check";
                if (s.voice == v) s.voice = {};   // never played: nothing to keep
                saveMix();
            }
            else if (tk.folder.getChildFile ("requests").getChildFile (key (s.part, v) + ".working").existsAsFile())
                s.status = "Making " + voiceLabel (v) + "...";
        }
    }

    juce::File folder;
    juce::String message;
    double hostRate = 48000.0;
    std::vector<Take> takes;
    std::array<Strip, maxStrips> strips;
    juce::SpinLock lock;
    std::vector<float> tmpL, tmpR;
    juce::dsp::IIR::Filter<float> dipL, dipR;
};
