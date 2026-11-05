#include "PluginProcessor.h"
#include "PluginEditor.h"

NovaToneAudioProcessor::NovaToneAudioProcessor()
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::stereo(), false) // beat / instrumental for the Harmony tab
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    ),
    apvts(*this, nullptr, "NOVATONE_PARAMETERS", createParameterLayout()),
    presetManager(apvts,
        [this](const juce::ValueTree& tree) { applyStateGuarded(tree); },
        [this] { resetToDefaultsGuarded(); })
{
    tuneFlexAmountParam     = apvts.getRawParameterValue("tune_flexAmount");
    tuneHumanizeAmountParam = apvts.getRawParameterValue("tune_humanizeAmount");
    tuneHumanizeButtonParam = apvts.getRawParameterValue("tune_humanize");
    tunePolyphonicParam     = apvts.getRawParameterValue("tune_polyphonic");
    tuneMidiCleanParam      = apvts.getRawParameterValue("tune_midiClean");
    tuneVibratoParam        = apvts.getRawParameterValue("tune_vibrato");
    tuneCenterParam         = apvts.getRawParameterValue("tune_center");
    tuneVariationParam      = apvts.getRawParameterValue("tune_variation");
    tuneTransitionParam     = apvts.getRawParameterValue("tune_transition");
    tuneSensitivityParam    = apvts.getRawParameterValue("tune_sensitivity");
    tuneSnapScaleParam      = apvts.getRawParameterValue("tune_snapScale");
    tuneAutoScrollParam     = apvts.getRawParameterValue("tune_autoScroll");
    tuneLookaheadParam      = apvts.getRawParameterValue("tune_lookahead");
    tuneNaturalParam        = apvts.getRawParameterValue("tune_natural");
    tuneSyncParam           = apvts.getRawParameterValue("tune_sync");
    tuneSmoothParam         = apvts.getRawParameterValue("tune_smooth");
    tuneCharacterParam      = apvts.getRawParameterValue("tune_character");

    for (auto* id : syncListenerIds)
        apvts.addParameterListener(id, this);

    for (int i = 0; i < 12; ++i)
    {
        const juce::String id = "tune_scaleDegree_" + juce::String(i);
        tuneScaleDegreeParams[(size_t)i] = apvts.getRawParameterValue(id);
        apvts.addParameterListener(id, this);
    }

    tuneRootParam = apvts.getRawParameterValue("tune_root");
    tuneModeParam = apvts.getRawParameterValue("tune_mode");
    tuneVocalRangeParam = apvts.getRawParameterValue("tune_vocalRange");
    apvts.addParameterListener("tune_root", this);
    apvts.addParameterListener("tune_mode", this);

    alterPitchParam   = apvts.getRawParameterValue("alter_pitch");
    alterFormantParam = apvts.getRawParameterValue("alter_formant");
    alterMixParam     = apvts.getRawParameterValue("alter_mix");
    alterSoloWetParam = apvts.getRawParameterValue("alter_solo_wet");
    for (int v = 0; v < HarmonyEngine::numVoices; ++v)
    {
        const juce::String p = "harm_v" + juce::String(v + 1) + "_";
        harmVoiceParams[(size_t)v] = { apvts.getRawParameterValue(p + "on"), apvts.getRawParameterValue(p + "mode"),
                                       apvts.getRawParameterValue(p + "level"), apvts.getRawParameterValue(p + "pan"),
                                       apvts.getRawParameterValue(p + "formant") };
    }
    harmMixParam      = apvts.getRawParameterValue("harm_mix");
    harmLeadParam     = apvts.getRawParameterValue("harm_lead");
    harmHumanizeParam = apvts.getRawParameterValue("harm_humanize");
    harmSourceParam   = apvts.getRawParameterValue("harm_source");
    harmMidiOffsetParam = apvts.getRawParameterValue("harm_midiOffset");

    inputGainParam    = apvts.getRawParameterValue("global_input");
    outputGainParam   = apvts.getRawParameterValue("global_output");

    tuneBypassParam   = apvts.getRawParameterValue("tune_bypass");
    alterBypassParam  = apvts.getRawParameterValue("alter_bypass");
}

NovaToneAudioProcessor::~NovaToneAudioProcessor()
{
    cancelPendingUpdate();
    for (auto* id : syncListenerIds)
        apvts.removeParameterListener(id, this);
    apvts.removeParameterListener("tune_root", this);
    apvts.removeParameterListener("tune_mode", this);
    for (int i = 0; i < 12; ++i)
        apvts.removeParameterListener("tune_scaleDegree_" + juce::String(i), this);
}

//==============================================================================
// Tune key / scale
//==============================================================================
uint16_t NovaToneAudioProcessor::scaleMaskFor(int root, int mode) noexcept
{
    static constexpr int major[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int minor[7] = { 0, 2, 3, 5, 7, 8, 10 };

    uint16_t mask = 0;
    for (int step : (mode == minorScale ? minor : major))
        mask |= (uint16_t)(1u << ((root + step) % 12));
    return mask;
}

int NovaToneAudioProcessor::currentRoot() const noexcept
{
    return tuneRootParam != nullptr ? juce::jlimit(0, 11, juce::roundToInt(tuneRootParam->load(std::memory_order_relaxed))) : 0;
}

int NovaToneAudioProcessor::currentMode() const noexcept
{
    return tuneModeParam != nullptr ? juce::jlimit(0, 2, juce::roundToInt(tuneModeParam->load(std::memory_order_relaxed))) : (int)majorScale;
}

uint16_t NovaToneAudioProcessor::currentDegreeMask() const noexcept
{
    uint16_t mask = 0;
    for (int i = 0; i < 12; ++i)
    {
        const auto* p = tuneScaleDegreeParams[(size_t)i];
        if (p != nullptr && p->load(std::memory_order_relaxed) > 0.5f) mask |= (uint16_t)(1u << i);
    }
    return mask;
}

namespace
{
    struct ScaleWriteGuard
    {
        explicit ScaleWriteGuard(std::atomic<bool>& f) : flag(f), previous(f.exchange(true)) {}
        ~ScaleWriteGuard() { flag = previous; }
        std::atomic<bool>& flag;
        const bool previous;
    };
}

void NovaToneAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // The roll draws notes where Center puts them: keep its copy current even while no audio runs.
    if (parameterID == "tune_center")         pitchQueue.globalCenter.store(newValue, std::memory_order_relaxed);
    if (parameterID == "tune_variation")      pitchQueue.globalVariation.store(newValue, std::memory_order_relaxed);
    if (parameterID == "tune_humanizeAmount") pitchQueue.globalHumanizeAmount.store(newValue, std::memory_order_relaxed);

    if (parameterID == "tune_smooth" || parameterID == "tune_lookahead" || parameterID == "tune_natural")
    {
        syncRequested = true;
        triggerAsyncUpdate();
        return;
    }
    if (parameterID == "tune_sync" || parameterID == "tune_character")
    {
        syncRequested = true;
        triggerAsyncUpdate();
        return;
    }

    if (parameterID == "tune_flexAmount" || parameterID == "tune_humanizeAmount" || parameterID == "tune_vibrato"
        || parameterID == "tune_center" || parameterID == "tune_variation"
        || parameterID == "tune_transition" || parameterID == "tune_sensitivity")
    {
        // Only post an async update if SYNC is actually active; avoids message-thread queue
        // flooding while the user drags macro knobs in the lower dock.
        if (! applyingMacro.load() && ! applyingScale.load()
            && tuneSyncParam != nullptr && tuneSyncParam->load(std::memory_order_relaxed) > 0.5f)
        {
            knobTouched = true;
            triggerAsyncUpdate();
        }
        return;
    }

    if (applyingScale.load())
        return;

    if (parameterID == "tune_root" || parameterID == "tune_mode")
        scaleChangeRequested = true;
    else
        keyEdited = true;

    triggerAsyncUpdate();
}

void NovaToneAudioProcessor::applySyncMacro()
{
    const bool touched = knobTouched.exchange(false);
    const bool requested = syncRequested.exchange(false);
    if (tuneSyncParam == nullptr || tuneSyncParam->load() < 0.5f)
        return;

    if (touched)
    {
        setParameterFromMessageThread("tune_sync", 0.0f);
        return;
    }
    if (! requested)
        return;

    const float character = tuneCharacterParam != nullptr ? tuneCharacterParam->load() : 0.5f;
    const bool lookaheadButtonOn = tuneLookaheadParam != nullptr && tuneLookaheadParam->load() > 0.5f;
    const auto knobs = TuneMacro::forCharacter(character, lookaheadButtonOn);
    const ScaleWriteGuard guard(applyingMacro);
    setParameterFromMessageThread("tune_flexAmount",     knobs.speed);
    setParameterFromMessageThread("tune_humanizeAmount", knobs.humanize);
    setParameterFromMessageThread("tune_vibrato",        knobs.vibratoCents);
    setParameterFromMessageThread("tune_center",         knobs.center);
    setParameterFromMessageThread("tune_variation",      knobs.variation);
    setParameterFromMessageThread("tune_transition",     knobs.transition);
    setParameterFromMessageThread("tune_sensitivity",    knobs.sensitivity);
}

bool NovaToneAudioProcessor::smoothRequested() const noexcept
{
    return tuneSmoothParam != nullptr && tuneSmoothParam->load(std::memory_order_relaxed) > 0.5f;
}

bool NovaToneAudioProcessor::lookaheadRequested() const noexcept
{
    return smoothRequested()
        || (tuneLookaheadParam != nullptr && tuneLookaheadParam->load(std::memory_order_relaxed) > 0.5f);
}

int NovaToneAudioProcessor::totalLatencyFor(bool lookahead) const noexcept
{
    return tuneEngine.getLatencySamplesFor(lookahead)
        + alterEngine.getLatencySamples();
}

void NovaToneAudioProcessor::handleAsyncUpdate()
{
    const int wanted = totalLatencyFor(lookaheadRequested());
    if (wanted != getLatencySamples())
        setLatencySamples(wanted);

    applySyncMacro();

    const bool scaleChanged = scaleChangeRequested.exchange(false);
    const bool edited = keyEdited.exchange(false);

    const int mode = currentMode();
    if (mode == customScale)
        return;

    const uint16_t target = scaleMaskFor(currentRoot(), mode);

    if (scaleChanged)
    {
        const ScaleWriteGuard guard(applyingScale);
        for (int i = 0; i < 12; ++i)
            setParameterFromMessageThread("tune_scaleDegree_" + juce::String(i), ((target >> i) & 1u) ? 1.0f : 0.0f);
    }
    else if (edited && currentDegreeMask() != target)
    {
        const ScaleWriteGuard guard(applyingScale);
        setParameterFromMessageThread("tune_mode", (float)customScale);
    }
}

void NovaToneAudioProcessor::setParameterFromMessageThread(const juce::String& id, float plainValue)
{
    if (auto* p = apvts.getParameter(id))
    {
        const float normalised = p->convertTo0to1(plainValue);
        if (p->getValue() == normalised) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(normalised);
        p->endChangeGesture();
    }
}

void NovaToneAudioProcessor::applyStateGuarded(const juce::ValueTree& newState)
{
    {
        const ScaleWriteGuard guard(applyingScale);
        apvts.replaceState(newState);
    }
    scaleChangeRequested = true;
    triggerAsyncUpdate();
}

void NovaToneAudioProcessor::resetToDefaultsGuarded()
{
    {
        const ScaleWriteGuard guard(applyingScale);
        for (auto* p : getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
                setParameterFromMessageThread(ranged->getParameterID(), ranged->convertFrom0to1(ranged->getDefaultValue()));
    }
    scaleChangeRequested = true;
    triggerAsyncUpdate();
}

const juce::String NovaToneAudioProcessor::getName() const
{
#ifdef JucePlugin_Name
    return JucePlugin_Name;
#else
    return "NovaTone";
#endif
}

bool NovaToneAudioProcessor::acceptsMidi() const { return true; }
bool NovaToneAudioProcessor::producesMidi() const { return true; }
bool NovaToneAudioProcessor::isMidiEffect() const { return false; }
double NovaToneAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int NovaToneAudioProcessor::getNumPrograms() { return 1; }
int NovaToneAudioProcessor::getCurrentProgram() { return 0; }
void NovaToneAudioProcessor::setCurrentProgram(int) {}
const juce::String NovaToneAudioProcessor::getProgramName(int) { return {}; }
void NovaToneAudioProcessor::changeProgramName(int, const juce::String&) {}
bool NovaToneAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* NovaToneAudioProcessor::createEditor()
{
    return new NovaToneAudioProcessorEditor(*this);
}

bool NovaToneAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
#if ! JucePlugin_IsSynth
    if (out != layouts.getMainInputChannelSet()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet(true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono() && side != juce::AudioChannelSet::stereo())
            return false;
    }
#endif
    return true;
}

void NovaToneAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::dsp::ProcessSpec spec
    {
        sampleRate,
        (juce::uint32)samplesPerBlock,
        (juce::uint32)juce::jmax(1, getTotalNumOutputChannels())
    };

    tuneEngine.setLookaheadEnabled(lookaheadRequested());
    tuneEngine.prepare(spec);
    polyClip.prepare(sampleRate, samplesPerBlock, (int)spec.numChannels);
    pitchQueue.monoTake.prepare(sampleRate);
    harmony.prepare(sampleRate, samplesPerBlock);
    harmonyExporter.prepare(sampleRate, samplesPerBlock);
    choirRecorder.prepare(sampleRate, samplesPerBlock);
    choir.prepare(sampleRate, samplesPerBlock);
    harmonyDry.setSize((int)spec.numChannels, samplesPerBlock);
    alterEngine.prepare(spec);
    lastPublishedHopIndex = 0;
    clearMidiState();
    preparedBlockSize = samplesPerBlock;

    for (auto* g : { &inputGain, &outputGain })
    {
        g->prepare(spec);
        g->setRampDurationSeconds(0.02);
        g->setGainLinear(1.0f);
        g->reset();
    }

    const bool tuneStartsBypassed = tuneBypassParam != nullptr && tuneBypassParam->load() > 0.5f;
    bypass[tuneModule].prepare(spec, tuneEngine.getLatencySamples(), tuneStartsBypassed,
        juce::jmax(tuneEngine.getLatencySamplesFor(false), tuneEngine.getLatencySamplesFor(true)));
    bypass[alterModule].prepare(spec, alterEngine.getLatencySamples(), true);

    setLatencySamples(totalLatencyFor(tuneEngine.isLookaheadEnabled()));
}

void NovaToneAudioProcessor::releaseResources()
{
    tuneEngine.reset();
    alterEngine.reset();
    lastPublishedHopIndex = 0;
    clearMidiState();
}

void NovaToneAudioProcessor::clearMidiState()
{
    heldMidiNotes.fill(false);
    midiHeld.fill(false);
    activeOutputMidiNotes.fill(false);
    lastActiveMidiNote = -1;
}

uint16_t NovaToneAudioProcessor::packScaleDegreeMask() const noexcept
{
    const int mode = currentMode();
    uint16_t mask = mode == customScale ? currentDegreeMask() : scaleMaskFor(currentRoot(), mode);
    if (mask == 0) mask = 0x0FFF;
    return mask;
}

float NovaToneAudioProcessor::configureEngines(TuneEngine& te, AlterEngine& ae, int engineHop, bool allowLiveOverride) noexcept
{
    auto load = [](const std::atomic<float>* p, float fallback) noexcept {
        return p != nullptr ? p->load(std::memory_order_relaxed) : fallback;
    };

    const float center      = load(tuneCenterParam,      0.85f);
    const float variation   = load(tuneVariationParam,   1.00f);
    const float transition  = load(tuneTransitionParam,  load(tuneFlexAmountParam, 0.65f));
    const float sensitivity = load(tuneSensitivityParam, 0.65f);
    const float vibrato     = load(tuneVibratoParam,     0.0f);
    const float globalForm  = load(alterFormantParam,    0.0f);
    const uint16_t scaleMask = packScaleDegreeMask();

    te.setFlexAmount(transition);
    const float humanizeAmt = juce::jlimit(0.0f, 1.0f, load(tuneHumanizeAmountParam, 0.50f));
    const bool  humanizeBtn = (humanizeAmt > 0.005f) || (load(tuneHumanizeButtonParam, 0.0f) > 0.5f);
    te.setHumanizeAmount(humanizeAmt);
    te.setHumanizeEnabled(humanizeBtn);
    te.setNaturalEngine(load(tuneNaturalParam, 1.0f) > 0.5f);
    // Poly mode is clip-based now (Poly::ClipSession); the old real-time poly path stays off.
    te.setPolyphonicEnabled(false);
    te.setMidiCleanModeEnabled(load(tuneMidiCleanParam, 0.0f) > 0.5f);
    te.setCenterAmount(center);
    te.setVariationAmount(variation);
    te.setTransitionAmount(transition);
    te.setSensitivityAmount(sensitivity);
    te.setVibratoCents(vibrato);
    te.setVocalRange(juce::roundToInt(load(tuneVocalRangeParam, 0.0f)));
    te.setNaturalOnly(smoothRequested());
    te.setScale(scaleMask);

    float extraNoteFormant = 0.0f;
    PitchTraceQueue::HopOverrideSlot hopSlot {};
    const auto& ov = pitchQueue.getLiveOverride();
    if (pitchQueue.getHopOverride(engineHop, hopSlot))
    {
        extraNoteFormant = hopSlot.formantSemitones;
        te.setLiveNoteOverride(true, hopSlot.targetMidi, hopSlot.noteCenter, hopSlot.noteVariation, hopSlot.gainDb,
                               hopSlot.rampInRatio, hopSlot.sibilantDb, hopSlot.hasCustomPitch, hopSlot.customPitchMidi,
                               hopSlot.fadeMultiplier, hopSlot.rampOutRatio, hopSlot.pitchEdit);
    }
    else if (allowLiveOverride && ov.active.load(std::memory_order_relaxed)) // a playing clip: notes without their own edit stay as they are
    {
        extraNoteFormant = ov.formantSemitones.load(std::memory_order_relaxed);
        te.setLiveNoteOverride(true,
            ov.targetMidiFloat.load(std::memory_order_relaxed), ov.noteCenter.load(std::memory_order_relaxed),
            ov.noteVariation.load(std::memory_order_relaxed), ov.gainDb.load(std::memory_order_relaxed),
            ov.rampInRatio.load(std::memory_order_relaxed), ov.sibilantDb.load(std::memory_order_relaxed),
            ov.hasCustomPitch.load(std::memory_order_relaxed), ov.customPitchMidi.load(std::memory_order_relaxed),
            ov.fadeMultiplier.load(std::memory_order_relaxed), ov.rampOutRatio.load(std::memory_order_relaxed));
    }
    else
    {
        te.setLiveNoteOverride(false, 60.0f, 1.0f, 1.0f, 0.0f, 0.18f);
    }

    ae.setPitch(load(alterPitchParam, 0.0f));
    ae.setFormant(juce::jlimit(-12.0f, 12.0f, globalForm + extraNoteFormant));
    ae.setMix(load(alterMixParam, 1.0f));
    ae.setSoloWet(load(alterSoloWetParam, 0.0f) > 0.5f);
    return extraNoteFormant;
}

bool NovaToneAudioProcessor::renderMonoClipOffline(const juce::AudioBuffer<float>& clip, double clipRate, int hopSamples,
                                                  juce::AudioBuffer<float>& out, const std::function<bool(float)>& progress)
{
    const int chans = juce::jlimit(1, 2, clip.getNumChannels());
    const int len = clip.getNumSamples();
    if (len <= 0 || clipRate <= 0.0) return false;
    constexpr int blockSize = 512;
    const juce::dsp::ProcessSpec spec { clipRate, (juce::uint32) blockSize, (juce::uint32) chans };

    auto te = std::make_unique<TuneEngine>();
    auto ae = std::make_unique<AlterEngine>();
    auto tuneBypass = std::make_unique<ModuleBypass>();
    auto alterBypass = std::make_unique<ModuleBypass>();
    te->prepare(spec);
    te->setLookaheadEnabled(lookaheadRequested());
    ae->prepare(spec);
    const bool tuneOff = tuneBypassParam != nullptr && tuneBypassParam->load() > 0.5f;
    tuneBypass->prepare(spec, te->getLatencySamples(), tuneOff, te->getLatencySamples());
    alterBypass->prepare(spec, ae->getLatencySamples(), true);
    const int latency = te->getLatencySamples() + ae->getLatencySamples();

    auto dB = [](const std::atomic<float>* p) { return juce::Decibels::decibelsToGain(p != nullptr ? p->load() : 0.0f); };
    const float inGain = dB(inputGainParam), outGain = dB(outputGainParam);
    const float globalForm = alterFormantParam != nullptr ? alterFormantParam->load() : 0.0f;
    const float alterPitch = alterPitchParam != nullptr ? alterPitchParam->load() : 0.0f;
    const int hop = juce::jmax(1, hopSamples);

    out.setSize(chans, len);
    out.clear();
    juce::AudioBuffer<float> io(chans, blockSize);
    using Context = juce::dsp::ProcessContextReplacing<float>;
    int blockIndex = 0;
    for (int pos = 0; pos < len + latency; pos += blockSize, ++blockIndex)
    {
        io.clear();
        const int avail = juce::jlimit(0, blockSize, len - pos);
        for (int ch = 0; ch < chans; ++ch)
            if (avail > 0) io.copyFrom(ch, 0, clip, juce::jmin(ch, clip.getNumChannels() - 1), pos, avail);
        io.applyGain(inGain);

        // Same as playback: the edit at the playhead (which reads the position after the block)
        const int engineHop = juce::jmin(PitchTraceQueue::maxTimelineHops - 1, (pos + blockSize) / hop);
        const float noteForm = configureEngines(*te, *ae, engineHop, false);
        const bool alterNeeded = std::abs(globalForm + noteForm) > 0.05f || std::abs(alterPitch) > 0.05f;

        juce::dsp::AudioBlock<float> block(io);
        Context ctx(block);
        tuneBypass->process(ctx, tuneOff, [&](const Context& c) { te->process(c); }, [&] { te->reset(); });
        alterBypass->process(ctx, tuneOff || ! alterNeeded, [&](const Context& c) { ae->process(c); }, [&] { ae->reset(); });
        io.applyGain(outGain);

        // drop the chain's latency so the file lines up with the original
        for (int i = 0; i < blockSize; ++i)
        {
            const int o = pos + i - latency;
            if (o < 0 || o >= len) continue;
            for (int ch = 0; ch < chans; ++ch)
                out.setSample(ch, o, io.getSample(ch, i));
        }
        if (progress && blockIndex % 64 == 0 && ! progress((float) pos / (float) (len + latency)))
            return false;
    }
    return true;
}

void NovaToneAudioProcessor::applyHarmonyVoices(HarmonyEngine& h) noexcept
{
    auto loadP = [](std::atomic<float>* p, float def) { return p != nullptr ? p->load(std::memory_order_relaxed) : def; };
    for (int v = 0; v < HarmonyEngine::numVoices; ++v)
    {
        const auto& hp = harmVoiceParams[(size_t)v];
        HarmonyEngine::VoiceSettings vs;
        vs.on = loadP(hp.on, 0.0f) > 0.5f;
        vs.mode = juce::roundToInt(loadP(hp.mode, 0.0f));
        vs.levelDb = loadP(hp.level, -6.0f);
        vs.pan = loadP(hp.pan, 0.0f);
        vs.formant = loadP(hp.formant, 0.0f);
        h.setVoice(v, vs);
    }
}

void NovaToneAudioProcessor::applyHarmonyMix(HarmonyEngine& h) noexcept
{
    auto loadP = [](std::atomic<float>* p, float def) { return p != nullptr ? p->load(std::memory_order_relaxed) : def; };
    h.setHumanize(loadP(harmHumanizeParam, 0.4f));
    h.setHarmonyGainDb(loadP(harmMixParam, 0.0f));
    h.setLeadGainDb(loadP(harmLeadParam, 0.0f));
    h.setScaleMask(packScaleDegreeMask());
    h.setKeyRoot(currentRoot());
}

bool NovaToneAudioProcessor::renderChoirPartsOffline(const juce::File& wavOut, const std::vector<int>& parts,
                                                     std::vector<int>& lo, std::vector<int>& hi,
                                                     double& rateOut, juce::int64& startOut,
                                                     const std::function<bool(float)>& progress, juce::String& error)
{
    // The lead: the Tune tab's Mono clip (a take or a loaded file), tuned exactly as playback / Export WAV
    std::shared_ptr<const juce::AudioBuffer<float>> clip;
    double clipRate = 48000.0; int hop = 512;
    auto& mp = pitchQueue.monoPreview;
    {
        const juce::SpinLock::ScopedLockType sl(mp.lock);
        clip = mp.clip; clipRate = mp.clipRate; hop = mp.hopSamples;
    }
    if (clip == nullptr || clip->getNumSamples() == 0)
    { error = "No lead vocal yet: record a take or load your vocal in the Tune tab (Mono) first."; return false; }
    const auto chords = getHarmonyMidi();
    if (chords == nullptr || chords->segments.empty())
    { error = "No chords yet: import a MIDI file or write chords first."; return false; }

    juce::AudioBuffer<float> lead;
    if (! renderMonoClipOffline(*clip, clipRate, hop, lead, [&](float p) { return ! progress || progress(0.45f * p); }))
    { error = "Cancelled."; return false; }

    const double sessionRate = getSampleRate() > 0.0 ? getSampleRate() : clipRate;
    const juce::int64 songStart = std::max<juce::int64>(0, mp.timelineStart.load());   // a loaded file starts at bar 1
    const double bpm = juce::jmax(1.0, pitchQueue.hostBpm.load());
    const double off = harmMidiOffsetParam != nullptr ? harmMidiOffsetParam->load() : 0.0;

    constexpr int blockSize = 512;
    auto h = std::make_unique<HarmonyEngine>();
    h->prepare(clipRate, blockSize);
    applyHarmonyVoices(*h);
    applyHarmonyMix(*h);
    h->setUseSidechainChords(false);
    h->setMidiMode(false);
    const int lat = h->getMinTotalLatency();
    h->setTotalLatency(lat);

    const int len = lead.getNumSamples();
    constexpr int numStems = 1 + HarmonyEngine::numVoices;   // lead + voices, as the Choirize recorder writes them
    juce::AudioBuffer<float> stems(numStems, len);
    stems.clear();
    lo.assign(parts.size(), 0); hi.assign(parts.size(), 0);
    juce::AudioBuffer<float> dry(1, blockSize), io(1, blockSize);
    for (int pos = 0, blk = 0; pos < len + lat; pos += blockSize, ++blk)
    {
        dry.clear();
        const int avail = juce::jlimit(0, blockSize, len - pos);
        if (avail > 0)
        {
            dry.copyFrom(0, 0, lead, 0, pos, avail);
            if (lead.getNumChannels() > 1) { dry.addFrom(0, 0, lead, 1, pos, avail); dry.applyGain(0.5f); }
        }
        io.makeCopyOf(dry, true);
        // chord at this point of the song (same rule as playback: file position = song position - offset)
        const double songSec = (double) songStart / sessionRate + (double) pos / clipRate;
        const auto* seg = chords->at(songSec * bpm / 60.0 - off);
        h->setExternalChord(true, seg != nullptr, seg != nullptr ? seg->mask : 0, seg != nullptr ? seg->root : -1, seg != nullptr ? seg->quality : 0);
        h->process(dry, io);
        for (size_t k = 0; k < parts.size(); ++k)
            if (const int n = h->getVoiceNote(parts[k] - 1); n > 0)
            { lo[k] = lo[k] == 0 ? n : juce::jmin(lo[k], n); hi[k] = juce::jmax(hi[k], n); }
        // drop the engine's latency so the parts line up with the lead
        for (int i = 0; i < blockSize; ++i)
        {
            const int o = pos + i - lat;
            if (o < 0 || o >= len) continue;
            stems.setSample(0, o, h->getLeadStem()[i]);
            for (int v = 0; v < HarmonyEngine::numVoices; ++v) stems.setSample(v + 1, o, h->getVoiceStem(v)[i]);
        }
        if (progress && blk % 64 == 0 && ! progress(0.45f + 0.5f * (float) pos / (float) (len + lat)))
        { error = "Cancelled."; return false; }
    }

    // the Choir plays takes at the session's rate
    if (std::abs(clipRate - sessionRate) > 0.5)
    {
        const double ratio = clipRate / sessionRate;
        const int outLen = (int) std::floor(len / ratio);
        juce::AudioBuffer<float> res(numStems, outLen);
        for (int ch = 0; ch < numStems; ++ch)
        { juce::LagrangeInterpolator li; li.process(ratio, stems.getReadPointer(ch), res.getWritePointer(ch), outLen); }
        stems = std::move(res);
    }

    wavOut.getParentDirectory().createDirectory();
    wavOut.deleteFile();
    auto stream = std::make_unique<juce::FileOutputStream>(wavOut);
    if (! stream->openedOk()) { error = "Can't write " + wavOut.getFullPathName(); return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(stream.get(), sessionRate, (unsigned int) numStems, 24, {}, 0));
    if (w == nullptr) { error = "Can't create the WAV writer."; return false; }
    stream.release();
    if (! w->writeFromAudioSampleBuffer(stems, 0, stems.getNumSamples())) { error = "Writing the parts failed."; return false; }
    rateOut = sessionRate;
    startOut = songStart;
    if (progress) progress(1.0f);
    return true;
}

void NovaToneAudioProcessor::pushParametersToEngines() noexcept
{
    auto load = [](const std::atomic<float>* p, float fallback) noexcept {
        return p != nullptr ? p->load(std::memory_order_relaxed) : fallback;
    };

    // The roll's copy of the knobs
    const float humanizeAmt = juce::jlimit(0.0f, 1.0f, load(tuneHumanizeAmountParam, 0.50f));
    pitchQueue.globalHumanizeAmount.store(humanizeAmt, std::memory_order_relaxed);
    pitchQueue.globalHumanize.store((humanizeAmt > 0.005f) || (load(tuneHumanizeButtonParam, 0.0f) > 0.5f), std::memory_order_relaxed);
    pitchQueue.globalPolyphonic.store(load(tunePolyphonicParam, 0.0f) > 0.5f, std::memory_order_relaxed);
    pitchQueue.globalMidiCleanMode.store(load(tuneMidiCleanParam, 0.0f) > 0.5f, std::memory_order_relaxed);
    pitchQueue.vocalRangeMode.store(juce::roundToInt(load(tuneVocalRangeParam, 0.0f)), std::memory_order_relaxed);
    pitchQueue.globalCenter.store(load(tuneCenterParam, 0.85f), std::memory_order_relaxed);
    pitchQueue.globalVariation.store(load(tuneVariationParam, 1.0f), std::memory_order_relaxed);
    pitchQueue.globalTransition.store(load(tuneTransitionParam, load(tuneFlexAmountParam, 0.65f)), std::memory_order_relaxed);
    pitchQueue.globalSensitivity.store(load(tuneSensitivityParam, 0.65f), std::memory_order_relaxed);
    pitchQueue.globalFormant.store(load(alterFormantParam, 0.0f), std::memory_order_relaxed);
    pitchQueue.activeScaleMask.store(packScaleDegreeMask(), std::memory_order_relaxed);

    const bool lookahead = lookaheadRequested();
    if (lookahead != tuneEngine.isLookaheadEnabled())
    {
        tuneEngine.setLookaheadEnabled(lookahead);
        bypass[tuneModule].setLatency(tuneEngine.getLatencySamples());
    }

    const int playHop = pitchQueue.currentPlayheadHop.load(std::memory_order_relaxed);
    const int engineHop = (playHop > 0) ? playHop : (tuneEngine.getCurrentHopIndex() % PitchTraceQueue::maxTimelineHops);
    configureEngines(tuneEngine, alterEngine, engineHop, playHop <= 0);

    for (int v = 0; v < PitchTraceQueue::maxPolyVoices; ++v)
    {
        const auto& pov = pitchQueue.getPolyVoiceOverride(v);
        tuneEngine.setPolyVoiceOverride(v, pov.active.load(std::memory_order_relaxed),
            pov.detectedMidiRef.load(std::memory_order_relaxed), pov.targetMidiFloat.load(std::memory_order_relaxed),
            pov.noteCenter.load(std::memory_order_relaxed), pov.noteVariation.load(std::memory_order_relaxed),
            pov.gainDb.load(std::memory_order_relaxed));
    }

    inputGain.setGainDecibels(load(inputGainParam, 0.0f));
    outputGain.setGainDecibels(load(outputGainParam, 0.0f));
}

void NovaToneAudioProcessor::publishPeaks(const juce::dsp::AudioBlock<float>& block, PeakMeter& meter) noexcept
{
    for (size_t m = 0; m < meter.size(); ++m)
    {
        const size_t ch = juce::jmin(m, block.getNumChannels() - 1);
        const auto range = juce::FloatVectorOperations::findMinAndMax(block.getChannelPointer(ch), (int)block.getNumSamples());
        const float peak = juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
        if (peak > meter[m].load(std::memory_order_relaxed))
            meter[m].store(peak, std::memory_order_relaxed);
    }
}

void NovaToneAudioProcessor::captureMonoTake(const juce::AudioBuffer<float>& buffer, std::optional<juce::int64> hostSample, bool hostPlaying) noexcept
{
    using T = PitchTraceQueue::MonoTake;
    auto& t = pitchQueue.monoTake;
    auto& mp = pitchQueue.monoPreview;
    const bool polyMode = tunePolyphonicParam != nullptr && tunePolyphonicParam->load(std::memory_order_relaxed) > 0.5f;
    if (polyMode)
        return;

    const int n = buffer.getNumSamples();
    const bool songPos = hostPlaying && hostSample.has_value();

    auto begin = [&] (bool anchored)
    {
        t.length.store(0, std::memory_order_relaxed);
        t.peak = 0.0f;
        t.songStart.store(anchored ? *hostSample : -1, std::memory_order_relaxed);
        t.nextHostSample = anchored ? *hostSample : -1;
        t.stopRequested.store(false, std::memory_order_relaxed);
    };
    auto finish = [&]
    {
        // A take that is too short or silent isn't worth replacing the roll with.
        const bool keep = t.length.load(std::memory_order_relaxed) > static_cast<int>(t.rate * 0.3) && t.peak > 0.002f;
        t.state.store(keep ? T::done : T::idle, std::memory_order_release);
    };

    int st = t.state.load(std::memory_order_acquire);
    if (st == T::done)
        return;
    if (st == T::idle)
    {
        // Automatic take: the DAW plays, Mono is on, nothing is loaded yet, and we're not previewing.
        if (! songPos || mp.loaded.load(std::memory_order_relaxed) || mp.playing.load(std::memory_order_relaxed))
            return;
        if (! t.state.compare_exchange_strong(st, T::capturing, std::memory_order_acq_rel))
            return;
        begin(true);
    }
    else if (st == T::armed)
    {
        // Rec pressed: start with the DAW, or (no DAW playing, e.g. Standalone) on the first sound.
        const float pk = buffer.getMagnitude(0, n);
        if (! songPos && pk < 0.003f)
            return;
        if (! t.state.compare_exchange_strong(st, T::capturing, std::memory_order_acq_rel))
            return;
        begin(songPos);
    }

    if (t.stopRequested.exchange(false, std::memory_order_relaxed)) { finish(); return; }

    const bool anchored = t.songStart.load(std::memory_order_relaxed) >= 0;
    if (anchored)
    {
        // DAW stopped, or jumped (loop / click on the timeline): the take ends here.
        if (! songPos) { finish(); return; }
        if (std::llabs(*hostSample - t.nextHostSample) > 1) { finish(); return; }
        t.nextHostSample = *hostSample + n;
    }

    const int written = t.append(buffer, n);
    if (written > 0)
        t.peak = juce::jmax(t.peak, buffer.getMagnitude(0, written));
    if (written < n)
        finish(); // full (10 min) or the allocator fell behind
}

void NovaToneAudioProcessor::renderMonoPreview(juce::AudioBuffer<float>& buffer, std::optional<juce::int64> hostSample, bool hostPlaying)
{
    auto& mp = pitchQueue.monoPreview;
    const bool polyMode = tunePolyphonicParam != nullptr && tunePolyphonicParam->load(std::memory_order_relaxed) > 0.5f;
    if (polyMode || ! mp.loaded.load(std::memory_order_relaxed))
        return;

    const bool independent = pitchQueue.independentPlay.load(std::memory_order_relaxed);
    const int takeState = pitchQueue.monoTake.state.load(std::memory_order_relaxed);
    const bool recordingTake = takeState == PitchTraceQueue::MonoTake::armed || takeState == PitchTraceQueue::MonoTake::capturing;
    const juce::int64 songStart = mp.timelineStart.load(std::memory_order_relaxed);

    bool inPlace = false;
    if (hostPlaying && ! independent)
    {
        // DAW transport wins over the preview. A recorded take plays in place at its song position
        // (edits audible in the song, like Poly); a dropped file isn't tied to the song, so live input passes.
        mp.playing.store(false, std::memory_order_relaxed);
        if (recordingTake || songStart < 0 || ! hostSample.has_value())
            return;
        inPlace = true;
    }
    else if (! mp.playing.load(std::memory_order_relaxed))
        return;

    const juce::SpinLock::ScopedTryLockType tl(mp.lock);
    if (! tl.isLocked() || mp.clip == nullptr || getSampleRate() <= 0.0)
        return;

    const auto& clip = *mp.clip;
    const int len = clip.getNumSamples();
    const int n = buffer.getNumSamples();
    const double step = mp.clipRate / getSampleRate();
    double pos = inPlace ? static_cast<double>(*hostSample - songStart) * step : mp.pos.load(std::memory_order_relaxed);
    const double blockStartPos = pos;
    for (int i = 0; i < n; ++i, pos += step)
    {
        if (pos < 0.0 || pos >= len)
        {
            if (inPlace) continue; // outside the take: keep the live input
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample(ch, i, 0.0f);
            continue;
        }
        // shortcut: linear interpolation for preview resampling; fine for monitoring, not for bouncing.
        const int i0 = static_cast<int>(pos);
        const float frac = static_cast<float>(pos - i0);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const int sc = juce::jmin(ch, clip.getNumChannels() - 1);
            const float a = clip.getSample(sc, i0);
            const float b = i0 + 1 < len ? clip.getSample(sc, i0 + 1) : 0.0f;
            buffer.setSample(ch, i, a + frac * (b - a));
        }
    }

    const int hopSamples = juce::jmax(1, mp.hopSamples);
    if (inPlace)
    {
        const bool inside = blockStartPos >= 0.0 && blockStartPos < len;
        pitchQueue.currentPlayheadHop.store(inside ? static_cast<int>(blockStartPos / hopSamples) : -1, std::memory_order_relaxed);
        if (inside) mp.pos.store(blockStartPos, std::memory_order_relaxed); // Space after the DAW stops continues from here
        return;
    }
    mp.pos.store(pos, std::memory_order_relaxed);
    pitchQueue.currentPlayheadHop.store(static_cast<int>(pos / hopSamples), std::memory_order_relaxed);
    if (pos >= len)
        mp.playing.store(false, std::memory_order_relaxed);
}

void NovaToneAudioProcessor::processBlock(juce::AudioBuffer<float>& fullBuffer, juce::MidiBuffer& midiMessages)
{
    // Engines size their scratch buffers to prepareToPlay's block size; some hosts send more.
    if (preparedBlockSize > 0 && fullBuffer.getNumSamples() > preparedBlockSize)
    {
        // shortcut: incoming MIDI goes to the first chunk and later chunks' MIDI output lands at their own start
        // offset 0; fine for note-level MIDI, revisit if sample-accurate MIDI matters.
        juce::MidiBuffer rest;
        for (int start = 0; start < fullBuffer.getNumSamples(); start += preparedBlockSize)
        {
            const int len = juce::jmin(preparedBlockSize, fullBuffer.getNumSamples() - start);
            juce::AudioBuffer<float> chunk(fullBuffer.getArrayOfWritePointers(), fullBuffer.getNumChannels(), start, len);
            juce::MidiBuffer& m = start == 0 ? midiMessages : rest;
            if (start > 0) rest.clear();
            processBlock(chunk, m);
            if (start > 0) midiMessages.addEvents(rest, 0, len, start);
        }
        return;
    }
    juce::ScopedNoDenormals noDenormals;

    // Main bus only from here on; the sidechain (if the host connected one) feeds the Harmony tab.
    const int mainChannels = juce::jmin(fullBuffer.getNumChannels(), juce::jmax(1, getMainBusNumOutputChannels()));
    juce::AudioBuffer<float> buffer(fullBuffer.getArrayOfWritePointers(), mainChannels, fullBuffer.getNumSamples());
    const bool hasSidechain = getBusCount(true) > 1 && getBus(true, 1) != nullptr && getBus(true, 1)->isEnabled()
                              && getBus(true, 1)->getNumberOfChannels() > 0;
    sidechainConnected.store(hasSidechain, std::memory_order_relaxed);

    // Upgrade #4: Parse incoming live MIDI notes for Real-Time MIDI Pitch Targeting & Ghost Notes:
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
        {
            const int n = juce::jlimit(0, 127, msg.getNoteNumber());
            heldMidiNotes[(size_t)n] = true;
            lastActiveMidiNote = n;
        }
        else if (msg.isNoteOff())
        {
            const int n = juce::jlimit(0, 127, msg.getNoteNumber());
            heldMidiNotes[(size_t)n] = false;
            if (lastActiveMidiNote == n)
            {
                lastActiveMidiNote = -1;
                for (int k = 127; k >= 0; --k)
                    if (heldMidiNotes[(size_t)k]) { lastActiveMidiNote = k; break; }
            }
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            heldMidiNotes.fill(false);
            lastActiveMidiNote = -1;
        }
    }

    uint16_t midiMask = 0;
    for (int k = 0; k < 128; ++k)
        if (heldMidiNotes[(size_t)k])
            midiMask |= static_cast<uint16_t>(1u << (k % 12));

    tuneEngine.setMidiTargetNote(lastActiveMidiNote, midiMask);
    pitchQueue.liveMidiTargetNote.store(lastActiveMidiNote, std::memory_order_relaxed);
    pitchQueue.liveMidiNoteMask.store(midiMask, std::memory_order_relaxed);

    for (auto ch = getMainBusNumInputChannels(); ch < mainChannels; ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // Host transport (sample position + playing state) for the clips
    std::optional<juce::int64> hostSample;
    bool hostPlaying = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            hostPlaying = pos->getIsPlaying();
            if (pos->getTimeInSamples().hasValue()) hostSample = *pos->getTimeInSamples();
        }

    pitchQueue.hostPlaying.store(hostPlaying, std::memory_order_relaxed);
    captureMonoTake(buffer, hostSample, hostPlaying);   // raw input, before the preview replaces it
    renderMonoPreview(buffer, hostSample, hostPlaying);
    pushParametersToEngines();

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    inputGain.process(context);
    publishPeaks(block, inputPeak);

    const bool polyMode = tunePolyphonicParam != nullptr && tunePolyphonicParam->load(std::memory_order_relaxed) > 0.5f;
    if (polyMode)
        polyClip.capture(buffer, hostSample, hostPlaying);

    // Harmony: settings, sidechain chords, and a copy of the dry lead for the voices
    auto loadP = [](std::atomic<float>* p, float def) { return p != nullptr ? p->load(std::memory_order_relaxed) : def; };
    applyHarmonyVoices(harmony);
    for (const auto m : midiMessages)   // held notes for the Harmony tab's MIDI mode
    {
        const auto msg = m.getMessage();
        if (msg.isNoteOn()) midiHeld[(size_t) msg.getNoteNumber()] = true;
        else if (msg.isNoteOff()) midiHeld[(size_t) msg.getNoteNumber()] = false;
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) midiHeld.fill(false);
    }
    harmony.setMidiNotes(midiHeld.data());
    const bool harmonyOn = harmony.anyVoiceOn();
    if (hasSidechain)
        harmony.processSidechain(getBusBuffer(fullBuffer, true, 1));
    if (harmonyOn && harmonyDry.getNumSamples() >= buffer.getNumSamples())
        for (int ch = 0; ch < juce::jmin(harmonyDry.getNumChannels(), buffer.getNumChannels()); ++ch)
            harmonyDry.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());

    using Context = juce::dsp::ProcessContextReplacing<float>;
    // In poly mode the live (mono) tuner is bypassed; edits come from the rendered clip.
    const bool tuneBypassed = polyMode || (tuneBypassParam != nullptr && tuneBypassParam->load(std::memory_order_relaxed) > 0.5f);

    const float globalForm = alterFormantParam != nullptr ? alterFormantParam->load(std::memory_order_relaxed) : 0.0f;
    float noteForm = 0.0f;
    {
        const int playHop = pitchQueue.currentPlayheadHop.load(std::memory_order_relaxed);
        const int engineHop = (playHop > 0) ? playHop : (tuneEngine.getCurrentHopIndex() % PitchTraceQueue::maxTimelineHops);
        PitchTraceQueue::HopOverrideSlot hopSlot {};
        if (pitchQueue.getHopOverride(engineHop, hopSlot))
            noteForm = hopSlot.formantSemitones;
        else if (pitchQueue.getLiveOverride().active.load(std::memory_order_relaxed))
            noteForm = pitchQueue.getLiveOverride().formantSemitones.load(std::memory_order_relaxed);
    }
    const float alterPitch = alterPitchParam != nullptr ? alterPitchParam->load(std::memory_order_relaxed) : 0.0f;
    const bool alterNeeded = (std::abs(globalForm + noteForm) > 0.05f) || (std::abs(alterPitch) > 0.05f);

    bypass[tuneModule].process(context, tuneBypassed,
        [this](const Context& c) { tuneEngine.process(c); }, [this] { tuneEngine.reset(); });
    bypass[alterModule].process(context, tuneBypassed || !alterNeeded,
        [this](const Context& c) { alterEngine.process(c); }, [this] { alterEngine.reset(); });

    // Inside the clip's range the rendered clip replaces the (latency-matched) dry signal.
    if (polyMode)
        polyClip.playback(buffer, hostSample, hostPlaying, getLatencySamples());

    // Choir playing (and not recording new parts): the AI choir replaces the live harmonies
    // ---- Harmony "MIDI file" source: chord at the song position (host, or a playing Mono take) ----
    {
        double posPpq = 0.0, blockPpq = 0.0, bpmNow = 120.0;
        bool moving = false;
        if (auto* ph = getPlayHead())
            if (const auto pos = ph->getPosition())
            {
                if (pos->getPpqPosition().hasValue()) posPpq = *pos->getPpqPosition();
                if (pos->getBpm().hasValue()) bpmNow = *pos->getBpm();
                moving = hostPlaying;
            }
        const double sr = juce::jmax(1.0, getSampleRate());
        auto& mp = pitchQueue.monoPreview;
        const juce::int64 takeStart = mp.timelineStart.load(std::memory_order_relaxed);
        if (! hostPlaying && mp.playing.load(std::memory_order_relaxed) && takeStart >= 0 && mp.clipRate > 0.0)
        {
            // NovaTone's own transport playing a take: its song position = where it was recorded + play position
            const double songSamples = (double) takeStart + mp.pos.load(std::memory_order_relaxed) * (sr / mp.clipRate);
            posPpq = songSamples / sr * bpmNow / 60.0;
            moving = true;
        }
        blockPpq = buffer.getNumSamples() / sr * bpmNow / 60.0;
        harmonyPpq.store(posPpq, std::memory_order_relaxed);
        harmonyPpqMoving.store(moving, std::memory_order_relaxed);

        const bool fileSource = juce::roundToInt(loadP(harmSourceParam, 0.0f)) == 3;
        bool valid = false; uint16_t mask = 0; int root = -1, quality = 0;
        if (fileSource)
        {
            const juce::SpinLock::ScopedTryLockType tl(harmonyMidiLock);
            if (tl.isLocked() && harmonyMidi != nullptr)
                if (const auto* seg = harmonyMidi->at(posPpq - (double) loadP(harmMidiOffsetParam, 0.0f)))
                { valid = true; mask = seg->mask; root = seg->root; quality = seg->quality; }
        }
        harmony.setExternalChord(fileSource, valid, mask, root, quality);

        // Lead pitch by song position (for Auto-align): one sample per block while the song moves
        if (leadHistoryClear.exchange(false)) { leadHistoryWrite = 0; leadHistoryCount.store(0); }
        if (moving && ! tuneBypassed && tuneEngine.isVoiced() && tuneEngine.getUncorrectedPitchHz() > 0.0f)
        {
            leadHistoryPpq[(size_t) leadHistoryWrite]  = (float) posPpq;
            leadHistoryLen[(size_t) leadHistoryWrite]  = (float) blockPpq;
            leadHistoryMidi[(size_t) leadHistoryWrite] = 69.0f + 12.0f * std::log2(tuneEngine.getUncorrectedPitchHz() / 440.0f);
            leadHistoryWrite = (leadHistoryWrite + 1) & (leadHistoryCap - 1);
            leadHistoryCount.store(juce::jmin(leadHistoryCap, leadHistoryCount.load(std::memory_order_relaxed) + 1), std::memory_order_release);
        }
    }

    const auto recState = choirRecorder.getState();
    const bool choirRecording = recState == HarmonyExporter::State::armed || recState == HarmonyExporter::State::recording;
    if (harmonyOn && ! (choir.enabled.load(std::memory_order_relaxed) && ! choirRecording) && harmonyDry.getNumSamples() >= buffer.getNumSamples())
    {
        applyHarmonyMix(harmony);
        harmony.setUseSidechainChords(juce::roundToInt(loadP(harmSourceParam, 0.0f)) == 0);
        harmony.setMidiMode(juce::roundToInt(loadP(harmSourceParam, 0.0f)) == 2);
        harmony.setTotalLatency(getLatencySamples());
        juce::AudioBuffer<float> dryView(harmonyDry.getArrayOfWritePointers(), juce::jmin(harmonyDry.getNumChannels(), buffer.getNumChannels()), buffer.getNumSamples());
        harmony.process(dryView, buffer);
        harmonyExporter.push(harmony.getHarmonyOutL(), harmony.getHarmonyOutR(), buffer.getNumSamples(), hostSample, hostPlaying, getLatencySamples());
        const float* stems[9] = { harmony.getLeadStem() };
        for (int v = 0; v < HarmonyEngine::numVoices; ++v) stems[v + 1] = harmony.getVoiceStem(v);
        choirRecorder.push(stems, buffer.getNumSamples(), hostSample, hostPlaying, getLatencySamples());
    }
    else
    {
        harmonyExporter.push(nullptr, nullptr, buffer.getNumSamples(), hostSample, hostPlaying, getLatencySamples());
        choirRecorder.push(static_cast<const float* const*>(nullptr), buffer.getNumSamples(), hostSample, hostPlaying, getLatencySamples());
    }
    choir.render(buffer.getWritePointer(0), buffer.getWritePointer(juce::jmin(1, buffer.getNumChannels() - 1)), buffer.getNumSamples(), hostSample, hostPlaying, getLatencySamples());

    outputGain.process(context);
    publishPeaks(block, outputPeak);

    double ppq = 0.0;
    double bpm = 120.0;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            if (pos->getPpqPosition().hasValue()) ppq = *pos->getPpqPosition();
            if (pos->getBpm().hasValue())         bpm = *pos->getBpm();
        }
    }
    pitchQueue.hostBpm.store(bpm, std::memory_order_relaxed);

    // MIDI clean turned off / Tune bypassed / Poly: the block below stops running, so release its notes now.
    if (! tuneEngine.isMidiCleanModeEnabled() || tuneBypassed)
        for (int m = 0; m < 128; ++m)
            if (activeOutputMidiNotes[(size_t)m])
            {
                midiMessages.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                activeOutputMidiNotes[(size_t)m] = false;
            }

    const int currentHopIdx = tuneEngine.getCurrentHopIndex();
    if (currentHopIdx == lastPublishedHopIndex)
        return;
    lastPublishedHopIndex = currentHopIdx;

    PitchTraceQueue::PitchData hopPacket {
        tuneEngine.getUncorrectedPitchHz(),
        tuneEngine.getCorrectedPitchHz(),
        tuneEngine.isVoiced(),
        tuneEngine.getLastRms(),
        tuneEngine.getLastClarity(),
        tuneEngine.isSibilant(),
        currentHopIdx,
        -1,
        ppq,
        bpm
    };
    hopPacket.activeMidiInputNote = lastActiveMidiNote;
    hopPacket.midiInputMask       = midiMask;
    hopPacket.isPolyphonic        = tuneEngine.isPolyphonicEnabled();
    hopPacket.numPolyVoices = static_cast<uint8_t>(juce::jlimit(0, PitchTraceQueue::maxPolyVoices, tuneEngine.getNumPolyVoices()));
    for (int v = 0; v < PitchTraceQueue::maxPolyVoices; ++v)
    {
        hopPacket.polyRawMidi[(size_t)v]       = tuneEngine.getPolyRawMidi(v);
        hopPacket.polyCorrectedMidi[(size_t)v] = tuneEngine.getPolyCorrectedMidi(v);
        hopPacket.polyVoiceRms[(size_t)v]      = tuneEngine.getPolyVoiceRms(v);
    }
    pitchQueue.push(hopPacket);

    // Real-Time Audio-to-MIDI Output when MIDI Clean Mode is active:
    if (tuneEngine.isMidiCleanModeEnabled())
    {
        std::array<bool, 128> desiredMidiNotes {};
        const juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(45, 127, juce::roundToInt(tuneEngine.getLastRms() * 240.0f + 50.0f)));
        if (tuneEngine.isPolyphonicEnabled() && tuneEngine.getNumPolyVoices() > 0)
        {
            for (int v = 0; v < tuneEngine.getNumPolyVoices(); ++v)
            {
                const int m = juce::jlimit(0, 127, static_cast<int>(std::lround(tuneEngine.getPolyCorrectedMidi(v))));
                if (m >= 24 && m <= 108)
                    desiredMidiNotes[(size_t)m] = true;
            }
        }
        else if (tuneEngine.isVoiced() && tuneEngine.getCorrectedPitchHz() > 0.0f)
        {
            const int m = juce::jlimit(0, 127, static_cast<int>(std::lround(Quantizer::hzToMidi(tuneEngine.getCorrectedPitchHz()))));
            if (m >= 24 && m <= 108)
                desiredMidiNotes[(size_t)m] = true;
        }

        for (int m = 0; m < 128; ++m)
        {
            if (desiredMidiNotes[(size_t)m] && !activeOutputMidiNotes[(size_t)m])
            {
                midiMessages.addEvent(juce::MidiMessage::noteOn(1, m, vel), 0);
                activeOutputMidiNotes[(size_t)m] = true;
            }
            else if (!desiredMidiNotes[(size_t)m] && activeOutputMidiNotes[(size_t)m])
            {
                midiMessages.addEvent(juce::MidiMessage::noteOff(1, m), 0);
                activeOutputMidiNotes[(size_t)m] = false;
            }
        }
    }
}

void NovaToneAudioProcessor::setHarmonyMidi(std::shared_ptr<const MidiChordTrack> t)
{
    std::shared_ptr<const MidiChordTrack> old;
    {
        const juce::SpinLock::ScopedLockType sl(harmonyMidiLock);
        old = std::move(harmonyMidi);
        harmonyMidi = std::move(t);
    }
    // `old` is released here, on the message thread
}

bool NovaToneAudioProcessor::loadHarmonyMidi(const juce::File& file, juce::String& error)
{
    auto t = MidiChordTrack::load(file, error);
    if (t == nullptr) return false;
    setHarmonyMidi(std::move(t));
    return true;
}

void NovaToneAudioProcessor::clearHarmonyMidi() { setHarmonyMidi(nullptr); }

std::shared_ptr<const MidiChordTrack> NovaToneAudioProcessor::getHarmonyMidi() const
{
    const juce::SpinLock::ScopedLockType sl(harmonyMidiLock);
    return harmonyMidi;
}

std::vector<MidiChordTrack::LeadNote> NovaToneAudioProcessor::getLeadHistory() const
{
    std::vector<MidiChordTrack::LeadNote> out;
    const int n = leadHistoryCount.load(std::memory_order_acquire);
    out.reserve((size_t) n);
    for (int i = 0; i < n; ++i)
        out.push_back({ leadHistoryPpq[(size_t) i], leadHistoryPpq[(size_t) i] + leadHistoryLen[(size_t) i], leadHistoryMidi[(size_t) i] });
    return out;
}

void NovaToneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild(state.getChildWithName("HarmonyMidi"), nullptr);
    if (const auto hm = getHarmonyMidi(); hm != nullptr && ! hm->segments.empty())
    {
        // the chords themselves (imported, then maybe edited / written in the chord editor)
        juce::ValueTree t("HarmonyMidi");
        t.setProperty("name", hm->fileName, nullptr);
        t.setProperty("segments", hm->toString(), nullptr);
        state.appendChild(t, nullptr);
    }
    state.removeChild(state.getChildWithName("PolyClip"), nullptr);
    if (auto clipTree = polyClip.toValueTree(); clipTree.isValid() && clipTree.hasProperty("file"))
        state.appendChild(clipTree, nullptr);
    state.setProperty("choirFolder", choir.isOpen() ? choir.getFolder().getFullPathName() : juce::String(), nullptr);
    choir.saveMix();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    if (xml != nullptr) copyXmlToBinary(*xml, destData);
}

void NovaToneAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
    {
        auto tree = juce::ValueTree::fromXml(*xml);
        const auto clipTree = tree.getChildWithName("PolyClip").createCopy();
        tree.removeChild(tree.getChildWithName("PolyClip"), nullptr);
        const auto midiTree = tree.getChildWithName("HarmonyMidi").createCopy();
        tree.removeChild(tree.getChildWithName("HarmonyMidi"), nullptr);
        {
            std::shared_ptr<const MidiChordTrack> hm;
            juce::MemoryBlock mb;
            if (midiTree.isValid() && midiTree.hasProperty("segments"))
                hm = MidiChordTrack::fromString(midiTree.getProperty("segments").toString(), midiTree.getProperty("name").toString());
            else if (midiTree.isValid() && mb.fromBase64Encoding(midiTree.getProperty("data").toString()))   // saved before the chord editor
            {
                juce::String err;
                hm = MidiChordTrack::fromData(mb, err);
                if (hm != nullptr) const_cast<MidiChordTrack&>(*hm).fileName = midiTree.getProperty("name").toString();
            }
            setHarmonyMidi(std::move(hm));
        }
        const auto choirFolder = tree.getProperty("choirFolder").toString();
        applyStateGuarded(tree);
        juce::MessageManager::callAsync([this, choirFolder, alive = juce::WeakReference<NovaToneAudioProcessor>(this)]
        {
            if (alive == nullptr) return;
            if (choirFolder.isNotEmpty()) choir.open(juce::File(choirFolder));
            else choir.close();
        });
        if (clipTree.isValid())
            polyClip.fromValueTree(clipTree);
        else
            polyClip.clear();
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout NovaToneAudioProcessor::createParameterLayout()
{
    using APF = juce::AudioParameterFloat;
    using APB = juce::AudioParameterBool;
    using NR  = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    const auto startKnobs = TuneMacro::forCharacter(TuneMacro::defaultCharacter, false);

    auto percentAttrs = []()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; })
            .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue() / 100.0f; });
    };

    auto semitoneAttrs = []()
    {
        return juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) {
                if (std::abs(v) < 0.05f) return juce::String("0.0 st");
                return (v > 0.0f ? "+" : "") + juce::String(v, 1) + " st";
            })
            .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue(); });
    };

    layout.add(std::make_unique<APF>("tune_flexAmount",     "Flex Amount", NR(0.0f, 1.0f, 0.001f), startKnobs.speed));
    // Defaults are dry: an imported file plays exactly as sung until notes are edited or the knobs are turned
    // (Center 0%, Variation 100%, Humanize 0%, Vibrato off). Transition / Sensitivity only matter once correcting.
    layout.add(std::make_unique<APF>("tune_humanizeAmount", "Humanize",    NR(0.0f, 1.0f, 0.001f), 0.0f, percentAttrs()));

    layout.add(std::make_unique<APF>("tune_center",      "Center",      NR(0.0f, 1.0f, 0.001f), 0.0f,                   percentAttrs()));
    layout.add(std::make_unique<APF>("tune_variation",   "Variation",   NR(0.0f, 2.0f, 0.001f), 1.0f,                   percentAttrs()));
    layout.add(std::make_unique<APF>("tune_transition",  "Transition",  NR(0.0f, 1.0f, 0.001f), startKnobs.transition,  percentAttrs()));
    layout.add(std::make_unique<APF>("tune_sensitivity", "Sensitivity", NR(0.0f, 1.0f, 0.001f), startKnobs.sensitivity, percentAttrs()));

    layout.add(std::make_unique<APB>("tune_snapScale",  "Snap to Scale", false));
    layout.add(std::make_unique<APB>("tune_autoScroll", "Auto-Scroll",   true));

    layout.add(std::make_unique<APB>("tune_humanize",   "Humanize Mode",   false));
    layout.add(std::make_unique<APB>("tune_polyphonic", "Polyphonic Mode", false));
    layout.add(std::make_unique<APB>("tune_midiClean",  "MIDI Clean Mode", false));
    layout.add(std::make_unique<APB>("tune_lookahead", "Lookahead", false));
    layout.add(std::make_unique<APB>("tune_natural", "Natural Engine", true));
    layout.add(std::make_unique<APB>("tune_smooth",    "Smooth",    false));
    layout.add(std::make_unique<APB>("tune_sync",      "Sync",      false));
    layout.add(std::make_unique<APF>("tune_character", "Soft/Hard", NR(0.0f, 1.0f, 0.001f), TuneMacro::defaultCharacter, percentAttrs()));
    layout.add(std::make_unique<APF>("tune_vibrato",   "Vibrato",   NR(0.0f, 100.0f, 1.0f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return v < 0.5f ? juce::String("OFF") : juce::String(juce::roundToInt(v)) + " c"; })
            .withValueFromStringFunction([](const juce::String& t) { return t.trim().equalsIgnoreCase("off") ? 0.0f : t.getFloatValue(); })));

    static constexpr int kCMajorDegrees[7] = { 0, 2, 4, 5, 7, 9, 11 };
    for (int i = 0; i < 12; ++i)
    {
        bool isInCMajor = false;
        for (int d : kCMajorDegrees) if (d == i) { isInCMajor = true; break; }
        layout.add(std::make_unique<APB>("tune_scaleDegree_" + juce::String(i), "Scale Degree " + juce::String(i), isInCMajor));
    }

    layout.add(std::make_unique<juce::AudioParameterChoice>("tune_root", "Key",
        juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>("tune_mode", "Scale",
        juce::StringArray { "Major", "Minor", "Custom" }, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>("tune_vocalRange", "Vocal Range",
        juce::StringArray { "Full", "Soprano", "Alto", "Tenor", "Bass" }, 0));

    layout.add(std::make_unique<APF>("alter_pitch",    "Global Transpose", NR(-12.0f, 12.0f, 0.01f), 0.0f, semitoneAttrs()));
    layout.add(std::make_unique<APF>("alter_formant",  "Global Formant",   NR(-12.0f, 12.0f, 0.01f), 0.0f, semitoneAttrs()));
    layout.add(std::make_unique<APF>("alter_mix",      "Formant Mix",      NR(0.0f, 1.0f, 0.001f),   1.0f, percentAttrs()));
    layout.add(std::make_unique<APB>("alter_solo_wet", "Formant Solo Wet", false));

    layout.add(std::make_unique<APF>("global_input",  "Input Gain (dB)",  NR(-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add(std::make_unique<APF>("global_output", "Output Gain (dB)", NR(-24.0f, 24.0f, 0.1f), 0.0f));

    layout.add(std::make_unique<APB>("tune_bypass",  "Pitch Engine Bypass", false));
    layout.add(std::make_unique<APB>("alter_bypass", "Formant Bypass",      false));
    layout.add(std::make_unique<juce::AudioParameterChoice>("ui_theme", "UI Theme",
        juce::StringArray { "NovaTone Studio", "Harmony Engine", "The Drum Master", "The Conductor", "AmpPire Valve" }, 0));

    // ---- Harmony tab ----
    {
        const int   defMode[] = { HarmonyEngine::chordAbove, HarmonyEngine::chordBelow, HarmonyEngine::octaveDown, HarmonyEngine::chordAbove2,
                                  HarmonyEngine::maj7Colour, HarmonyEngine::maj9Colour, HarmonyEngine::sus4Voice, HarmonyEngine::randomInKey };
        const float defPan[]  = { -0.45f, 0.45f, 0.0f, 0.7f, -0.7f, 0.3f, -0.25f, 0.55f };
        static_assert(HarmonyEngine::numVoices == 8, "update the voice defaults");
        const auto dbAttr = juce::AudioParameterFloatAttributes().withLabel("dB");
        for (int v = 0; v < HarmonyEngine::numVoices; ++v)
        {
            const juce::String id = "harm_v" + juce::String(v + 1) + "_", nm = "Harmony " + juce::String(v + 1) + " ";
            layout.add(std::make_unique<APB>(id + "on", nm + "On", false));
            layout.add(std::make_unique<juce::AudioParameterChoice>(id + "mode", nm + "Mode", HarmonyEngine::modeNames(), defMode[v]));
            layout.add(std::make_unique<APF>(id + "level", nm + "Level", NR(-36.0f, 6.0f, 0.1f), -6.0f, dbAttr));
            layout.add(std::make_unique<APF>(id + "pan", nm + "Pan", NR(-1.0f, 1.0f, 0.01f), defPan[v]));
            layout.add(std::make_unique<APF>(id + "formant", nm + "Formant", NR(-6.0f, 6.0f, 0.1f), 0.0f));
        }
        layout.add(std::make_unique<juce::AudioParameterInt>("harm_voices", "Harmony Voice Slots", 4, HarmonyEngine::numVoices, 4,
            juce::AudioParameterIntAttributes().withAutomatable(false)));
        layout.add(std::make_unique<APF>("harm_mix", "Harmony Mix", NR(-60.0f, 6.0f, 0.1f), 0.0f, dbAttr));
        layout.add(std::make_unique<APF>("harm_lead", "Lead Level", NR(-60.0f, 6.0f, 0.1f), 0.0f, dbAttr));
        layout.add(std::make_unique<APF>("harm_humanize", "Harmony Humanize", NR(0.0f, 1.0f, 0.01f), 0.4f));
        layout.add(std::make_unique<juce::AudioParameterChoice>("harm_source", "Harmony Chords From",
            juce::StringArray { "Sidechain (beat)", "Key & scale", "MIDI notes", "MIDI file" }, 0));
        // MIDI file source: shifts the file's chords against the song (beats; + = later) to line them up with the lead.
        layout.add(std::make_unique<APF>("harm_midiOffset", "Chord File Offset", NR(-64.0f, 64.0f, 0.001f), 0.0f,
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction([](float v, int) { return (v > 0.0f ? "+" : "") + juce::String(v, 2) + " beats"; })
                .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue(); })));
    }

    return layout;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NovaToneAudioProcessor(); }