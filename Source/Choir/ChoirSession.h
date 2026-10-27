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

    A choir lives in a folder (Documents/NovaTone/Choir/<name>/):
      parts.wav      9 channels recorded from the Harmony tab: lead, then harmony voices 1..8 (the "parts")
      session.json   sample rate, song position of the first sample, and which parts were on
      mix.json       the mixer (strips, levels, pans, mutes, solos, choir level, dip)
      requests/      <part>_<voice>.req   written here, picked up by the Choir helper (tools/choir/choir_helper.py)
      voices/        <part>_<voice>.wav   converted voice, same length / rate as parts.wav
                     <part>_<voice>.fail  didn't pass the voice check (sounds too much like the lead singer)
    Swapping a strip's voice loads the cached file at once if it exists, otherwise asks the helper and keeps
    playing the old voice until the new one is ready.

    Audio thread: render() only (try-lock, no allocation). Everything else: message thread.
*/
class ChoirSession : private juce::Timer
{
public:
    static constexpr int maxStrips = 16, numParts = 8;

    struct VoiceInfo { const char* id; const char* label; int reg; };   // reg: 0 low male, 1 mid male, 2 mid female, 3 high female, -1 original
    static const std::vector<VoiceInfo>& catalog()
    {
        static const std::vector<VoiceInfo> c {
            { "original",     "Original (no AI)", -1 },
            { "male_low3",    "Low male A", 0 }, { "male_low4", "Low male B", 0 }, { "male_low", "Low male C", 0 }, { "male_low2", "Low male D", 0 },
            { "male_mid",     "Mid male A", 1 }, { "male_mid2", "Mid male B", 1 },
            { "fmid_a",       "Mid female A", 2 }, { "fmid_b", "Mid female B", 2 }, { "fmid_c", "Mid female C", 2 },
            { "female_mid",   "Mid female D", 2 }, { "female_mid2", "Mid female E", 2 },
            { "fhigh_a",      "High female A", 3 }, { "fhigh_b", "High female B", 3 },
            { "female_high",  "High female C", 3 }, { "female_high2", "High female D", 3 } };
        return c;
    }
    static juce::String voiceLabel (const juce::String& id)
    {
        for (auto& v : catalog()) if (id == v.id) return v.label;
        return id;
    }

    struct Strip
    {
        std::atomic<bool> used { false }, mute { false }, solo { false };
        std::atomic<float> levelDb { 0.0f }, pan { 0.0f };
        int part = 1, offsetMs = 0;
        juce::String voice = "original", pendingVoice, status;   // message thread
        std::shared_ptr<const std::vector<float>> audio;           // swapped under `lock`
    };

    static juce::File rootFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("NovaTone").getChildFile ("Choir");
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
    juce::File getFolder() const { return folder; }
    juce::String getMessage() const { return message; }
    double getSampleRate() const { return rate; }
    std::vector<int> getParts() const { return parts; }
    int getPartRegister (int part) const { return part >= 1 && part <= numParts ? partReg[(size_t) part - 1] : 2; }
    Strip& strip (int i) { return strips[(size_t) i]; }

    /** After the Harmony tab recorded parts.wav into `dir`: write session.json and a default mix, then open it. */
    bool createFromRecording (const juce::File& dir, double sr, juce::int64 startSample, const std::vector<int>& onParts, const std::vector<int>& regs)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("sampleRate", sr);
        o->setProperty ("startSample", startSample);
        juce::Array<juce::var> ps;
        for (size_t k = 0; k < onParts.size(); ++k)
        {
            juce::DynamicObject::Ptr p = new juce::DynamicObject();
            p->setProperty ("part", onParts[k]); p->setProperty ("register", regs[k]);
            ps.add (juce::var (p.get()));
        }
        o->setProperty ("parts", ps);
        dir.getChildFile ("session.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
        dir.getChildFile ("mix.json").deleteFile();
        return open (dir);
    }

    bool open (const juce::File& dir)
    {
        close();
        const auto js = juce::JSON::parse (dir.getChildFile ("session.json"));
        if (! dir.getChildFile ("parts.wav").existsAsFile() || ! js.isObject()) { message = "Not a choir folder: " + dir.getFullPathName(); return false; }
        folder = dir;
        rate = (double) js["sampleRate"];
        start.store ((juce::int64) js["startSample"]);
        parts.clear();
        if (auto* a = js["parts"].getArray())
            for (auto& p : *a)
            {
                const int part = (int) p["part"];
                if (part < 1 || part > numParts) continue;
                parts.push_back (part); partReg[(size_t) part - 1] = (int) p["register"];
            }
        if (! loadMix()) defaultMix();
        for (int i = 0; i < maxStrips; ++i)
            if (strips[(size_t) i].used.load()) { auto& s = strips[(size_t) i]; const auto v = s.voice; s.voice = {}; setVoice (i, v); }
        enabled.store (true);
        message = {};
        return true;
    }

    void close()
    {
        enabled.store (false);
        const juce::SpinLock::ScopedLockType sl (lock);
        for (auto& s : strips) { s.used.store (false); s.audio.reset(); s.pendingVoice = {}; s.status = {}; }
        folder = juce::File();
    }

    int addStrip (int part, const juce::String& voice)
    {
        for (int i = 0; i < maxStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (s.used.load()) continue;
            s.part = part; s.levelDb.store (0.0f); s.pan.store (0.0f); s.mute.store (false); s.solo.store (false);
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
        if (! isOpen() || voice == s.voice) return;
        if (voice == "original" || voiceFile (s.part, voice).existsAsFile())
        {
            if (auto a = load (s.part, voice))
            {
                { const juce::SpinLock::ScopedLockType sl (lock); s.audio = a; }
                s.voice = voice; s.pendingVoice = {}; s.status = {};
            }
            else s.status = "Can't read the " + voiceLabel (voice) + " file";
        }
        else if (failFile (s.part, voice).existsAsFile())
            s.status = voiceLabel (voice) + " didn't pass the voice check";
        else
        {
            folder.getChildFile ("requests").createDirectory();
            folder.getChildFile ("requests").getChildFile (key (s.part, voice) + ".req").replaceWithText (juce::String (s.part) + " " + voice);
            s.pendingVoice = voice;
            s.status = "Waiting for the Choir helper...";
            if (s.audio == nullptr) s.voice = voice;
        }
        saveMix();
    }

    void saveMix()
    {
        if (! isOpen()) return;
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("bus", busDb.load()); o->setProperty ("dip", dip.load());
        juce::Array<juce::var> ss;
        for (auto& s : strips)
        {
            if (! s.used.load()) continue;
            juce::DynamicObject::Ptr d = new juce::DynamicObject();
            d->setProperty ("part", s.part);
            d->setProperty ("voice", s.pendingVoice.isNotEmpty() ? s.pendingVoice : s.voice);
            d->setProperty ("level", s.levelDb.load()); d->setProperty ("pan", s.pan.load());
            d->setProperty ("mute", s.mute.load()); d->setProperty ("solo", s.solo.load());
            ss.add (juce::var (d.get()));
        }
        o->setProperty ("strips", ss);
        folder.getChildFile ("mix.json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }

    /** Choir about 7.5 dB under the lead: every voice is loudness-matched to its part by the helper. */
    void balance()
    {
        int n = 0;
        for (auto& s : strips) if (s.used.load()) { ++n; s.levelDb.store (0.0f); }
        busDb.store (-7.5f - 10.0f * std::log10 ((float) juce::jmax (1, n)));
        saveMix();
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
        const double ratio = rate / hostRate;
        bool any = false;
        for (auto& s : strips)
        {
            if (! s.used.load (std::memory_order_relaxed) || s.audio == nullptr || s.mute.load (std::memory_order_relaxed)) continue;
            if (anySolo && ! s.solo.load (std::memory_order_relaxed)) continue;
            const auto& a = *s.audio;
            const float g = juce::Decibels::decibelsToGain (s.levelDb.load (std::memory_order_relaxed));
            const float p = juce::jlimit (-1.0f, 1.0f, s.pan.load (std::memory_order_relaxed));
            const float gl = g * std::cos ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const float gr = g * std::sin ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const double first = (double) (*hostSample - latency - start.load (std::memory_order_relaxed)) * ratio - s.offsetMs * 0.001 * rate;
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
    juce::File voiceFile (int part, const juce::String& v) const { return folder.getChildFile ("voices").getChildFile (key (part, v) + ".wav"); }
    juce::File failFile (int part, const juce::String& v) const { return folder.getChildFile ("voices").getChildFile (key (part, v) + ".fail"); }

    std::shared_ptr<const std::vector<float>> load (int part, const juce::String& voice) const
    {
        juce::WavAudioFormat wav;
        const auto f = voice == "original" ? folder.getChildFile ("parts.wav") : voiceFile (part, voice);
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

    bool loadMix()
    {
        const auto js = juce::JSON::parse (folder.getChildFile ("mix.json"));
        auto* a = js["strips"].getArray();
        if (a == nullptr) return false;
        busDb.store ((float) js["bus"]); dip.store ((bool) js["dip"]);
        int i = 0;
        for (auto& d : *a)
        {
            if (i >= maxStrips) break;
            auto& s = strips[(size_t) i];
            s.part = (int) d["part"]; s.voice = d["voice"].toString(); s.pendingVoice = {}; s.status = {};
            s.levelDb.store ((float) d["level"]); s.pan.store ((float) d["pan"]);
            s.mute.store ((bool) d["mute"]); s.solo.store ((bool) d["solo"]); s.offsetMs = (i * 7) % 23;
            s.used.store (true); ++i;
        }
        return true;
    }

    /** Two singers per part, panned apart, from the part's register. */
    void defaultMix()
    {
        int i = 0;
        for (int part : parts)
        {
            const int reg = getPartRegister (part);
            int k = 0;
            for (auto& v : catalog())
            {
                if (v.reg != reg || k >= 2 || i >= maxStrips) continue;
                auto& s = strips[(size_t) i];
                s.part = part; s.voice = v.id; s.pendingVoice = {}; s.status = {};
                s.levelDb.store (0.0f); s.pan.store ((k == 0 ? -1.0f : 1.0f) * (0.25f + 0.15f * (float) reg));
                s.mute.store (false); s.solo.store (false); s.offsetMs = (i * 7) % 23;
                s.used.store (true); ++i; ++k;
            }
        }
        balance();
    }

    void timerCallback() override
    {
        if (! isOpen()) return;
        const bool helperBusy = folder.getChildFile ("requests").getNumberOfChildFiles (juce::File::findFiles, "*.working") > 0;
        for (int i = 0; i < maxStrips; ++i)
        {
            auto& s = strips[(size_t) i];
            if (! s.used.load() || s.pendingVoice.isEmpty()) continue;
            const auto v = s.pendingVoice;
            if (voiceFile (s.part, v).existsAsFile() && ! folder.getChildFile ("requests").getChildFile (key (s.part, v) + ".req").existsAsFile())
            {
                s.voice = {}; setVoice (i, v);
            }
            else if (failFile (s.part, v).existsAsFile())
            {
                s.pendingVoice = {}; s.status = voiceLabel (v) + " didn't pass the voice check";
                if (s.voice == v) s.voice = {};   // never played: nothing to keep
                saveMix();
            }
            else if (helperBusy) s.status = "Making " + voiceLabel (v) + "...";
        }
    }

    juce::File folder;
    juce::String message;
    double rate = 48000.0, hostRate = 48000.0;
    std::atomic<juce::int64> start { 0 };
    std::vector<int> parts;
    std::array<int, numParts> partReg {};
    std::array<Strip, maxStrips> strips;
    juce::SpinLock lock;
    std::vector<float> tmpL, tmpR;
    juce::dsp::IIR::Filter<float> dipL, dipR;
};
