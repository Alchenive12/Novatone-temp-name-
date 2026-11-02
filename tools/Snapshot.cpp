// Dev-only: renders the NovaTone editor to PNGs without a host.
// Build with -DNOVATONE_BUILD_SNAPSHOT=ON, run: NovaToneSnapshot <outDir> [width height]
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GUI/PitchTraceComponent.h"
#include "GUI/ChordLane.h"
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
        // MIDI mode: VIDEO_MIDI = file of lines "seconds note note ..." (song time); notes held until the next line
        std::vector<std::pair<int, std::vector<int>>> midiChart;
        if (const juce::File mf(juce::SystemStats::getEnvironmentVariable("VIDEO_MIDI", "")); mf.existsAsFile())
        {
            setP("harm_source", 2.0f);
            for (auto line : juce::StringArray::fromLines(mf.loadFileAsString()))
            {
                auto tok = juce::StringArray::fromTokens(line.trim(), " ", "");
                if (tok.size() < 1 || line.trim().startsWith("#")) continue;
                std::vector<int> ns; for (int k = 1; k < tok.size(); ++k) ns.push_back(tok[k].getIntValue());
                midiChart.push_back({ (int) ((tok[0].getDoubleValue() - startSec) * sr), ns });
            }
        }
        size_t midiNext = 0; std::vector<int> midiOn;
        if (const int solo = juce::SystemStats::getEnvironmentVariable("VIDEO_SOLO", "0").getIntValue(); solo > 0)
        {   // one harmony voice only (stems for the Choirize test)
            setP("harm_voices", (float) juce::jmax(4, solo));
            for (int k = 1; k <= 8; ++k) setP(("harm_v" + juce::String(k) + "_on").toRawUTF8(), k == solo ? 1.0f : 0.0f);
            setP(("harm_v" + juce::String(solo) + "_level").toRawUTF8(), -3.0f);
        }

        std::unique_ptr<juce::AudioProcessorEditor> ved(proc->createEditor());
        ved->setSize(w, h);
        ved->setVisible(true);
        auto presetBarTab = [](juce::Component& root, int tab)
        {
            std::function<void(juce::Component&)> f = [&](juce::Component& c)
            {
                for (auto* ch : c.getChildren())
                    if (auto* pb = dynamic_cast<PresetBar*>(ch)) { pb->setActiveTab(tab); if (pb->onTabChanged) pb->onTabChanged(tab); }
                    else f(*ch);
            };
            f(root);
        };
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
        const bool testChoir = juce::SystemStats::getEnvironmentVariable("VIDEO_CHOIR", "0") == "1";
        const auto choirDir = outDir.getChildFile("choir").getChildFile("Test group").getChildFile("Take 1");
        const bool testChoirize = juce::SystemStats::getEnvironmentVariable("VIDEO_CHOIR", "0") == "2";
        if (testChoir) { outDir.getChildFile("choir").deleteRecursively(); np->getChoirRecorder().arm(choirDir.getChildFile("parts.wav")); }
        if (testChoirize)
        {   // Harmony tab Choirize: voice 1 = Hila + double, voice 2 = Off (own voice)
            ChoirSession::rootFolder().getChildFile("Choirize test").deleteRecursively();
            np->getChoir().close(); np->getChoir().newGroup("Choirize test");
            np->apvts.state.setProperty("choirSinger1", "fmid_b", nullptr); np->apvts.state.setProperty("choirDouble1", true, nullptr);
            np->apvts.state.setProperty("choirSinger2", "off", nullptr);
            ChoirSession::PendingArm a; a.parts = { 1, 2 }; a.regs = { 2, 0 }; a.singers = { "fmid_b", "off" }; a.doubles = { true, false };
            np->getChoir().beginArm(np->getChoirRecorder(), a);
        }
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
            midi.clear();
            while (midiNext < midiChart.size() && midiChart[midiNext].first < pos + block)
            {
                const int at = juce::jlimit(0, block - 1, midiChart[midiNext].first - pos);
                for (int nn : midiOn) midi.addEvent(juce::MidiMessage::noteOff(1, nn), at);
                midiOn = midiChart[midiNext].second;
                for (int nn : midiOn) midi.addEvent(juce::MidiMessage::noteOn(1, nn, (juce::uint8) 100), at);
                ++midiNext;
            }
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
        if (testChoir)
        {
            // parts recorded -> choir session -> a stand-in helper "converts" each request (copies the part, -6 dB)
            playHead.playing = false;
            big.clear(); proc->processBlock(big, midi);
            pump(400);
            auto& rec = np->getChoirRecorder(); auto& choir = np->getChoir();
            std::cout << "choir parts state " << (int) rec.getState() << " sec " << rec.getSecondsRecorded() << "\n";
            std::vector<int> on, regs, los, his;
            for (int v = 1; v <= 8; ++v) if (np->apvts.getRawParameterValue("harm_v" + juce::String(v) + "_on")->load() > 0.5f)
            { on.push_back(v); regs.push_back(on.size() == 1 ? 2 : 0); los.push_back(on.size() == 1 ? 62 : 43); his.push_back(on.size() == 1 ? 70 : 55); }
            choir.close();
            choir.createFromRecording(choirDir, rec.getSampleRate(), rec.getStartSample(), on, regs, los, his);
            juce::AudioBuffer<float> parts;
            {
                std::unique_ptr<juce::AudioFormatReader> pr(wav.createReaderFor(new juce::FileInputStream(choirDir.getChildFile("parts.wav")), true));
                parts.setSize((int) pr->numChannels, (int) pr->lengthInSamples); pr->read(&parts, 0, parts.getNumSamples(), 0, true, true);
            }
            int conv = 0;
            for (auto f : choirDir.getChildFile("requests").findChildFiles(juce::File::findFiles, false, "*.req"))
            {
                const int part = f.loadFileAsString().getIntValue();
                choirDir.getChildFile("voices").createDirectory();
                std::unique_ptr<juce::AudioFormatWriter> vw(wav.createWriterFor(new juce::FileOutputStream(choirDir.getChildFile("voices").getChildFile(f.getFileNameWithoutExtension() + ".wav")), sr, 1, 24, {}, 0));
                vw->writeFromFloatArrays(std::vector<const float*>{ parts.getReadPointer(part) }.data(), 1, parts.getNumSamples());
                vw.reset(); f.deleteFile(); ++conv;
            }
            pump(1600);   // the session's poll picks the new voices up
            std::cout << "choir strips converted " << conv << " rows playing:";
            for (int i = 0; i < ChoirSession::maxStrips; ++i) if (choir.strip(i).used.load()) std::cout << " [" << choir.strip(i).part << " " << choir.strip(i).voice << " '" << choir.strip(i).status << "']";
            std::cout << "\n";
            presetBarTab(*ved, 2); pump(300);
            {
                auto img = ved->createComponentSnapshot(ved->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream os(outDir.getChildFile("choir_tab.png")); os.setPosition(0); os.truncate();
                juce::PNGImageFormat().writeImageToStream(img, os);
            }
            // replay with the choir on, lead muted: output must be the choir only, lined up with the recorded parts
            setP("harm_lead", -60.0f);
            choir.busDb.store(0.0f); choir.dip.store(false);
            for (int i = 0; i < ChoirSession::maxStrips; ++i) { choir.strip(i).pan.store(0.0f); choir.strip(i).offsetMs = 0; }
            playHead.playing = true; juce::AudioBuffer<float> rep(2, total + lat + block * 4); rep.clear();
            for (int pos2 = 0; pos2 + block <= total + lat; pos2 += block)
            {
                big.clear();
                for (int i = 0; i < block; ++i) if (pos2 + i < total) { big.setSample(0, i, vocal.getSample(0, pos2 + i)); big.setSample(1, i, vocal.getSample(1, pos2 + i)); }
                playHead.pos = pos2; proc->processBlock(big, midi);
                rep.copyFrom(0, pos2, big, 0, 0, block); rep.copyFrom(1, pos2, big, 1, 0, block);
            }
            // expected: sum of the recorded parts * 0.5 (equal-power centre pan), from song sample 0
            double err = 0, ref = 0; int bestLag = 0; double best = -1;
            for (int lag = -2000; lag <= 2000; lag += 1)
            {
                double c = 0;
                for (int i = 20000; i < total - 20000; i += 7) { float e = 0; for (int p : on) e += parts.getSample(p, i); c += e * rep.getSample(0, i + lat + lag); }
                if (c > best) { best = c; bestLag = lag; }
            }
            for (int i = 0; i < juce::jmin(total, parts.getNumSamples()); ++i)
            {
                float e = 0; for (int p : on) e += 2.0f * parts.getSample(p, i) * std::cos(juce::MathConstants<float>::pi * 0.25f);   // 2 singers per part
                e += vocal.getSample(0, i);   // the lead passes through untouched while the choir plays
                err += std::pow(rep.getSample(0, i + lat) - e, 2); ref += e * e;
            }
            {   // group tools: render the group mix, then move the take to another group and back
                double gr = 0; const auto gf = choirDir.getParentDirectory().getChildFile("render.wav");
                const auto gs = choir.renderGroup(gf, gr);
                std::cout << "group render start " << gs << " rate " << gr << " bytes " << gf.getSize() << "\n";
                const bool mv = choir.moveTake(0, "Moved group");
                std::cout << "move take: " << mv << " takes left in group " << choir.getNumTakes() << "\n";
                choir.open(ChoirSession::rootFolder().getChildFile("Moved group"));
                std::cout << "moved group takes " << choir.getNumTakes() << " strips:";
                for (int i = 0; i < ChoirSession::maxStrips; ++i) if (choir.strip(i).used.load()) std::cout << " " << choir.strip(i).voice;
                std::cout << "\n";
                ChoirSession::rootFolder().getChildFile("Moved group").deleteRecursively();
            }
            std::cout << "choir replay (lead + 2 singers per part): best lag " << bestLag << " samples, error " << juce::Decibels::gainToDecibels((float) std::sqrt(err / juce::jmax(1e-12, ref))) << " dB vs choir\n";
        }
        if (testChoirize)
        {
            playHead.playing = false;
            big.clear(); proc->processBlock(big, midi);
            pump(400);
            auto& choir = np->getChoir(); auto& rec = np->getChoirRecorder();
            const bool made = choir.pollArm(rec, [](int) { return 0; });
            std::cout << "choirize take made " << made << " takes " << choir.getNumTakes() << "\n";
            const auto take = choir.take(choir.latestTake()).folder;
            juce::AudioBuffer<float> parts;
            {
                std::unique_ptr<juce::AudioFormatReader> pr(wav.createReaderFor(new juce::FileInputStream(take.getChildFile("parts.wav")), true));
                parts.setSize((int) pr->numChannels, (int) pr->lengthInSamples); pr->read(&parts, 0, parts.getNumSamples(), 0, true, true);
            }
            for (auto f : take.getChildFile("requests").findChildFiles(juce::File::findFiles, false, "*.req"))
            {
                const int part = f.loadFileAsString().getIntValue();
                take.getChildFile("voices").createDirectory();
                std::unique_ptr<juce::AudioFormatWriter> vw(wav.createWriterFor(new juce::FileOutputStream(take.getChildFile("voices").getChildFile(f.getFileNameWithoutExtension() + ".wav")), sr, 1, 24, {}, 0));
                vw->writeFromFloatArrays(std::vector<const float*>{ parts.getReadPointer(part) }.data(), 1, parts.getNumSamples());
                vw.reset(); f.deleteFile();
            }
            pump(1600);
            for (int p = 1; p <= 2; ++p)
            {
                std::cout << "voice " << p << ": singers '" << choir.cardVoice(p, 0) << "' '" << choir.cardVoice(p, 1) << "' notes " << (choir.partNotes(p) ? choir.partNotes(p)->size() : 0);
                if (choir.partNotes(p)) for (size_t k = 0; k < juce::jmin<size_t>(6, choir.partNotes(p)->size()); ++k)
                    std::cout << " " << ChoirSession::noteName((*choir.partNotes(p))[k].midi) << "@" << juce::String((double) (*choir.partNotes(p))[k].start / sr, 2);
                std::cout << "\n";
            }
            if (choir.partNotes(1) && choir.partNotes(1)->size() > 2) { choir.cycleNote(1, 1); choir.cycleNote(1, 2); choir.cycleNote(1, 2); }   // note 1 -> your voice, note 2 -> muted
            const auto stems = choir.exportStems(choir.getFolder().getChildFile("Stems"));
            std::cout << "stems:"; for (auto& f : stems) std::cout << " [" << f.getFileName() << " " << f.getSize() << "]"; std::cout << "\n";
            presetBarTab(*ved, 1); pump(800);
            {
                auto img = ved->createComponentSnapshot(ved->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream os(outDir.getChildFile("harmony_choirize.png")); os.setPosition(0); os.truncate();
                juce::PNGImageFormat().writeImageToStream(img, os);
            }
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

    if (argc > 8 && juce::String(argv[8]) == "chordedit")
    {
        juce::MidiMessageSequence seq;
        const int chordsMidi[4][3] = { { 48, 52, 55 }, { 45, 48, 52 }, { 41, 45, 48 }, { 43, 47, 50 } };
        for (int bar = 0; bar < 8; ++bar)
            for (int n : chordsMidi[bar % 4])
            { seq.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8) 90), bar * 1920.0); seq.addEvent(juce::MidiMessage::noteOff(1, n), (bar + 1) * 1920.0 - 1.0); }
        seq.updateMatchedPairs();
        juce::MidiFile mf; mf.setTicksPerQuarterNote(480); mf.addTrack(seq);
        const auto mid = outDir.getChildFile("chords.mid"); mid.deleteFile();
        { juce::FileOutputStream os(mid); mf.writeTo(os); }
        juce::String err; np->loadHarmonyMidi(mid, err);
        for (auto [id, v] : { std::pair<const char*, float> { "harm_source", 3.0f }, { "harm_v1_on", 1.0f } })
            if (auto* prm = np->apvts.getParameter(id)) prm->setValueNotifyingHost(prm->convertTo0to1(v));
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        std::function<void(juce::Component&)> show = [&](juce::Component& c)
        { for (auto* ch : c.getChildren()) { if (dynamic_cast<HarmonyTab*>(ch) != nullptr) ch->setVisible(true); else if (dynamic_cast<TuneTab*>(ch) != nullptr) ch->setVisible(false); else show(*ch); } };
        show(*med); pump(300);
        std::function<ChordLane*(juce::Component&)> findLane = [&](juce::Component& c) -> ChordLane*
        { if (auto* l = dynamic_cast<ChordLane*>(&c)) return l; for (auto* ch : c.getChildren()) if (auto* l = findLane(*ch)) return l; return nullptr; };
        auto* lane = findLane(*med);
        auto clickButton = [&](const juce::String& text) { for (auto* ch : lane->getChildren()) if (auto* b = dynamic_cast<juce::TextButton*>(ch); b != nullptr && b->getButtonText() == text) { b->triggerClick(); pump(100); return true; } return false; };
        auto dump = [&](const char* label)
        {
            std::cout << label << ":";
            for (auto& sg : np->getHarmonyMidi()->segments) if (sg.start >= 28) std::cout << " [" << sg.start << "-" << sg.end << " " << MidiChordTrack::chordName(sg.root, sg.quality) << "]";
            std::cout << "\n";
        };
        // let the lane follow to the area after the file (song beats 32..)
        playHead.pos = (juce::int64) (34 * 0.5 * 48000); playHead.playing = false; buf.clear(); proc->processBlock(buf, midi); pump(200);
        clickButton("Expand");
        std::cout << "expanded: " << lane->isExpanded() << " lane bounds " << lane->getBounds().toString() << "\n";
        save(*med, outDir.getChildFile("chord_editor.png"), 1.0f);
        clickButton("Write");
        pump(200);
        // geometry helpers via the lane's own mapping: find x of a song beat by scanning
        auto xOfBeat = [&](double beat) { for (int x = 0; x < lane->getWidth(); ++x) { juce::MouseEvent* dummy = nullptr; (void) dummy; } return 0; };
        (void) xOfBeat;
        // drop an F chip at song beat 40 (lane maps x linearly; read it back from the view by probing chord row y)
        const auto lb = lane->getLocalBounds();
        const int rowY = 8 + 26 + 6 + 30 + 6 + 16;   // top bar, palette, then the chord row's middle
        const int laneLeft = 12 + 38, laneW = lb.getWidth() - 24 - 38;
        // the view starts at a bar boundary set by the follow logic; recover it from two probes is overkill: use known state
        const double viewStart = 28.0, viewBeats = 32.0;   // followed to bar 8 before expanding
        auto xFor = [&](double beat) { return (float) laneLeft + (float) ((beat - viewStart) / viewBeats) * (float) laneW; };
        lane->itemDropped(juce::DragAndDropTarget::SourceDetails("chord:5:0", nullptr, { juce::roundToInt(xFor(40.1)), rowY }));
        pump(100); dump("dropped F at beat 40");
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&](float x, float y, juce::ModifierKeys m, juce::Point<float> down, bool dragged)
        { return juce::MouseEvent(src, { x, (float) y }, m, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, lane, lane, juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1, dragged); };
        const auto L = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        { const juce::Point<float> d { xFor(41.5), (float) rowY }; lane->mouseDown(ev(d.x, d.y, L, d, false)); lane->mouseDrag(ev(xFor(45.5), d.y, L, d, true)); lane->mouseUp(ev(xFor(45.5), d.y, {}, d, true)); }
        pump(100); dump("moved +4 beats");
        { const juce::Point<float> d { xFor(48.0) - 2.0f, (float) rowY }; lane->mouseDown(ev(d.x, d.y, L, d, false)); lane->mouseDrag(ev(xFor(50.0), d.y, L, d, true)); lane->mouseUp(ev(xFor(50.0), d.y, {}, d, true)); }
        pump(100); dump("stretched to beat 50");
        lane->keyPressed(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0)); pump(100); dump("undo");
        lane->keyPressed(juce::KeyPress('z', juce::ModifierKeys(juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier), 0)); pump(100); dump("redo");
        // engine follows the written chord
        playHead.pos = (juce::int64) (46.5 * 0.5 * 48000); playHead.playing = true; buf.clear(); proc->processBlock(buf, midi); playHead.playing = false;
        std::cout << "chord at song beat 46.5: " << np->getHarmonyEngine().getChord() << " (F = " << 5 * ChordTracker::numQualities + 0 << ")\n";
        save(*med, outDir.getChildFile("chord_editor_write.png"), 1.0f);
        juce::MemoryBlock st; proc->getStateInformation(st); np->clearHarmonyMidi(); proc->setStateInformation(st.getData(), (int) st.getSize());
        dump("after project reload");
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "export")
    {
        for (auto kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("TEST_PARAMS", ""), ",", ""))
            if (auto* prm = np->apvts.getParameter(kv.upToFirstOccurrenceOf("=", false, false)))
                prm->setValueNotifyingHost(prm->convertTo0to1(kv.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        auto* roll = findRoll(*med);
        auto& q = np->getPitchTraceQueue();
        const double sr = 48000.0; const int N = (int) (8 * sr);
        juce::AudioBuffer<float> voc(1, N); double vph = 0;
        const double freqs[] = { 216.0, 250.5, 258.4, 297.2 };
        for (int i = 0; i < N; ++i)
        {
            const double t = i / sr; const int k = (int) (t / 2.0) % 4; const double ph2 = std::fmod(t, 2.0);
            float smp = 0.0f;
            if (ph2 < 1.6)
            {
                vph += 2 * 3.14159265 * freqs[k] * std::pow(2.0, 0.25 * std::sin(t * 2 * 3.14159 * 5.5) / 12.0) / sr;
                const double env = juce::jmin(1.0, ph2 / 0.05) * juce::jmin(1.0, (1.6 - ph2) / 0.05);
                double hs = 0; for (int hN = 1; hN <= 14; ++hN) hs += std::sin(hN * vph) / (hN * (hN > 2 ? 1.6 : 1.0));
                smp = (float) (0.2 * env * hs);
            }
            voc.setSample(0, i, smp);
        }
        const auto wav = outDir.getChildFile("exportvocal.wav"); wav.deleteFile();
        { std::unique_ptr<juce::AudioFormatWriter> wr(juce::WavAudioFormat().createWriterFor(new juce::FileOutputStream(wav), sr, 1, 24, {}, 0)); wr->writeFromAudioSampleBuffer(voc, 0, N); }
        roll->filesDropped(juce::StringArray(wav.getFullPathName()), 0, 0);
        pump(400);
        roll->selectAdjacentNote(1); roll->transposeSelectedNote(2); pump(200);

        // Playback through the plugin (NovaTone's own transport, silent input)
        for (int b = 0; b < 8; ++b) { buf.clear(); proc->processBlock(buf, midi); }
        roll->seekClipToStart(); roll->setPreviewPlaying(true);
        const int lat = proc->getLatencySamples();
        juce::AudioBuffer<float> play(1, N + lat + 4096); int w0 = 0;
        while (w0 + 512 <= play.getNumSamples()) { buf.clear(); playHead.playing = false; proc->processBlock(buf, midi); play.copyFrom(0, w0, buf, 0, 0, 512); w0 += 512; }
        roll->setPreviewPlaying(false);

        // Export (same call the File menu makes)
        juce::AudioBuffer<float> ex;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const bool ok = np->renderMonoClipOffline(voc, sr, q.monoPreview.hopSamples, ex, [](float) { return true; });
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

        double sig = 0, err = 0, errDry = 0;
        for (int i = 0; i < N; ++i)
        {
            const float y = play.getSample(0, i + lat), e = ex.getSample(0, i), x = voc.getSample(0, i);
            sig += y * y; err += (e - y) * (e - y); errDry += (x - y) * (x - y);
        }
        // Perceptual comparison: log-spectral distance (dB, 2048-pt frames) and pitch (zero crossings, 40 ms)
        auto lsd = [&](const juce::AudioBuffer<float>& A, int offA, const juce::AudioBuffer<float>& Bf, int offB)
        {
            juce::dsp::FFT fft(11); std::vector<float> fa(4096), fb(4096); double tot = 0; int frames = 0;
            for (int st = 0; st + 2048 < N; st += 1024)
            {
                double ea = 0; for (int i = 0; i < 2048; ++i) ea += std::pow(A.getSample(0, st + i + offA), 2.0f);
                if (ea < 2048 * 1e-4) continue;   // voiced/loud frames only
                std::fill(fa.begin(), fa.end(), 0.0f); std::fill(fb.begin(), fb.end(), 0.0f);
                for (int i = 0; i < 2048; ++i) { const float wv = 0.5f - 0.5f * std::cos(6.2831853f * i / 2047.0f); fa[(size_t) i] = A.getSample(0, st + i + offA) * wv; fb[(size_t) i] = Bf.getSample(0, st + i + offB) * wv; }
                fft.performFrequencyOnlyForwardTransform(fa.data()); fft.performFrequencyOnlyForwardTransform(fb.data());
                double d = 0; int nb = 0;
                for (int k = 4; k < 700; ++k) { const double x = 20 * std::log10(fa[(size_t) k] + 1e-4), y = 20 * std::log10(fb[(size_t) k] + 1e-4); d += (x - y) * (x - y); ++nb; }
                tot += std::sqrt(d / nb); ++frames;
            }
            return tot / juce::jmax(1, frames);
        };
        auto pitchDiff = [&](const juce::AudioBuffer<float>& A, int offA, const juce::AudioBuffer<float>& Bf, int offB)
        {
            const int win = 4800; double sum = 0; int nw = 0;
            auto zc = [&](const juce::AudioBuffer<float>& X, int from) { int z = 0; for (int i = from + 1; i < from + win; ++i) if ((X.getSample(0, i) >= 0) != (X.getSample(0, i - 1) >= 0)) ++z; return z; };
            for (int k = 0; k < 4; ++k)
                for (int i = (int) ((k * 2.0 + 0.2) * sr); i + win < (int) ((k * 2.0 + 1.4) * sr); i += win)
                { const double fa = zc(A, i + offA), fb = zc(Bf, i + offB); if (fa > 0 && fb > 0) { sum += std::abs(1200.0 * std::log2(fa / fb)); ++nw; } }
            return sum / juce::jmax(1, nw);
        };
        std::cout << "spectral distance export vs playback: " << juce::String(lsd(ex, 0, play, lat), 2) << " dB   (original vs playback: " << juce::String(lsd(voc, 0, play, lat), 2) << " dB)\n";
        std::cout << "pitch difference export vs playback: " << juce::String(pitchDiff(ex, 0, play, lat), 1) << " c   (original vs playback: " << juce::String(pitchDiff(voc, 0, play, lat), 1) << " c)\n";
        std::cout << "export ok=" << ok << " in " << juce::String(ms, 0) << " ms (8 s of audio), latency " << lat << "\n";
        std::cout << "export vs playback: " << juce::String(10 * std::log10(err / sig), 1) << " dB difference\n";
        std::cout << "(original vs playback, for scale: " << juce::String(10 * std::log10(errDry / sig), 1) << " dB)\n";
        { std::unique_ptr<juce::AudioFormatWriter> wr(juce::WavAudioFormat().createWriterFor(new juce::FileOutputStream(outDir.getChildFile("exported.wav")), sr, 1, 24, {}, 0)); wr->writeFromAudioSampleBuffer(ex, 0, N); }
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "harmmidi")
    {
        // Chord file: C | Am | F | G (one bar each, 4/4, 480 tpq), twice
        juce::MidiMessageSequence seq;
        const int chordsMidi[4][3] = { { 48, 52, 55 }, { 45, 48, 52 }, { 41, 45, 48 }, { 43, 47, 50 } };
        for (int bar = 0; bar < 8; ++bar)
            for (int n : chordsMidi[bar % 4])
            {
                seq.addEvent(juce::MidiMessage::noteOn(1, n, (juce::uint8) 90), bar * 4 * 480.0);
                seq.addEvent(juce::MidiMessage::noteOff(1, n), (bar + 1) * 4 * 480.0 - 1.0);
            }
        seq.updateMatchedPairs();
        juce::MidiFile mf; mf.setTicksPerQuarterNote(480); mf.addTrack(seq);
        const auto mid = outDir.getChildFile("chords.mid"); mid.deleteFile();
        { juce::FileOutputStream os(mid); mf.writeTo(os); }
        juce::String err;
        std::cout << "load: " << np->loadHarmonyMidi(mid, err) << " " << err << " segments=" << np->getHarmonyMidi()->segments.size() << "\n";
        for (auto& sg : np->getHarmonyMidi()->segments) std::cout << "  " << sg.start << "-" << sg.end << " " << MidiChordTrack::chordName(sg.root, sg.quality) << "\n";
        for (auto [id, v] : { std::pair<const char*, float> { "harm_source", 3.0f }, { "harm_v1_on", 1.0f } })
            if (auto* prm = np->apvts.getParameter(id)) prm->setValueNotifyingHost(prm->convertTo0to1(v));

        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        std::function<void(juce::Component&)> show = [&](juce::Component& c)
        {
            for (auto* ch : c.getChildren())
            {
                if (dynamic_cast<HarmonyTab*>(ch) != nullptr) ch->setVisible(true);
                else if (dynamic_cast<TuneTab*>(ch) != nullptr) ch->setVisible(false);
                else show(*ch);
            }
        };
        show(*med); pump(300);
        const auto& eng = np->getHarmonyEngine();
        // Lead melody that belongs to the chords 2 beats LATER than the file has them: chord tones of chord(beat - 2)
        const int leadTones[4][2] = { { 64, 67 }, { 69, 72 }, { 65, 69 }, { 67, 71 } };
        playHead.pos = 0; double lph = 0;
        const double spb = 0.5;   // 120 BPM
        for (int blk = 0; blk < (int) (32 * spb * 48000 / 512); ++blk)
        {
            for (int i = 0; i < 512; ++i)
            {
                const double t = (playHead.pos + i) / 48000.0;
                const double beat = t / spb - 2.0;   // the song's chord for this lead note is 2 beats behind
                const int c = beat < 0 ? 0 : ((int) (beat / 4.0)) % 4;
                const int note = leadTones[c][((int) beat + 8) % 2];
                const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0);
                const bool gap = std::fmod(t, spb) > spb * 0.9;
                lph += 2 * 3.14159265 * f / 48000.0;
                double hs = 0; for (int hN = 1; hN <= 8; ++hN) hs += std::sin(hN * lph) / hN;
                const float smp = gap ? 0.0f : (float) (0.2 * hs);
                buf.setSample(0, i, smp); buf.setSample(1, i, smp);
            }
            playHead.playing = true;
            proc->processBlock(buf, midi);
            playHead.pos += 512;
            if (blk % 8 == 0) pump(5);
            if (blk == (int) (5.2 * spb * 48000 / 512)) std::cout << "beat 5.2 (file: Am): chord shown " << eng.getChord() << " voice1 note " << eng.getVoiceNote(0) << "\n";
        }
        playHead.playing = false; buf.clear(); proc->processBlock(buf, midi); pump(400);
        std::cout << "lead history samples: " << np->getLeadHistory().size() << "\n";
        std::function<ChordLane*(juce::Component&)> findLane = [&](juce::Component& c) -> ChordLane*
        { if (auto* l = dynamic_cast<ChordLane*>(&c)) return l; for (auto* ch : c.getChildren()) if (auto* l = findLane(*ch)) return l; return nullptr; };
        auto* lane = findLane(*med);
        std::cout << "lane visible: " << (lane != nullptr && lane->isVisible()) << "\n";
        {
            const auto rn = lane->hooks.leadNotes(); const auto tr = np->getHarmonyMidi();
            std::cout << "lane lead notes: " << rn.size() << "\n";
            for (size_t i = 0; i < juce::jmin<size_t>(6, rn.size()); ++i) std::cout << "  " << rn[i].start << "-" << rn[i].end << " " << rn[i].midi << "\n";
            for (double off = 1.75; off <= 2.26; off += 0.125)
            {
                double tot = 0, hit = 0;
                for (auto& n : rn) { const int pc = ((int) std::lround(n.midi) % 12 + 12) % 12; for (double p = n.start; p < n.end; p += 0.125) { const double wv = juce::jmin(0.125, n.end - p); if (auto* sg = tr->at(p + 0.5 * wv - off)) { tot += wv; if ((sg->mask >> pc) & 1u) hit += wv; } } }
                std::cout << "  roll off " << off << " fit " << juce::String(hit / tot * 100.0, 1) << "%\n";
            }
        }
        save(*med, outDir.getChildFile("harm_before_align.png"), 1.0f);
        for (auto* ch : lane->getChildren())
            if (auto* b = dynamic_cast<juce::TextButton*>(ch); b != nullptr && b->getButtonText() == "Auto-align") b->triggerClick();
        pump(300);
        {
            const auto leadH = np->getLeadHistory(); const auto tr = np->getHarmonyMidi();
            for (double off = 1.5; off <= 2.51; off += 0.125)
            {
                double tot = 0, hit = 0;
                for (auto& n : leadH) { const int pc = ((int) std::lround(n.midi) % 12 + 12) % 12; if (auto* sg = tr->at(0.5 * (n.start + n.end) - off)) { tot += n.end - n.start; if ((sg->mask >> pc) & 1u) hit += n.end - n.start; } }
                std::cout << "  off " << off << " fit " << juce::String(hit / tot * 100.0, 1) << "%\n";
            }
            // where does the lead history put the first note change? (true: beat 4 -> first Am-tone note at beat 6 in song terms)
            float prev = -1; for (auto& n : leadH) { const int m = (int) std::lround(n.midi); if (m != (int) prev && n.start > 5.0 && n.start < 7.0) std::cout << "  lead change at ppq " << n.start << " to " << m << "\n"; prev = (float) m; }
        }
        std::cout << "offset after Auto-align: " << np->apvts.getRawParameterValue("harm_midiOffset")->load() << " beats\n";
        save(*med, outDir.getChildFile("harm_after_align.png"), 1.0f);
        // state round trip
        juce::MemoryBlock st; proc->getStateInformation(st);
        np->clearHarmonyMidi();
        proc->setStateInformation(st.getData(), (int) st.getSize());
        std::cout << "after state reload: file=" << (np->getHarmonyMidi() ? np->getHarmonyMidi()->fileName : juce::String("none"))
                  << " offset=" << np->apvts.getRawParameterValue("harm_midiOffset")->load() << "\n";
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "lookahead")
    {
        for (auto kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("TEST_PARAMS", ""), ",", ""))
            if (auto* prm = np->apvts.getParameter(kv.upToFirstOccurrenceOf("=", false, false)))
                prm->setValueNotifyingHost(prm->convertTo0to1(kv.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
        std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
        ed->setSize(w, h); ed->setVisible(true); pump(200);
        const int toggles = juce::SystemStats::getEnvironmentVariable("TEST_TOGGLES", "1").getIntValue();
        playHead.pos = 0;
        for (int half = 0; half < 16; ++half)
        {
            if (half == 4 || (toggles > 1 && half == 10))
                if (auto* p = np->apvts.getParameter("tune_lookahead"))
                {
                    const int before = proc->getLatencySamples();
                    p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
                    pump(30);
                    std::cout << "-- Lookahead " << (p->getValue() > 0.5f ? "ON" : "OFF") << " (latency " << before << " -> " << proc->getLatencySamples() << ")\n";
                    if (juce::SystemStats::getEnvironmentVariable("TEST_REPREPARE", "0") == "1")
                    { proc->releaseResources(); proc->prepareToPlay(48000.0, 512); std::cout << "-- host re-prepared\n"; }
                }
            double e = 0;
            for (int b = 0; b < 47; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (playHead.pos + i) / 48000.0;
                    const double f = 220.0 * std::pow(2.0, (0.3 + 0.2 * std::sin(t * 2 * 3.14159 * 5.5)) / 12.0);
                    double hs = 0; for (int hN = 1; hN <= 8; ++hN) hs += std::sin(hN * ph) / hN;
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0;
                    buf.setSample(0, i, (float) (0.2 * hs)); buf.setSample(1, i, (float) (0.2 * hs));
                }
                playHead.playing = true;
                proc->processBlock(buf, midi);
                playHead.pos += 512;
                int nans = 0;
                for (int i = 0; i < 512; ++i) { const float y = buf.getSample(0, i); if (! std::isfinite(y)) ++nans; else e += y * y; }
                if (nans > 0 && b % 47 == 0) std::cout << "[NaN/inf x" << nans << "] ";
                if (b % 6 == 0) pump(5);
            }
            std::cout << "t=" << juce::String(half * 0.5, 1) << "s out " << juce::String(juce::Decibels::gainToDecibels((float) std::sqrt(e / (47 * 512))), 1) << " dB\n";
        }
        ed.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "reopen")
    {
        auto& q = np->getPitchTraceQueue();
        auto sing = [&](int blocks, bool hostPlays, float amp)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (playHead.pos + i) / 48000.0;
                    const double f = 220.0 * std::pow(2.0, (2.0 * std::floor(t * 2.0)) / 12.0);
                    double hs = 0; for (int hN = 1; hN <= 8; ++hN) hs += std::sin(hN * ph) / hN;
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0;
                    buf.setSample(0, i, (float) (amp * hs)); buf.setSample(1, i, (float) (amp * hs));
                }
                playHead.playing = hostPlays;
                proc->processBlock(buf, midi);
                if (hostPlays) playHead.pos += 512;
                if (b % 4 == 0) pump(5);
            }
        };
        auto report = [&](const char* label, juce::AudioProcessorEditor& ed)
        {
            auto* r = findRoll(ed);
            int edited = 0; for (auto& n : r->getNotesForTests()) if (n.pitchEdited) ++edited;
            std::cout << label << ": hasClip=" << r->hasPreviewClip() << " hops=" << r->getTimelineSize()
                      << " notes=" << r->getNotesForTests().size() << " pitch-edited=" << edited << "\n";
        };
        std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
        ed->setSize(w, h); ed->setVisible(true); pump(200);
        playHead.pos = 48000 * 4;
        sing(94 * 4, true, 0.2f);
        sing(4, false, 0.0f); pump(500);
        auto* roll = findRoll(*ed);
        roll->selectAdjacentNote(1); roll->transposeSelectedNote(2); pump(100);
        report("after take + edit", *ed);
        ed.reset(); pump(100);                       // FL closes the plugin window
        playHead.pos = 0; sing(94 * 6, true, 0.2f);  // FL plays the song with the vocal track (input) while closed
        sing(4, false, 0.0f);
        ed.reset(proc->createEditor()); ed->setSize(w, h); ed->setVisible(true); pump(300);
        report("reopened", *ed);
        playHead.pos = 0; sing(94 * 3, true, 0.2f); sing(4, false, 0.0f); pump(300);
        report("reopened + DAW played", *ed);
        roll = findRoll(*ed);
        roll->setPolyphonicMode(true); pump(200);
        roll->setPolyphonicMode(false); pump(200);
        report("after Poly -> Mono", *ed);
        ed.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "centre")
    {
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        auto* roll = findRoll(*med);
        pump(300);
        const double sr = 48000.0; const int N = (int) (8 * sr);
        juce::AudioBuffer<float> voc(1, N); double vph = 0;
        const double freqs[] = { 216.0, 250.5, 258.4, 297.2 };   // A3 -31c, B3 +23c, C4 -22c, D4 +20c
        for (int i = 0; i < N; ++i)
        {
            const double t = i / sr; const int k = (int) (t / 2.0) % 4; const double ph2 = std::fmod(t, 2.0);
            float smp = 0.0f;
            if (ph2 < 1.6)
            {
                vph += 2 * 3.14159265 * freqs[k] * std::pow(2.0, 0.15 * std::sin(t * 2 * 3.14159 * 5.5) / 12.0) / sr;
                const double env = juce::jmin(1.0, ph2 / 0.05) * juce::jmin(1.0, (1.6 - ph2) / 0.05);
                double hs = 0; for (int hN = 1; hN <= 12; ++hN) hs += std::sin(hN * vph) / (hN * (hN > 2 ? 1.6 : 1.0));
                smp = (float) (0.2 * env * hs);
            }
            voc.setSample(0, i, smp);
        }
        const auto wav = outDir.getChildFile("centrevocal.wav"); wav.deleteFile();
        { std::unique_ptr<juce::AudioFormatWriter> wr(juce::WavAudioFormat().createWriterFor(new juce::FileOutputStream(wav), sr, 1, 24, {}, 0)); wr->writeFromAudioSampleBuffer(voc, 0, N); }
        roll->filesDropped(juce::StringArray(wav.getFullPathName()), 0, 0);
        pump(600);
        for (const auto& n : roll->getNotesForTests())
            if (! n.isSibilant) std::cout << "note " << n.id << " detected " << juce::String(n.detectedMidi, 3) << " target " << n.targetMidi << "\n";
        save(*med, outDir.getChildFile("centre_0.png"), 1.0f);
        if (auto* p = np->apvts.getParameter("tune_center")) p->setValueNotifyingHost(1.0f);
        for (int b = 0; b < 4; ++b) { buf.clear(); proc->processBlock(buf, midi); }   // publishes the knob to the roll
        pump(90);
        save(*med, outDir.getChildFile("centre_moving.png"), 1.0f);
        pump(1200);
        save(*med, outDir.getChildFile("centre_100.png"), 1.0f);
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "level")
    {
        // Loud / quiet notes; press Level; per-note output level + pitch (zero crossings) before and after.
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        auto* roll = findRoll(*med);
        auto& q = np->getPitchTraceQueue();
        for (auto kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("TEST_PARAMS", ""), ",", ""))
            if (auto* prm = np->apvts.getParameter(kv.upToFirstOccurrenceOf("=", false, false)))
                prm->setValueNotifyingHost(prm->convertTo0to1(kv.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
        const bool polyLoad = argc > 9 && juce::String(argv[9]) == "poly";
        if (polyLoad) if (auto* p = np->apvts.getParameter("tune_polyphonic")) p->setValueNotifyingHost(1.0f);
        pump(300);
        const double sr = 48000.0; const int N = (int) (8 * sr);
        juce::AudioBuffer<float> voc(1, N); double vph = 0;
        const double freqs[] = { 226.0, 240.0, 268.0, 285.0 };   // a bit off-pitch on purpose (246, 293)
        const float amps[] = { 0.06f, 0.35f, 0.06f, 0.35f };
        for (int i = 0; i < N; ++i)
        {
            const double t = i / sr; const int k = (int) (t / 2.0) % 4; const double ph2 = std::fmod(t, 2.0);
            float smp = 0.0f;
            if (ph2 < 1.6)
            {
                vph += 2 * 3.14159265 * freqs[k] * std::pow(2.0, (0.35 * std::sin(t * 2 * 3.14159 * 5.5) + 0.4 * std::sin(t * 1.3)) / 12.0) / sr;
                const double env = juce::jmin(1.0, ph2 / 0.05) * juce::jmin(1.0, (1.6 - ph2) / 0.05);
                double hs = 0; for (int hN = 1; hN <= 12; ++hN) hs += std::sin(hN * vph) / (hN * (hN > 2 ? 1.6 : 1.0));
                smp = (float) (amps[k] * env * (juce::SystemStats::getEnvironmentVariable("TEST_RICH", "0") == "1" ? 0.6 * hs : std::sin(vph) + 0.3 * std::sin(2 * vph)));
            }
            voc.setSample(0, i, smp);
        }
        const auto wav = outDir.getChildFile("levelvocal.wav"); wav.deleteFile();
        { std::unique_ptr<juce::AudioFormatWriter> wr(juce::WavAudioFormat().createWriterFor(new juce::FileOutputStream(wav), sr, 1, 24, {}, 0)); wr->writeFromAudioSampleBuffer(voc, 0, N); }
        roll->filesDropped(juce::StringArray(wav.getFullPathName()), 0, 0);
        const auto t0 = juce::Time::getMillisecondCounter();
        while (! roll->hasPreviewClip() && juce::Time::getMillisecondCounter() - t0 < 60000) pump(50);
        pump(300);
        auto playAndMeasure = [&](const char* label)
        {
            if (polyLoad) { auto& c = np->getPolyClip(); const auto t1 = juce::Time::getMillisecondCounter(); pump(200); while (! c.isRenderCurrent() && juce::Time::getMillisecondCounter() - t1 < 30000) pump(50); }
            roll->seekClipToStart(); roll->setPreviewPlaying(true);
            juce::AudioBuffer<float> out(1, N + 48000); int w0 = 0;
            while (w0 + 512 <= out.getNumSamples() && roll->isPreviewPlaying())
            { buf.clear(); proc->processBlock(buf, midi); out.copyFrom(0, w0, buf, 0, 0, 512); w0 += 512; }
            roll->setPreviewPlaying(false);
            const int lat = proc->getLatencySamples();
            std::cout << label << " (lat " << lat << "):";
            for (int k = 0; k < 4; ++k)
            {
                const int a = (int) ((k * 2.0 + 0.4) * sr) + lat, b = (int) ((k * 2.0 + 1.2) * sr) + lat;
                double e = 0; int zc = 0;
                for (int i = a; i < b && i < w0; ++i) { const float x = out.getSample(0, i); e += x * x; if (i > a && (x >= 0) != (out.getSample(0, i - 1) >= 0)) ++zc; }
                const double rms = std::sqrt(e / (b - a));
                std::cout << "  n" << k << " " << juce::String(juce::Decibels::gainToDecibels((float) rms), 1) << "dB " << juce::String(zc / 2.0 / ((b - a) / sr), 0) << "Hz";
            }
            // How dry is it? Pitch contour (40 ms windows, zero crossings) of output vs input, voiced middles only
            auto zcHz = [&](const float* d, int from, int len) { int z = 0; for (int i = from + 1; i < from + len; ++i) if ((d[i] >= 0) != (d[i - 1] >= 0)) ++z; return z / 2.0 / (len / sr); };
            double sumC = 0, maxC = 0; int nw = 0; const int win = (int) (0.04 * sr);
            for (int k = 0; k < 4; ++k)
                for (int i = (int) ((k * 2.0 + 0.2) * sr); i + win < (int) ((k * 2.0 + 1.4) * sr); i += win)
                {
                    if (i + lat + win >= w0) break;
                    const double fi = zcHz(voc.getReadPointer(0), i, win), fo = zcHz(out.getReadPointer(0), i + lat, win);
                    const double c = std::abs(1200.0 * std::log2(fo / fi));
                    sumC += c; maxC = juce::jmax(maxC, c); ++nw;
                }
            std::cout << "  | pitch vs dry: mean " << juce::String(sumC / juce::jmax(1, nw), 1) << "c, max " << juce::String(maxC, 0) << "c";
            std::cout << "\n";
        };
        playAndMeasure("before Level");
        roll->levelAllNotesVolume(0.72f);
        pump(300);
        if (juce::SystemStats::getEnvironmentVariable("TEST_NOTES", "0") == "1")
            for (const auto& n : roll->getNotesForTests())
                std::cout << "  note " << n.id << " lane " << n.voiceLane << " hops " << n.startHop << "-" << n.endHop << " midi " << juce::String(n.detectedMidi, 2)
                          << " rms " << juce::String(juce::Decibels::gainToDecibels(n.meanRms), 1) << "dB gain " << juce::String(n.gainDb, 1) << (n.isSibilant ? " sib" : "") << "\n";
        playAndMeasure("after Level ");
        roll->selectAdjacentNote(1);
        if (auto* sn = roll->getSelectedNote())
            std::cout << "selected: id " << sn->id << " hops " << sn->startHop << "-" << sn->endHop << " det " << sn->detectedMidi << " tgt " << sn->targetMidi << " count " << roll->getSelectedNoteCount() << "\n";
        roll->transposeSelectedNote(1);   // first note +1 semitone: an edit must still land with Center at 0%
        if (auto* sn = roll->getSelectedNote()) std::cout << "after: tgt " << sn->targetMidi << " pitchEdited " << sn->pitchEdited << "\n";
        pump(300);
        playAndMeasure("n0 +1 st    ");
        save(*med, outDir.getChildFile("level.png"), 1.0f);
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && (juce::String(argv[8]) == "monoload" || juce::String(argv[8]) == "idle"))
    {
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        auto* roll = findRoll(*med);
        auto& q = np->getPitchTraceQueue();
        juce::Random rng(7);
        if (juce::String(argv[8]) == "idle")
        {
            // Plugin open, DAW stopped, silent input: nothing should move.
            for (int sec = 0; sec < 6; ++sec)
            {
                for (int b = 0; b < 94; ++b) { buf.clear(); playHead.playing = false; proc->processBlock(buf, midi); if (b % 8 == 0) pump(5); }
                pump(50);
                std::cout << "idle t=" << sec + 1 << "s viewStart=" << roll->getViewStartHop() << " timeline=" << roll->getTimelineSize()
                          << " playheadHop=" << q.currentPlayheadHop.load() << "\n";
            }
            save(*med, outDir.getChildFile("idle.png"), 1.0f);
            med.reset(); proc->releaseResources(); return 0;
        }
        // Synthetic vocal: 1.4 s sung note, 0.6 s breath, repeated (16 s)
        const double sr = 44100.0; const int N = (int) (juce::SystemStats::getEnvironmentVariable("TEST_LEN", "16").getDoubleValue() * sr);
        juce::AudioBuffer<float> voc(1, N); double vph = 0;
        const int notes[] = { 57, 60, 62, 64, 62, 60, 59, 57 };
        for (int i = 0; i < N; ++i)
        {
            const double t = i / sr; const int k = (int) (t / 2.0); const double ph2 = std::fmod(t, 2.0);
            float smp = 0.0f;
            if (ph2 < 1.4)
            {
                const double f = 440.0 * std::pow(2.0, (notes[k % 8] - 69 + 0.2 * std::sin(t * 2 * 3.14159 * 5.5)) / 12.0);
                vph += 2 * 3.14159265 * f / sr;
                const double env = juce::jmin(1.0, ph2 / 0.05) * juce::jmin(1.0, (1.4 - ph2) / 0.05);
                smp = (float) (env * (0.3 * std::sin(vph) + 0.15 * std::sin(2 * vph) + 0.08 * std::sin(3 * vph)));
            }
            else smp = 0.02f * (rng.nextFloat() * 2 - 1);
            voc.setSample(0, i, smp);
        }
        const auto wav = outDir.getChildFile("synthvocal.wav"); wav.deleteFile();
        {
            std::unique_ptr<juce::AudioFormatWriter> wr(juce::WavAudioFormat().createWriterFor(new juce::FileOutputStream(wav), sr, 1, 24, {}, 0));
            wr->writeFromAudioSampleBuffer(voc, 0, N);
        }
        for (auto kv : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("TEST_PARAMS", ""), ",", ""))
            if (auto* prm = np->apvts.getParameter(kv.upToFirstOccurrenceOf("=", false, false)))
                prm->setValueNotifyingHost(prm->convertTo0to1(kv.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
        pump(200);
        const bool polyLoad = argc > 9 && juce::String(argv[9]) == "poly";
        if (polyLoad)
        {
            if (auto* p = np->apvts.getParameter("tune_polyphonic")) p->setValueNotifyingHost(1.0f);
            pump(300);
        }
        const auto tLoad = juce::Time::getMillisecondCounter();
        roll->filesDropped(juce::StringArray(wav.getFullPathName()), 0, 0);
        pump(300);
        if (polyLoad)
        {
            auto& clip = np->getPolyClip();
            while (! roll->hasPreviewClip() && juce::Time::getMillisecondCounter() - tLoad < 120000) pump(50);
            std::cout << "poly ready after " << (juce::Time::getMillisecondCounter() - tLoad) << " ms, render current=" << clip.isRenderCurrent() << "\n";
        }
        { const auto tp = juce::Time::getMillisecondCounterHiRes(); for (int r = 0; r < 10; ++r) { auto im = med->createComponentSnapshot(med->getLocalBounds(), true, 1.0f); } std::cout << "paint: " << (juce::Time::getMillisecondCounterHiRes() - tp) / 10.0 << " ms/frame\n"; }
        std::cout << "loaded: hasClip=" << roll->hasPreviewClip() << " in " << (juce::Time::getMillisecondCounter() - tLoad) << " ms, roll hops=" << roll->getTimelineSize() << "\n";
        roll->setPreviewPlaying(true);
        if (const double seek = juce::SystemStats::getEnvironmentVariable("TEST_SEEK", "0").getDoubleValue(); seek > 0)
        {
            if (polyLoad) np->getPolyClip().seekPreview((juce::int64) (seek * 48000));
            else q.monoPreview.pos.store(seek * sr);
        }
        // Play 14 s on the plugin's own transport; report sung-part output level per 2 s phrase
        const int phrases = juce::SystemStats::getEnvironmentVariable("TEST_PHRASES", "7").getIntValue();
        for (int k = 0; k < phrases; ++k)
        {
            if (juce::SystemStats::getEnvironmentVariable("TEST_REALTIME", "0") == "1") pump(1900); // let the GUI/worker run like in a host
            double sung = 0; int ns = 0; double br = 0; int nb = 0;
            for (int b = 0; b < (int) (2.0 * 48000 / 512); ++b)
            {
                buf.clear(); playHead.playing = false; proc->processBlock(buf, midi);
                const double clipT = polyLoad ? (double) np->getPolyClip().getPlayPosition() / 48000.0 : q.monoPreview.pos.load() / sr;
                const double ph2 = std::fmod(clipT, 2.0);
                double e = 0; for (int i = 0; i < 512; ++i) e += buf.getSample(0, i) * buf.getSample(0, i);
                if (ph2 > 0.3 && ph2 < 1.3) { sung += e; ns += 512; } else if (ph2 > 1.6 && ph2 < 1.95) { br += e; nb += 512; }
                if (b % 8 == 0) pump(5);
            }
            if (polyLoad) std::cout << "[render current=" << np->getPolyClip().isRenderCurrent() << " t=" << (juce::Time::getMillisecondCounter() - tLoad) << "ms] ";
            std::cout << "phrase " << k << ": sung rms=" << std::sqrt(sung / juce::jmax(1, ns)) << " breath rms=" << std::sqrt(br / juce::jmax(1, nb)) << "\n";
        }
        med.reset(); proc->releaseResources(); return 0;
    }

    if (argc > 8 && juce::String(argv[8]) == "monotake")
    {
        // ---- Mono take: record while the "DAW" plays, stop, then play it on NovaTone's own transport ----
        std::unique_ptr<juce::AudioProcessorEditor> med(proc->createEditor());
        med->setSize(w, h); med->setVisible(true);
        auto* roll = findRoll(*med);
        auto& q = np->getPitchTraceQueue();
        auto sing = [&](int blocks, bool hostPlays, float amp)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const double t = (playHead.pos + i) / 48000.0;
                    const double f = 220.0 * std::pow(2.0, (2.0 * std::floor(t * 2.0)) / 12.0);
                    const float sm = (float) (amp * (std::sin(ph) + 0.3 * std::sin(2 * ph)));
                    ph += juce::MathConstants<double>::twoPi * f / 48000.0;
                    buf.setSample(0, i, sm); buf.setSample(1, i, sm);
                }
                playHead.playing = hostPlays;
                proc->processBlock(buf, midi);
                if (hostPlays) playHead.pos += 512;
                if (b % 4 == 0) pump(5);
            }
        };
        std::cout << "before: hasClip=" << roll->hasPreviewClip() << " take=" << roll->getMonoTakeState() << "\n";
        playHead.pos = 48000 * 4;                    // song position 4 s
        sing(94 * juce::SystemStats::getEnvironmentVariable("TEST_TAKE_SEC", "4").getIntValue(), true, 0.25f);   // singing while the DAW plays
        std::cout << "while DAW plays: take=" << roll->getMonoTakeState() << " len=" << q.monoTake.length.load() << "\n";
        sing(4, false, 0.0f);                        // DAW stops
        pump(500);
        std::cout << "after stop: hasClip=" << roll->hasPreviewClip() << " take=" << roll->getMonoTakeState()
                  << " songStart=" << q.monoPreview.timelineStart.load() << "\n";
        roll->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        float outPeak = 0.0f;
        for (int b = 0; b < 100; ++b) { buf.clear(); playHead.playing = false; proc->processBlock(buf, midi); outPeak = juce::jmax(outPeak, buf.getMagnitude(0, 512)); }
        pump(200);
        const int hopPlaying = q.currentPlayheadHop.load();
        std::cout << "space -> playing=" << roll->isPreviewPlaying() << " outPeak(silent input)=" << outPeak << " playheadHop=" << hopPlaying << "\n";
        save(*med, outDir.getChildFile("mono_take_playing.png"), 1.0f);
        roll->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        std::cout << "space -> playing=" << roll->isPreviewPlaying() << "\n";
        for (int b = 0; b < 20; ++b) { buf.clear(); proc->processBlock(buf, midi); }
        std::cout << "stopped: playheadHop=" << q.currentPlayheadHop.load() << " (held)\n";
        roll->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        for (int b = 0; b < 10; ++b) { buf.clear(); proc->processBlock(buf, midi); }
        std::cout << "continue: playheadHop=" << q.currentPlayheadHop.load() << " (> held)\n";
        roll->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey));
        // DAW plays inside the take: clip replaces the (silent) input in place
        playHead.pos = 48000 * 5; outPeak = 0.0f;
        for (int b = 0; b < 40; ++b) { buf.clear(); playHead.playing = true; proc->processBlock(buf, midi); playHead.pos += 512; outPeak = juce::jmax(outPeak, buf.getMagnitude(0, 512)); }
        std::cout << "DAW in place: outPeak=" << outPeak << " playheadHop=" << q.currentPlayheadHop.load()
                  << " take=" << roll->getMonoTakeState() << " (no new take)\n";
        playHead.playing = false; buf.clear(); proc->processBlock(buf, midi); pump(100);
        save(*med, outDir.getChildFile("mono_take_stopped.png"), 1.0f);
        med.reset();
        proc->releaseResources();
        return 0;
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
