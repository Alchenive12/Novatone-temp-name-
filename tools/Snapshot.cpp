// Dev-only: renders the NovaTone editor to PNGs without a host.
// Build with -DNOVATONE_BUILD_SNAPSHOT=ON, run: NovaToneSnapshot <outDir> [width height]
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GUI/PitchTraceComponent.h"
#include "GUI/HarmonyTab.h"
#include "GUI/TuneTab.h"
#include "PluginEditor.h"
#include <fstream>

static void save(juce::Component& c, const juce::File& f, float scale)
{
    auto img = c.createComponentSnapshot(c.getLocalBounds(), true, scale);
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
    std::cout << "wrote " << f.getFullPathName() << "\n";
}

static PitchTraceComponent* findRoll(juce::Component& c)
{
    if (auto* r = dynamic_cast<PitchTraceComponent*>(&c)) return r;
    for (auto* child : c.getChildren())
        if (auto* r = findRoll(*child)) return r;
    return nullptr;
}

struct FakePlayHead : juce::AudioPlayHead
{
    juce::int64 pos = 0; bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setTimeInSamples(pos); p.setIsPlaying(playing); p.setBpm(120.0);
        p.setPpqPosition((double) pos / 48000.0 * 2.0);
        return p;
    }
};

/** Plucked-ish additive chord progression (Am - F - C - G), 1.6 s per chord. */
static juce::AudioBuffer<float> makeProgression(double sr, std::vector<std::vector<int>>& chordsOut)
{
    chordsOut = { { 45, 52, 57, 60, 64 }, { 41, 48, 53, 57, 60 }, { 48, 52, 55, 60, 64 }, { 43, 47, 50, 55, 59 } };
    const double chordDur = 1.6;
    juce::AudioBuffer<float> b(2, (int) (sr * (chordDur * 4 + 0.5)));
    b.clear();
    for (size_t c = 0; c < chordsOut.size(); ++c)
        for (size_t k = 0; k < chordsOut[c].size(); ++k)
        {
            const double f0 = 440.0 * std::pow(2.0, (chordsOut[c][k] - 69) / 12.0);
            const int start = (int) (sr * (0.1 + chordDur * (double) c + 0.015 * (double) k));
            const int len = (int) (sr * chordDur) - (int) (0.015 * sr * (double) k);
            for (int h = 1; h <= 12 && f0 * h < 8000.0; ++h)
            {
                const double a = 0.06 / std::pow(h, 1.1) * (h == 1 ? 0.7 : 1.0);
                double ph = 0.37 * h * (double) k;
                for (int i = 0; i < len && start + i < b.getNumSamples(); ++i)
                {
                    const double t = i / sr;
                    const double env = std::min(1.0, t / 0.003) * std::exp(-t * (1.2 + 0.35 * h)) * std::min(1.0, (len - i) / (sr * 0.02));
                    ph += juce::MathConstants<double>::twoPi * f0 * h / sr;
                    const float v = (float) (a * env * std::sin(ph));
                    b.addSample(0, start + i, v);
                    b.addSample(1, start + i, v);
                }
            }
        }
    return b;
}

/** "A#m7/F" -> notes for a pad (bass in octave 2, chord in octave 3-4). Empty on failure. */
static std::vector<int> chordNotes(const juce::String& name, juce::String& normalised)
{
    static const char* letters = "C D EF G A B";
    if (name.isEmpty()) return {};
    auto pcOf = [](juce::String s, int& used) -> int
    {
        const int base = juce::String("C D EF G A B").indexOfChar(juce::CharacterFunctions::toUpperCase(s[0]));
        if (base < 0) return -1;
        int pc = base; used = 1;
        if (s.length() > 1 && (s[1] == '#' || s[1] == 's')) { ++pc; used = 2; }
        else if (s.length() > 1 && s[1] == 'b') { --pc; used = 2; }
        return (pc + 12) % 12;
    };
    juce::ignoreUnused(letters);
    auto main = name.upToFirstOccurrenceOf("/", false, false);
    int used = 0; const int root = pcOf(main, used);
    if (root < 0) return {};
    auto q = main.substring(used);
    std::vector<int> iv = { 0, 4, 7 };
    if (q.startsWith("maj7")) iv = { 0, 4, 7, 11 };
    else if (q.startsWith("m7") || q.startsWith("min7")) iv = { 0, 3, 7, 10 };
    else if (q.startsWith("m") && ! q.startsWith("maj")) iv = { 0, 3, 7 };
    else if (q.startsWith("7")) iv = { 0, 4, 7, 10 };
    else if (q.startsWith("sus2")) iv = { 0, 2, 7 };
    else if (q.startsWith("sus4") || q.startsWith("sus")) iv = { 0, 5, 7 };
    else if (q.startsWith("dim")) iv = { 0, 3, 6 };
    else if (q.startsWith("5")) iv = { 0, 7 };
    int bass = root;
    if (name.containsChar('/')) { int u = 0; const int b = pcOf(name.fromFirstOccurrenceOf("/", false, false), u); if (b >= 0) bass = b; }
    std::vector<int> notes { 36 + bass };
    for (int i : iv) { int n = 48 + root + i; if (n < 52) n += 12; notes.push_back(n); }
    normalised = name;
    return notes;
}

static void pump(int ms)
{
    auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir(argc > 1 ? juce::String(argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();
    const int w = argc > 3 ? std::atoi(argv[2]) : 1040, h = argc > 3 ? std::atoi(argv[3]) : 820;

    std::unique_ptr<juce::AudioProcessor> proc(createPluginFilter());
    const bool polyTest = argc > 8 && juce::String(argv[8]) == "poly";
    const bool harmonyTest = argc > 8 && juce::String(argv[8]) == "harmony";
    const int theme = argc > 5 ? std::atoi(argv[5]) : 0;
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(dynamic_cast<NovaToneAudioProcessor*>(proc.get())->apvts.getParameter("ui_theme")))
        p->setValueNotifyingHost(p->convertTo0to1((float) theme));
    proc->setPlayConfigDetails(2, 2, 48000.0, 512);
    proc->prepareToPlay(48000.0, 512);

    auto* np = dynamic_cast<NovaToneAudioProcessor*>(proc.get());
    juce::AudioBuffer<float> buf(2, 512); juce::MidiBuffer midi; double ph = 0;
    FakePlayHead playHead;
    proc->setPlayHead(&playHead);

    if (polyTest)
    {
        // ---- End-to-end poly clip test through processBlock ----
        if (auto* p = np->apvts.getParameter("tune_polyphonic")) p->setValueNotifyingHost(1.0f);
        std::vector<std::vector<int>> chords;
        auto audio = makeProgression(48000.0, chords);
        auto& clip = np->getPolyClip();
        clip.startRecording();
        playHead.playing = true;
        const juce::int64 startPos = 48000 * 4; // DAW playback from bar 3
        for (int pos = 0; pos + 512 <= audio.getNumSamples(); pos += 512)
        {
            playHead.pos = startPos + pos;
            for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, audio, ch, pos, 512);
            proc->processBlock(buf, midi);
        }
        playHead.playing = false;
        clip.stopRecording();
        auto t0 = juce::Time::getMillisecondCounter();
        while (! clip.isRenderCurrent() && juce::Time::getMillisecondCounter() - t0 < 60000) pump(50);
        std::cout << "clip: " << clip.getStatusText() << "\n";

        // Move the C4 of the C major chord (3rd chord) up to C#4 -> C augmented-ish, and the F's A3 down to Ab3 (F minor)
        auto notes = clip.getNotes();
        int moved = 0;
        const double chordDur = 1.6;
        for (auto& n : notes)
        {
            const double t = n.startFrame * 512.0 / 48000.0;
            const int m = (int) std::lround(n.detectedMidi);
            if (m == 57 && t > 0.1 + chordDur * 1 - 0.1 && t < 0.1 + chordDur * 2 - 0.2) { n.targetMidi = 56; n.fineCents = 0; n.userEdited = true; ++moved; }
        }
        clip.setNotes(notes);
        t0 = juce::Time::getMillisecondCounter();
        while (! clip.isRenderCurrent() && juce::Time::getMillisecondCounter() - t0 < 60000) pump(50);
        std::cout << "edited notes: " << moved << " (F major A3 -> Ab3 = F minor)\n";

        // Play back through processBlock as the DAW would
        const int lat = proc->getLatencySamples();
        juce::AudioBuffer<float> out(2, audio.getNumSamples());
        out.clear();
        playHead.playing = true;
        for (int pos = 0; pos + 512 <= audio.getNumSamples() + lat; pos += 512)
        {
            playHead.pos = startPos + pos;
            buf.clear();
            proc->processBlock(buf, midi);
            // output at block position P is heard at P - latency
            for (int i = 0; i < 512; ++i)
            {
                const int heard = pos + i - lat;
                if (heard >= 0 && heard < out.getNumSamples()) { out.setSample(0, heard, buf.getSample(0, i)); out.setSample(1, heard, buf.getSample(1, i)); }
            }
        }
        playHead.playing = false;
        auto goertzel = [](const juce::AudioBuffer<float>& x, double f, double a, double b)
        {
            double s1 = 0, s2 = 0; const double w = juce::MathConstants<double>::twoPi * f / 48000.0, cw = 2 * std::cos(w);
            const int i0 = (int) (a * 48000), i1 = (int) (b * 48000);
            for (int i = i0; i < i1; ++i) { const double win = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * (i - i0) / (i1 - i0)); const double s0 = x.getSample(0, i) * win + cw * s1 - s2; s2 = s1; s1 = s0; }
            return s1 * s1 + s2 * s2 - cw * s1 * s2;
        };
        auto db = [](double r) { return 10.0 * std::log10(std::max(1e-12, r)); };
        const double a = 0.1 + 1.6 + 0.3, b = 0.1 + 3.2 - 0.3;
        std::cout << "F chord: A3 left " << juce::String(db(goertzel(out, 220.0, a, b) / goertzel(audio, 220.0, a, b)), 1)
                  << " dB, Ab3 now " << juce::String(db(goertzel(out, 207.65, a, b) / goertzel(audio, 220.0, a, b)), 1) << " dB rel. old A3\n";
        const double ua = 0.1 + 0.3, ub = 1.5; // Am chord untouched
        double err = 0, ref = 0;
        for (int i = (int) (ua * 48000); i < (int) (ub * 48000); ++i) { const double d = out.getSample(0, i) - audio.getSample(0, i); err += d * d; ref += (double) audio.getSample(0, i) * audio.getSample(0, i); }
        std::cout << "untouched chord through the plugin: difference " << juce::String(db(err / ref), 1) << " dB (bit-exact = very low)\n";

        // State round trip through the processor
        juce::MemoryBlock state; proc->getStateInformation(state);
        std::cout << "state size: " << state.getSize() << " bytes\n";

        juce::WavAudioFormat wav;
        for (auto [name, bb] : { std::pair<const char*, juce::AudioBuffer<float>*> { "poly_in.wav", &audio }, { "poly_out.wav", &out } })
        {
            auto f = outDir.getChildFile(name); f.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> wr(wav.createWriterFor(new juce::FileOutputStream(f), 48000.0, 2, 24, {}, 0));
            wr->writeFromAudioSampleBuffer(*bb, 0, bb->getNumSamples());
        }
        playHead.pos = startPos + (juce::int64) (2.4 * 48000) + lat; // playhead in the F chord for the screenshot
        playHead.playing = true;
        buf.clear(); proc->processBlock(buf, midi);
        playHead.playing = false;
    }

    const bool videoMode = argc > 8 && juce::String(argv[8]) == "video";
    if (videoMode)
    {
        // ---- Harmony tab demo video: real vocal + chord chart pad on the sidechain ----
        const juce::File vocalFile(juce::SystemStats::getEnvironmentVariable("VIDEO_VOCAL", ""));
        const juce::File chordFile(juce::SystemStats::getEnvironmentVariable("VIDEO_CHORDS", ""));
        const double startSec = juce::SystemStats::getEnvironmentVariable("VIDEO_START", "0").getDoubleValue();
        const double lenSec   = juce::SystemStats::getEnvironmentVariable("VIDEO_LEN", "20").getDoubleValue();
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(vocalFile));
        if (rd == nullptr) { std::cout << "can't read vocal\n"; return 1; }
        const double sr = 48000.0;
        const int total = (int) (lenSec * sr);
        juce::AudioBuffer<float> vocal(2, total);
        rd->read(&vocal, 0, total, (juce::int64) (startSec * rd->sampleRate), true, true);

        // chord chart: lines "seconds chord" (seconds relative to the song)
        struct ChordAt { double t; juce::String name; std::vector<int> notes; };
        std::vector<ChordAt> chart;
        for (auto line : juce::StringArray::fromLines(chordFile.loadFileAsString()))
        {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            ChordAt c; c.t = line.upToFirstOccurrenceOf(" ", false, false).getDoubleValue() - startSec;
            juce::String norm; c.notes = chordNotes(line.fromFirstOccurrenceOf(" ", false, false).trim(), norm); c.name = norm;
            if (! c.notes.empty()) chart.push_back(c);
        }
        // soft keys pad, re-struck at every chord change
        juce::AudioBuffer<float> pad(2, total); pad.clear();
        for (size_t k = 0; k < chart.size(); ++k)
        {
            const double t0 = chart[k].t, t1 = k + 1 < chart.size() ? chart[k + 1].t : lenSec;
            const int a = juce::jmax(0, (int) (t0 * sr)), b = juce::jmin(total, (int) (t1 * sr));
            if (b <= a) continue;
            for (size_t vi = 0; vi < chart[k].notes.size(); ++vi)
            {
                const double f0 = 440.0 * std::pow(2.0, (chart[k].notes[vi] - 69) / 12.0);
                const double amp = vi == 0 ? 0.10 : 0.06;
                for (int h = 1; h <= 6; ++h)
                {
                    double ph = 0.7 * h + vi;
                    const double ah = amp / std::pow(h, 1.4);
                    for (int i = a; i < b; ++i)
                    {
                        const double t = (i - a) / sr;
                        const double env = std::min(1.0, t / 0.01) * (0.55 + 0.45 * std::exp(-t * (0.8 + 0.4 * h))) * std::min(1.0, (b - i) / (sr * 0.03));
                        ph += juce::MathConstants<double>::twoPi * f0 * h / sr;
                        const float v = (float) (ah * env * std::sin(ph));
                        pad.addSample(0, i, v); pad.addSample(1, i, v);
                    }
                }
            }
        }

        if (const juce::File scF(juce::SystemStats::getEnvironmentVariable("VIDEO_SIDECHAIN", "")); scF.existsAsFile())
        {   // real instrumental on the sidechain instead of the synthesised chord pad (same rate as VIDEO_VOCAL: 48 kHz)
            std::unique_ptr<juce::AudioFormatReader> sr2(fm.createReaderFor(scF));
            if (sr2 != nullptr) { pad.clear(); sr2->read(&pad, 0, total, (juce::int64) (startSec * sr2->sampleRate), true, true); }
        }
        auto setP = [&](const char* id, float plain) { if (auto* p = np->apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(plain)); };
        auto layout = proc->getBusesLayout();
        if (layout.inputBuses.size() > 1) layout.inputBuses.getReference(1) = juce::AudioChannelSet::stereo();
        proc->setBusesLayout(layout);
        const int block = 400; // 4 blocks per 30 fps frame
        proc->prepareToPlay(sr, block);
        setP("tune_bypass", 1.0f);
        const int nVoices = juce::SystemStats::getEnvironmentVariable("VIDEO_VOICES", "2").getIntValue();
        setP("harm_v1_on", 1.0f); setP("harm_v2_on", nVoices > 1 ? 1.0f : 0.0f);
        setP("harm_v1_level", -5.0f); setP("harm_v2_level", -6.0f);
        setP("harm_humanize", 0.35f);
        if (juce::SystemStats::getEnvironmentVariable("VIDEO_EXTRA", "0") == "1")
        {   // the + button's extra voices: Maj7 colour and Random (in key)
            setP("harm_voices", 6.0f);
            setP("harm_v5_on", 1.0f); setP("harm_v5_mode", (float) HarmonyEngine::maj7Colour); setP("harm_v5_level", -9.0f);
            setP("harm_v6_on", 1.0f); setP("harm_v6_mode", (float) HarmonyEngine::randomInKey); setP("harm_v6_level", -10.0f);
            setP("tune_root", 2.0f); setP("tune_mode", 1.0f); // D minor for the random voice
        }
        setP("harm_source", 0.0f);
        if (const auto key = juce::SystemStats::getEnvironmentVariable("VIDEO_KEY", ""); key.isNotEmpty())
        {   // "root mode": harmonies from key & scale instead of the sidechain (solo vocal, no chords)
            setP("harm_source", 1.0f);
            setP("tune_root", (float) key.upToFirstOccurrenceOf(" ", false, false).getIntValue());
            setP("tune_mode", (float) key.fromFirstOccurrenceOf(" ", false, false).getIntValue());
        }
        if (const int solo = juce::SystemStats::getEnvironmentVariable("VIDEO_SOLO", "0").getIntValue(); solo > 0)
        {   // one harmony voice only (stems for the Choirize test)
            setP("harm_voices", (float) juce::jmax(4, solo));
            for (int k = 1; k <= 8; ++k) setP(("harm_v" + juce::String(k) + "_on").toRawUTF8(), k == solo ? 1.0f : 0.0f);
            setP(("harm_v" + juce::String(solo) + "_level").toRawUTF8(), -3.0f);
        }

        std::unique_ptr<juce::AudioProcessorEditor> ved(proc->createEditor());
        ved->setSize(w, h);
        ved->setVisible(true);
        std::function<void(juce::Component&)> toHarmony = [&](juce::Component& c)
        {
            for (auto* ch : c.getChildren())
                if (auto* pb = dynamic_cast<PresetBar*>(ch)) { pb->setActiveTab(1); if (pb->onTabChanged) pb->onTabChanged(1); }
                else toHarmony(*ch);
        };
        toHarmony(*ved);
        pump(200);
        if (juce::SystemStats::getEnvironmentVariable("VIDEO_EXTRA", "0") == "1")
        {
            std::function<void(juce::Component&)> scrollEnd = [&](juce::Component& c)
            {
                for (auto* ch : c.getChildren())
                    if (auto* vp = dynamic_cast<juce::Viewport*>(ch)) { if (auto* vc = vp->getViewedComponent()) vp->setViewPosition(vc->getWidth() - vp->getWidth(), 0); }
                    else scrollEnd(*ch);
            };
            pump(100); scrollEnd(*ved); pump(100);
        }

        const bool testExport = juce::SystemStats::getEnvironmentVariable("VIDEO_EXPORT", "0") == "1";
        if (testExport) { setP("harm_lead", -60.0f); np->getHarmonyExporter().arm(); }
        playHead.pos = 0; playHead.playing = true;
        const int lat = proc->getLatencySamples();
        juce::AudioBuffer<float> out(2, total + lat + block * 4); out.clear();
        juce::AudioBuffer<float> big(4, block);
        const auto frames = outDir.getChildFile("frames");
        frames.deleteRecursively(); frames.createDirectory();
        int frameNo = 0, pos = 0;
        while (pos + block <= total + lat)
        {
            for (int i = 0; i < block; ++i)
            {
                const int src = pos + i;
                const bool in = src < total;
                big.setSample(0, i, in ? vocal.getSample(0, src) : 0.0f);
                big.setSample(1, i, in ? vocal.getSample(1, src) : 0.0f);
                big.setSample(2, i, in ? pad.getSample(0, src) : 0.0f);
                big.setSample(3, i, in ? pad.getSample(1, src) : 0.0f);
            }
            playHead.pos = pos;
            proc->processBlock(big, midi);
            out.copyFrom(0, pos, big, 0, 0, block);
            out.copyFrom(1, pos, big, 1, 0, block);
            pos += block;
            if (pos % (block * 4) == 0 && pos <= total)
            {
                pump(60); // let the GUI timers pick up the engine state
                if (testExport && frameNo == 100)
                {
                    auto im = ved->createComponentSnapshot(ved->getLocalBounds(), true, 1.0f);
                    juce::FileOutputStream os(outDir.getChildFile("export_recording.png")); os.setPosition(0); os.truncate();
                    juce::PNGImageFormat().writeImageToStream(im, os);
                }
                if (juce::SystemStats::getEnvironmentVariable("VIDEO_NOFRAMES", "0") == "1") continue;
                auto img = ved->createComponentSnapshot(ved->getLocalBounds(), true, 1.0f);
                auto f = frames.getChildFile("f" + juce::String(frameNo++).paddedLeft('0', 5) + ".png");
                juce::FileOutputStream os(f);
                juce::PNGImageFormat().writeImageToStream(img, os);
            }
        }
        // audio heard at video time t = plugin output for input time t (drop the latency), plus the pad
        const float padMix = (float) juce::SystemStats::getEnvironmentVariable("VIDEO_PAD_MIX", "0.8").getDoubleValue();
        juce::AudioBuffer<float> mix(2, total);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < total; ++i)
                mix.setSample(c, i, out.getSample(c, i + lat) + padMix * pad.getSample(c, i));
        mix.applyGain(0.89f / juce::jmax(1.0e-6f, mix.getMagnitude(0, total)));
        juce::WavAudioFormat wav;
        auto af = outDir.getChildFile("video_audio.wav"); af.deleteFile();
        std::unique_ptr<juce::AudioFormatWriter> wr(wav.createWriterFor(new juce::FileOutputStream(af), sr, 2, 16, {}, 0));
        wr->writeFromAudioSampleBuffer(mix, 0, total);
        std::cout << "frames: " << frameNo << ", latency " << lat << "\n";
        if (testExport)
        {
            playHead.playing = false;
            big.clear(); proc->processBlock(big, midi);
            pump(400);
            auto& ex = np->getHarmonyExporter();
            std::cout << "export state " << (int) ex.getState() << " file " << ex.getFile().getFullPathName() << " sec " << ex.getSecondsRecorded() << "\n";
            {
                auto img = ved->createComponentSnapshot(ved->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream os(outDir.getChildFile("export_ready.png")); os.setPosition(0); os.truncate();
                juce::PNGImageFormat().writeImageToStream(img, os);
            }
            auto o = outDir.getChildFile("plugin_out_harmonies_only.wav"); o.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> w2(wav.createWriterFor(new juce::FileOutputStream(o), sr, 2, 24, {}, 0));
            juce::AudioBuffer<float> aligned(2, total);
            for (int c = 0; c < 2; ++c) aligned.copyFrom(c, 0, out, c, lat, total);
            w2->writeFromAudioSampleBuffer(aligned, 0, total);
        }
        ved.reset();
        proc->releaseResources();
        return 0;
    }

    if (harmonyTest)
    {
        // Enable the sidechain bus, play C - Am - F - G into it and a sung melody into the main input
        auto layout = proc->getBusesLayout();
        if (layout.inputBuses.size() > 1) layout.inputBuses.getReference(1) = juce::AudioChannelSet::stereo();
        std::cout << "sidechain layout accepted: " << (proc->setBusesLayout(layout) ? "yes" : "no") << "\n";
        proc->prepareToPlay(48000.0, 512);
        for (auto [id, v] : { std::pair<const char*, float> { "harm_v1_on", 1.0f }, { "harm_v2_on", 1.0f } })
            if (auto* prm = np->apvts.getParameter(id)) prm->setValueNotifyingHost(v);
        std::vector<std::vector<int>> chords;
        auto beat = makeProgression(48000.0, chords);
        const int leadNotes[] = { 64, 64, 65, 62 };
        juce::AudioBuffer<float> big(4, 512);
        double lph = 0;
        for (int pos = 0; pos + 512 <= beat.getNumSamples(); pos += 512)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (pos + i) / 48000.0;
                const int bar = juce::jlimit(0, 3, (int) ((t - 0.1) / 1.6));
                const double f = 440.0 * std::pow(2.0, (leadNotes[bar] - 69 + 0.3 * std::sin(t * 2 * 3.14159 * 5.2)) / 12.0);
                lph += 2 * 3.14159265 * f / 48000.0;
                const float s = (float) (0.15 * std::sin(lph) + 0.07 * std::sin(2 * lph) + 0.04 * std::sin(3 * lph));
                big.setSample(0, i, s); big.setSample(1, i, s);
                big.setSample(2, i, beat.getSample(0, pos + i)); big.setSample(3, i, beat.getSample(1, pos + i));
            }
            proc->processBlock(big, midi);
            if (pos > (int) (0.1 + 1.6 * 2 + 0.9) * 48000) break; // stop inside the F chord for the screenshot
        }
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    ed->setSize(w, h);
    ed->setVisible(true);
    if (harmonyTest)
    {
        std::function<void(juce::Component&)> show = [&](juce::Component& c)
        {
            for (auto* ch : c.getChildren())
            {
                if (dynamic_cast<HarmonyTab*>(ch) != nullptr) ch->setVisible(true);
                else if (dynamic_cast<TuneTab*>(ch) != nullptr) ch->setVisible(false);
                else show(*ch);
            }
        };
        show(*ed);
        pump(300);
    }
    for (int b = 0; b < ((polyTest || harmonyTest) ? 0 : 300); ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            const double t = (b * 512 + i) / 48000.0;
            const double f = 220.0 * std::pow(2.0, (2.0 * std::sin(t * 1.3) + 0.3 * std::sin(t * 31.0)) / 12.0);
            const float s = (float) (0.3 * std::sin(ph) + 0.1 * std::sin(2 * ph));
            ph += juce::MathConstants<double>::twoPi * f / 48000.0;
            buf.setSample(0, i, s); buf.setSample(1, i, s);
        }
        proc->processBlock(buf, midi);
        if (b % 4 == 0) pump(10);
    }
    pump(400);
    if (argc > 6 && std::atoi(argv[6]) != 0)
        if (auto* roll = findRoll(*ed))
        {
            roll->selectAdjacentNote(1);
            roll->selectAdjacentNote(1);
            pump(200);
        }
    const float scale = argc > 4 ? (float) std::atof(argv[4]) : 1.0f;
    save(*ed, outDir.getChildFile(argc > 7 ? juce::String(argv[7]) : juce::String("editor.png")), scale);

    if (polyTest)
        if (auto* roll = findRoll(*ed))
        {
            auto& clip = np->getPolyClip();
            const auto notesBefore = clip.getNotes();
            const int editedBefore = (int) std::count_if(notesBefore.begin(), notesBefore.end(), [](auto& n) { return n.userEdited; });
            roll->selectAdjacentNote(1);
            roll->transposeSelectedNote(1);
            auto t0 = juce::Time::getMillisecondCounter();
            pump(100);
            while (! clip.isRenderCurrent() && juce::Time::getMillisecondCounter() - t0 < 30000) pump(50);
            const auto notesNow = clip.getNotes();
            const int editedAfter = (int) std::count_if(notesNow.begin(), notesNow.end(), [](auto& n) { return n.userEdited; });
            std::cout << "piano-roll edit -> session: edited notes " << editedBefore << " -> " << editedAfter
                      << ", render current: " << (clip.isRenderCurrent() ? "yes" : "no") << "\n";
        }

    ed.reset();
    proc->releaseResources();
    return 0;
}
