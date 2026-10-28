#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include "../DSP/MPMDetector.h"
#include "../Harmony/HarmonyExporter.h"

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

    enum NoteState { noteAI = 0, noteNormal, noteMuted };
    struct NoteSeg { juce::int64 start = 0, end = 0; int midi = 0, state = noteAI; };

    struct Take
    {
        juce::File folder;
        double rate = 48000.0;
        juce::int64 start = 0, length = 0;
        std::vector<int> parts;
        std::array<int, numParts> reg {}, lo {}, hi {};   // per part (1-based index - 1); lo/hi 0 = unknown
        std::array<std::vector<NoteSeg>, numParts> notes;  // per part: sung notes, each AI / normal / muted
    };

    struct Strip
    {
        std::atomic<bool> used { false }, mute { false }, solo { false };
        std::atomic<float> levelDb { 0.0f }, pan { 0.0f };
        int take = 0, part = 1, offsetMs = 0;
        bool normalRole = false;                                   // the part's own (un-choirized) voice, for notes set to normal
        juce::String voice = "original", pendingVoice, status;   // message thread
        std::shared_ptr<const std::vector<float>> audio, env;      // swapped under `lock`; env = per-sample gain from note states
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
                              const std::vector<int>& regs, const std::vector<int>& los = {}, const std::vector<int>& his = {},
                              const std::vector<juce::String>& singers = {}, const std::vector<bool>& doubles = {})
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
        detectNotes (takeDir, onParts);
        if (! singers.empty())   // from the Harmony tab: each card's singer (+ double), and the part's own voice for notes set to normal
        {
            Take tk; tk.folder = takeDir;
            for (size_t k = 0; k < onParts.size(); ++k)
            { const auto i = (size_t) onParts[k] - 1; tk.reg[i] = regs[k]; tk.lo[i] = k < los.size() ? los[k] : 0; tk.hi[i] = k < his.size() ? his[k] : 0; }
            juce::Array<juce::var> ss;
            auto add = [&] (int part, const juce::String& v, float pan, bool normal)
            {
                juce::DynamicObject::Ptr d = new juce::DynamicObject();
                d->setProperty ("part", part); d->setProperty ("voice", v); d->setProperty ("level", 0.0f); d->setProperty ("pan", pan);
                d->setProperty ("mute", false); d->setProperty ("solo", false); d->setProperty ("normal", normal);
                ss.add (juce::var (d.get()));
            };
            for (size_t k = 0; k < onParts.size(); ++k)
            {
                const int part = onParts[k];
                const auto want = k < singers.size() ? singers[k] : juce::String ("auto");
                const float side = (k % 2 == 0 ? -1.0f : 1.0f) * 0.35f;
                if (want != "off")
                {
                    const auto first = want == "auto" ? bestFit (tk, part, {}) : want;
                    add (part, first, side, false);
                    if (k < doubles.size() && doubles[k]) add (part, bestFit (tk, part, first), -side, false);
                }
                add (part, "original", side * 0.5f, true);
            }
            juce::DynamicObject::Ptr o2 = new juce::DynamicObject();
            o2->setProperty ("strips", ss);
            takeDir.getChildFile ("mix.json").replaceWithText (juce::JSON::toString (juce::var (o2.get())));
        }
        saveMix();
        const bool ok = open (takeDir.getParentDirectory());
        if (ok && ! singers.empty() && takes.size() == 1) balance();
        return ok;
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
            loadNotes (tk);
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
        rebuildAllEnvs();
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
            s.offsetMs = (i * 7) % 23; s.voice = {}; s.pendingVoice = {}; s.status = {}; s.normalRole = false; s.env.reset();
            s.used.store (true);
            setVoice (i, voice);
            rebuildEnvs (t, part);
            return i;
        }
        return -1;
    }

    void removeStrip (int i)
    {
        auto& s = strips[(size_t) i];
        s.used.store (false);
        { const juce::SpinLock::ScopedLockType sl (lock); s.audio.reset(); s.env.reset(); }
        rebuildEnvs (s.take, s.part);
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
                const bool roleChange = (s.voice == "original") != (voice == "original");
                s.voice = voice; s.pendingVoice = {}; s.status = {};
                if (roleChange && ! takes.empty()) rebuildEnvs (s.take, s.part);
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
                d->setProperty ("normal", s.normalRole);
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
            for (auto& s : strips) if (s.used.load() && s.take == t && ! s.normalRole) { ++n; s.levelDb.store (0.0f); }
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
                const float e = s.env != nullptr && k < s.env->size() ? (*s.env)[k] : 1.0f;
                out.addSample (0, (int) j, gl * e * a[k]); out.addSample (1, (int) j, gr * e * a[k]);
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

    //==========================================================================
    // Recording parts (shared by the Harmony and Choir tabs: whichever tab armed it, either one finishes it)
    struct PendingArm
    {
        bool active = false;
        std::vector<int> parts, regs, lo, hi;
        std::vector<juce::String> singers;   // Harmony tab: per part "auto" / "off" / voice id (empty = Choir tab default mix)
        std::vector<bool> doubles;
    } pendingArm;

    /** Arms `recorder` for a new take in the open group (a new group if none is open). */
    bool beginArm (HarmonyExporter& recorder, PendingArm a)
    {
        if (a.parts.empty()) return false;
        a.lo.assign (a.parts.size(), 0); a.hi.assign (a.parts.size(), 0); a.active = true;
        pendingArm = std::move (a);
        enabled.store (false);
        if (! isOpen()) newGroup ("Group " + juce::String (groupNames().size() + 1));
        const auto dir = folder.getNonexistentChildFile ("Take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), {}, false);
        return recorder.arm (dir.getChildFile ("parts.wav"));
    }

    /** Call from a GUI timer: tracks each part's notes while recording; turns a finished recording into a take. True when a take was added. */
    bool pollArm (HarmonyExporter& recorder, const std::function<int (int part)>& voiceNote)
    {
        if (! pendingArm.active) return false;
        using S = HarmonyExporter::State;
        const auto st = recorder.getState();
        if (st == S::recording)
            for (size_t k = 0; k < pendingArm.parts.size(); ++k)
                if (const int n = voiceNote (pendingArm.parts[k]); n > 0)
                {
                    pendingArm.lo[k] = pendingArm.lo[k] == 0 ? n : juce::jmin (pendingArm.lo[k], n);
                    pendingArm.hi[k] = juce::jmax (pendingArm.hi[k], n);
                }
        if (st == S::idle || st == S::error) { pendingArm.active = false; return false; }   // cancelled
        if (st != S::ready) return false;
        auto a = std::move (pendingArm); pendingArm = {};
        createFromRecording (recorder.getFile().getParentDirectory(), recorder.getSampleRate(), recorder.getStartSample(),
                             a.parts, a.regs, a.lo, a.hi, a.singers, a.doubles);
        recorder.cancel();   // back to idle (the file stays)
        enabled.store (true);
        return true;
    }

    //==========================================================================
    // Harmony tab: each voice card = one part of the latest take
    int latestTake() const { return (int) takes.size() - 1; }
    int pendingCount() const { int n = 0; for (auto& s : strips) n += s.used.load() && s.pendingVoice.isNotEmpty() ? 1 : 0; return n; }

    /** The card's AI singers on the latest take: which = 0 (main) or 1 (double). Empty = none. */
    juce::String cardVoice (int part, int which) const
    {
        const int t = latestTake(); int k = 0;
        for (auto& s : strips)
            if (s.used.load() && s.take == t && s.part == part && ! s.normalRole && k++ == which)
                return s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice;
        return {};
    }
    int cardStrip (int part, int which) const
    {
        const int t = latestTake(); int k = 0;
        for (int i = 0; i < maxStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (s.used.load() && s.take == t && s.part == part && ! s.normalRole && k++ == which) return i;
        }
        return -1;
    }
    bool hasPart (int part) const
    {
        const int t = latestTake();
        return t >= 0 && std::find (takes[(size_t) t].parts.begin(), takes[(size_t) t].parts.end(), part) != takes[(size_t) t].parts.end();
    }

    /** voice "off" removes the card's AI singers (the part's own voice then sings every note). */
    void setCardSinger (int part, const juce::String& voice)
    {
        const int t = latestTake();
        if (t < 0 || ! hasPart (part)) return;
        if (voice == "off") { for (int w = 1; w >= 0; --w) if (const int i = cardStrip (part, w); i >= 0) removeStrip (i); return; }
        const auto v = voice == "auto" ? bestFit (takes[(size_t) t], part, {}) : voice;
        if (const int i = cardStrip (part, 0); i >= 0) setVoice (i, v);
        else if (const int i2 = addStrip (t, part, v); i2 >= 0) strips[(size_t) i2].pan.store (part % 2 == 0 ? 0.35f : -0.35f);
    }

    void setCardDouble (int part, bool on)
    {
        const int t = latestTake();
        if (t < 0 || ! hasPart (part)) return;
        const int d = cardStrip (part, 1), m = cardStrip (part, 0);
        if (! on) { if (d >= 0) removeStrip (d); return; }
        if (d >= 0 || m < 0) return;
        if (const int i = addStrip (t, part, bestFit (takes[(size_t) t], part, cardVoice (part, 0))); i >= 0)
            strips[(size_t) i].pan.store (-strips[(size_t) m].pan.load());
    }

    const std::vector<NoteSeg>* partNotes (int part) const
    {
        const int t = latestTake();
        return t >= 0 && part >= 1 && part <= numParts ? &takes[(size_t) t].notes[(size_t) part - 1] : nullptr;
    }
    juce::int64 latestStart() const { return latestTake() >= 0 ? takes.back().start : 0; }
    juce::int64 latestLength() const { return latestTake() >= 0 ? takes.back().length : 0; }

    /** Click on a note: AI -> normal (the part's own voice) -> muted -> AI. */
    void cycleNote (int part, int idx)
    {
        const int t = latestTake();
        if (t < 0 || part < 1 || part > numParts) return;
        auto& ns = takes[(size_t) t].notes[(size_t) part - 1];
        if (idx < 0 || idx >= (int) ns.size()) return;
        ns[(size_t) idx].state = (ns[(size_t) idx].state + 1) % 3;
        saveNotes (takes[(size_t) t]);
        rebuildEnvs (t, part);
    }

    /** One mono WAV per audible singer of the open group (level and note states applied, no pan / choir level),
        each starting at the song's start so they line up when dropped at bar 1. */
    juce::Array<juce::File> exportStems (const juce::File& dir)
    {
        juce::Array<juce::File> files;
        dir.deleteRecursively(); dir.createDirectory();
        bool anySolo = false;
        for (auto& s : strips) anySolo = anySolo || (s.used.load() && s.solo.load());
        for (auto& s : strips)
        {
            if (! s.used.load() || s.audio == nullptr || s.mute.load() || (anySolo && ! s.solo.load()) || s.take >= (int) takes.size()) continue;
            const auto& tk = takes[(size_t) s.take];
            const auto& a = *s.audio;
            bool audible = s.env == nullptr;
            if (! audible) for (float e : *s.env) if (e > 0.0f) { audible = true; break; }
            if (! audible) continue;
            const auto lead = tk.start + (juce::int64) (s.offsetMs * 0.001 * tk.rate);
            const float g = juce::Decibels::decibelsToGain (s.levelDb.load());
            juce::String name = "V" + juce::String (s.part) + " ";
            if (s.normalRole) name << "normal voice";
            else if (auto* v = info (s.voice)) name << (v->reg < 0 ? juce::String ("original") : juce::String (v->name));
            const auto f = dir.getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav", false);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), tk.rate, 1, 24, {}, 0));
            if (w == nullptr) continue;
            std::vector<float> chunk (65536);
            for (juce::int64 pos = 0, total = lead + (juce::int64) a.size(); pos < total; pos += (juce::int64) chunk.size())
            {
                const int n = (int) std::min<juce::int64> ((juce::int64) chunk.size(), total - pos);
                for (int i = 0; i < n; ++i)
                {
                    const auto k = pos + i - lead;
                    chunk[(size_t) i] = k < 0 ? 0.0f : g * a[(size_t) k] * (s.env != nullptr && (size_t) k < s.env->size() ? (*s.env)[(size_t) k] : 1.0f);
                }
                const float* ch[1] = { chunk.data() };
                w->writeFromFloatArrays (ch, 1, n);
            }
            files.add (f);
        }
        return files;
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
            const auto* env = s.env.get();
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
                float v = a[(size_t) k] + f * (a[(size_t) k + 1] - a[(size_t) k]);
                if (env != nullptr && (size_t) k < env->size()) v *= (*env)[(size_t) k];
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
            s.normalRole = (bool) d["normal"]; s.env.reset();
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
                s.take = t; s.part = part; s.voice = v.id; s.pendingVoice = {}; s.status = {}; s.normalRole = false; s.env.reset();
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

    /** Best-fitting singer for a part (register + range), skipping `exclude`. */
    juce::String bestFit (const Take& tk, int part, const juce::String& exclude) const
    {
        const int reg = tk.reg[(size_t) part - 1], lo = tk.lo[(size_t) part - 1], hi = tk.hi[(size_t) part - 1];
        juce::String sameReg;
        for (auto& v : catalog())
        {
            if (v.reg < 0 || exclude == v.id) continue;
            if (lo > 0 && lo >= v.lo && hi <= v.hi && v.reg == reg) return v.id;
            if (sameReg.isEmpty() && v.reg == reg) sameReg = v.id;
        }
        if (sameReg.isNotEmpty()) return sameReg;
        for (auto& v : catalog()) if (v.reg >= 0 && exclude != v.id && lo > 0 && lo >= v.lo && hi <= v.hi) return v.id;
        return "fmid_a";
    }

    /** Sung notes of each part (MPM pitch, 2048 / hop 512), written to notes.json. */
    static void detectNotes (const juce::File& takeDir, const std::vector<int>& parts)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (takeDir.getChildFile ("parts.wav").createInputStream().release(), true));
        if (rd == nullptr) return;
        const int len = (int) rd->lengthInSamples, win = 2048, hop = 512;
        juce::AudioBuffer<float> buf ((int) rd->numChannels, len);
        rd->read (&buf, 0, len, 0, true, true);
        MPMDetector det; det.prepare (rd->sampleRate, win);
        Take tk; tk.folder = takeDir;
        for (int part : parts)
        {
            if (part >= buf.getNumChannels()) continue;
            auto& ns = tk.notes[(size_t) part - 1];
            const float* x = buf.getReadPointer (part);
            int cur = 0; juce::int64 segStart = 0, lastVoiced = 0;
            auto close = [&] (juce::int64 end)
            {
                if (cur > 0 && end - segStart > (juce::int64) (0.08 * rd->sampleRate))
                {
                    if (! ns.empty() && ns.back().midi == cur && segStart - ns.back().end < (juce::int64) (0.06 * rd->sampleRate)) ns.back().end = end;
                    else ns.push_back ({ segStart, end, cur, noteAI });
                }
                cur = 0;
            };
            for (int p = 0; p + win <= len; p += hop)
            {
                const auto r = det.processFrame (x + p);
                const juce::int64 centre = p + win / 2;
                const int m = r.isVoiced && r.pitchHz > 60.0f && r.pitchHz < 1400.0f ? (int) std::lround (69.0 + 12.0 * std::log2 (r.pitchHz / 440.0)) : 0;
                if (m == cur && m > 0) { lastVoiced = centre; continue; }
                if (m > 0 && cur > 0 && std::abs (m - cur) == 0) continue;
                close (cur > 0 ? lastVoiced : centre);
                if (m > 0) { cur = m; segStart = centre - hop / 2; lastVoiced = centre; }
            }
            close (lastVoiced);
        }
        saveNotes (tk);
    }

    static void saveNotes (const Take& tk)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        for (int p = 1; p <= numParts; ++p)
        {
            const auto& ns = tk.notes[(size_t) p - 1];
            if (ns.empty()) continue;
            juce::Array<juce::var> arr;
            for (auto& n : ns) arr.add (juce::Array<juce::var> { (juce::int64) n.start, (juce::int64) n.end, n.midi, n.state });
            o->setProperty ("part" + juce::String (p), arr);
        }
        tk.folder.getChildFile ("notes.json").replaceWithText (juce::JSON::toString (juce::var (o.get()), true));
    }

    static void loadNotes (Take& tk)
    {
        const auto js = juce::JSON::parse (tk.folder.getChildFile ("notes.json"));
        for (int p = 1; p <= numParts; ++p)
            if (auto* arr = js.getProperty (juce::Identifier ("part" + juce::String (p)), {}).getArray())
                for (auto& n : *arr)
                    if (n.size() >= 4)
                    {
                        NoteSeg seg; seg.start = (juce::int64) n[0]; seg.end = (juce::int64) n[1]; seg.midi = (int) n[2]; seg.state = (int) n[3];
                        tk.notes[(size_t) p - 1].push_back (seg);
                    }
    }

    void rebuildAllEnvs()
    {
        for (int t = 0; t < (int) takes.size(); ++t)
            for (int part : takes[(size_t) t].parts) rebuildEnvs (t, part);
    }

    /** Per-sample gain of each strip on (take, part) from the note states, with 10 ms fades. */
    void rebuildEnvs (int t, int part)
    {
        if (t < 0 || t >= (int) takes.size() || part < 1 || part > numParts) return;
        const auto& tk = takes[(size_t) t];
        const auto& ns = tk.notes[(size_t) part - 1];
        bool hasAI = false;
        for (auto& s : strips) hasAI = hasAI || (s.used.load() && s.take == t && s.part == part && ! s.normalRole && s.voice != "original");
        const auto len = (size_t) std::max<juce::int64> (1, tk.length);
        // state per sample: a note owns the time up to the next note (consonants / breaths go with it)
        std::vector<unsigned char> st (len, (unsigned char) noteAI);
        for (size_t k = 0; k < ns.size(); ++k)
        {
            const auto a = (size_t) std::clamp<juce::int64> (k == 0 ? 0 : ns[k].start, 0, (juce::int64) len);
            const auto b = (size_t) std::clamp<juce::int64> (k + 1 < ns.size() ? ns[k + 1].start : (juce::int64) len, 0, (juce::int64) len);
            std::fill (st.begin() + (long) a, st.begin() + (long) b, (unsigned char) ns[k].state);
        }
        auto make = [&] (auto gainOf) -> std::shared_ptr<const std::vector<float>>
        {
            auto e = std::make_shared<std::vector<float>> (len);
            for (size_t i = 0; i < len; ++i) (*e)[i] = gainOf (st[i]) ? 1.0f : 0.0f;
            const int fade = juce::jmax (1, (int) (0.01 * tk.rate));   // linear-ish edges: moving average
            std::vector<float> c (len + 1, 0.0f);
            for (size_t i = 0; i < len; ++i) c[i + 1] = c[i] + (*e)[i];
            for (size_t i = 0; i < len; ++i)
            {
                const auto lo = i >= (size_t) fade / 2 ? i - (size_t) fade / 2 : 0, hi = std::min (len, i + (size_t) fade / 2 + 1);
                (*e)[i] = (c[hi] - c[lo]) / (float) (hi - lo);
            }
            return e;
        };
        const bool none = ns.empty();
        auto aiEnv = none ? nullptr : make ([] (unsigned char x) { return x == noteAI; });
        auto origEnv = none ? nullptr : make ([] (unsigned char x) { return x != noteMuted; });
        auto normalEnv = make ([&] (unsigned char x) { return x == noteNormal || (x == noteAI && ! hasAI); });
        const juce::SpinLock::ScopedLockType sl (lock);
        for (auto& s : strips)
        {
            if (! s.used.load() || s.take != t || s.part != part) continue;
            s.env = s.normalRole ? normalEnv : (s.voice == "original" ? origEnv : aiEnv);
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
