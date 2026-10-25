// Dev-only: renders the NovaTone editor to PNGs without a host.
// Build with -DNOVATONE_BUILD_SNAPSHOT=ON, run: NovaToneSnapshot <outDir> [width height]
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GUI/PitchTraceComponent.h"

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

    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    ed->setSize(w, h);
    ed->setVisible(true);
    for (int b = 0; b < (polyTest ? 0 : 300); ++b)
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

    ed.reset();
    proc->releaseResources();
    return 0;
}
