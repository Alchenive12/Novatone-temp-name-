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
    const int theme = argc > 5 ? std::atoi(argv[5]) : 0;
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(dynamic_cast<NovaToneAudioProcessor*>(proc.get())->apvts.getParameter("ui_theme")))
        p->setValueNotifyingHost(p->convertTo0to1((float) theme));
    proc->setPlayConfigDetails(2, 2, 48000.0, 512);
    proc->prepareToPlay(48000.0, 512);

    // Feed a gliding test tone so the live trace has something to show.
    juce::AudioBuffer<float> buf(2, 512); juce::MidiBuffer midi; double ph = 0;
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    ed->setSize(w, h);
    ed->setVisible(true);
    for (int b = 0; b < 300; ++b)
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
