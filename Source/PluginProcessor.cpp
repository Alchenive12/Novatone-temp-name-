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

void NovaToneAudioProcessor::parameterChanged(const juce::String& parameterID, float)
{
    if (parameterID == "tune_smooth" || parameterID == "tune_lookahead")
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
    harmony.prepare(sampleRate, samplesPerBlock);
    harmonyExporter.prepare(sampleRate, samplesPerBlock);
    choirRecorder.prepare(sampleRate, samplesPerBlock);
    choir.prepare(sampleRate, samplesPerBlock);
    harmonyDry.setSize((int)spec.numChannels, samplesPerBlock);
    alterEngine.prepare(spec);
    lastPublishedHopIndex = 0;

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
}

uint16_t NovaToneAudioProcessor::packScaleDegreeMask() const noexcept
{
    const int mode = currentMode();
    uint16_t mask = mode == customScale ? currentDegreeMask() : scaleMaskFor(currentRoot(), mode);
    if (mask == 0) mask = 0x0FFF;
    return mask;
}

void NovaToneAudioProcessor::pushParametersToEngines() noexcept
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

    tuneEngine.setFlexAmount(transition);
    const float humanizeAmt = juce::jlimit(0.0f, 1.0f, load(tuneHumanizeAmountParam, 0.50f));
    const bool  humanizeBtn = (humanizeAmt > 0.005f) || (load(tuneHumanizeButtonParam, 0.0f) > 0.5f);
    const bool  polyBtn      = load(tunePolyphonicParam, 0.0f) > 0.5f;
    const bool  midiCleanBtn = load(tuneMidiCleanParam,  0.0f) > 0.5f;
    tuneEngine.setHumanizeAmount(humanizeAmt);
    tuneEngine.setHumanizeEnabled(humanizeBtn);
    // Poly mode is clip-based now (Poly::ClipSession); the old real-time poly path stays off.
    tuneEngine.setPolyphonicEnabled(false);
    tuneEngine.setMidiCleanModeEnabled(midiCleanBtn);
    pitchQueue.globalHumanizeAmount.store(humanizeAmt, std::memory_order_relaxed);
    pitchQueue.globalHumanize.store(humanizeBtn,       std::memory_order_relaxed);
    pitchQueue.globalPolyphonic.store(polyBtn,         std::memory_order_relaxed);
    pitchQueue.globalMidiCleanMode.store(midiCleanBtn, std::memory_order_relaxed);
    tuneEngine.setCenterAmount(center);
    tuneEngine.setVariationAmount(variation);
    tuneEngine.setTransitionAmount(transition);
    tuneEngine.setSensitivityAmount(sensitivity);
    tuneEngine.setVibratoCents(vibrato);
    const int vocalRangeIdx = juce::roundToInt(load(tuneVocalRangeParam, 0.0f));
    tuneEngine.setVocalRange(vocalRangeIdx);
    pitchQueue.vocalRangeMode.store(vocalRangeIdx, std::memory_order_relaxed);

    pitchQueue.globalCenter.store(center, std::memory_order_relaxed);
    pitchQueue.globalVariation.store(variation, std::memory_order_relaxed);
    pitchQueue.globalTransition.store(transition, std::memory_order_relaxed);
    pitchQueue.globalSensitivity.store(sensitivity, std::memory_order_relaxed);
    pitchQueue.globalFormant.store(globalForm, std::memory_order_relaxed);
    pitchQueue.activeScaleMask.store(scaleMask, std::memory_order_relaxed);

    tuneEngine.setNaturalOnly(smoothRequested());
    const bool lookahead = lookaheadRequested();
    if (lookahead != tuneEngine.isLookaheadEnabled())
    {
        tuneEngine.setLookaheadEnabled(lookahead);
        bypass[tuneModule].setLatency(tuneEngine.getLatencySamples());
    }
    tuneEngine.setScale(scaleMask);

    float extraNoteFormant = 0.0f;
    const int playHop = pitchQueue.currentPlayheadHop.load(std::memory_order_relaxed);
    const int engineHop = (playHop > 0) ? playHop : (tuneEngine.getCurrentHopIndex() % PitchTraceQueue::maxTimelineHops);
    PitchTraceQueue::HopOverrideSlot hopSlot {};

    const auto& ov = pitchQueue.getLiveOverride();
    const bool ovActive = ov.active.load(std::memory_order_relaxed);
    if (pitchQueue.getHopOverride(engineHop, hopSlot))
    {
        extraNoteFormant = hopSlot.formantSemitones;
        tuneEngine.setLiveNoteOverride(
            true,
            hopSlot.targetMidi,
            hopSlot.noteCenter,
            hopSlot.noteVariation,
            hopSlot.gainDb,
            hopSlot.rampInRatio,
            hopSlot.sibilantDb,
            hopSlot.hasCustomPitch,
            hopSlot.customPitchMidi,
            hopSlot.fadeMultiplier,
            hopSlot.rampOutRatio);
    }
    else if (ovActive)
    {
        extraNoteFormant = ov.formantSemitones.load(std::memory_order_relaxed);
        tuneEngine.setLiveNoteOverride(
            true,
            ov.targetMidiFloat.load(std::memory_order_relaxed),
            ov.noteCenter.load(std::memory_order_relaxed),
            ov.noteVariation.load(std::memory_order_relaxed),
            ov.gainDb.load(std::memory_order_relaxed),
            ov.rampInRatio.load(std::memory_order_relaxed),
            ov.sibilantDb.load(std::memory_order_relaxed),
            ov.hasCustomPitch.load(std::memory_order_relaxed),
            ov.customPitchMidi.load(std::memory_order_relaxed),
            ov.fadeMultiplier.load(std::memory_order_relaxed),
            ov.rampOutRatio.load(std::memory_order_relaxed));
    }
    else
    {
        tuneEngine.setLiveNoteOverride(false, 60.0f, 1.0f, 1.0f, 0.0f, 0.18f);
    }

    for (int v = 0; v < PitchTraceQueue::maxPolyVoices; ++v)
    {
        const auto& pov = pitchQueue.getPolyVoiceOverride(v);
        const bool pAct = pov.active.load(std::memory_order_relaxed);
        tuneEngine.setPolyVoiceOverride(
            v,
            pAct,
            pov.detectedMidiRef.load(std::memory_order_relaxed),
            pov.targetMidiFloat.load(std::memory_order_relaxed),
            pov.noteCenter.load(std::memory_order_relaxed),
            pov.noteVariation.load(std::memory_order_relaxed),
            pov.gainDb.load(std::memory_order_relaxed));
    }

    const float totalPitchShift   = load(alterPitchParam, 0.0f);
    const float totalFormantShift = juce::jlimit(-12.0f, 12.0f, globalForm + extraNoteFormant);
    alterEngine.setPitch(totalPitchShift);
    alterEngine.setFormant(totalFormantShift);
    alterEngine.setMix(load(alterMixParam, 1.0f));
    alterEngine.setSoloWet(load(alterSoloWetParam, 0.0f) > 0.5f);

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

void NovaToneAudioProcessor::processBlock(juce::AudioBuffer<float>& fullBuffer, juce::MidiBuffer& midiMessages)
{
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

    pushParametersToEngines();

    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);

    inputGain.process(context);
    publishPeaks(block, inputPeak);

    // Host transport (sample position + playing state) for the poly clip
    std::optional<juce::int64> hostSample;
    bool hostPlaying = false;
    if (auto* ph = getPlayHead())
        if (const auto pos = ph->getPosition())
        {
            hostPlaying = pos->getIsPlaying();
            if (pos->getTimeInSamples().hasValue()) hostSample = *pos->getTimeInSamples();
        }
    const bool polyMode = tunePolyphonicParam != nullptr && tunePolyphonicParam->load(std::memory_order_relaxed) > 0.5f;
    if (polyMode)
        polyClip.capture(buffer, hostSample, hostPlaying);

    // Harmony: settings, sidechain chords, and a copy of the dry lead for the voices
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
        harmony.setVoice(v, vs);
    }
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
    const auto recState = choirRecorder.getState();
    const bool choirRecording = recState == HarmonyExporter::State::armed || recState == HarmonyExporter::State::recording;
    if (harmonyOn && ! (choir.enabled.load(std::memory_order_relaxed) && ! choirRecording) && harmonyDry.getNumSamples() >= buffer.getNumSamples())
    {
        harmony.setHumanize(loadP(harmHumanizeParam, 0.4f));
        harmony.setHarmonyGainDb(loadP(harmMixParam, 0.0f));
        harmony.setLeadGainDb(loadP(harmLeadParam, 0.0f));
        harmony.setUseSidechainChords(juce::roundToInt(loadP(harmSourceParam, 0.0f)) == 0);
        harmony.setMidiMode(juce::roundToInt(loadP(harmSourceParam, 0.0f)) == 2);
        harmony.setScaleMask(packScaleDegreeMask());
        harmony.setKeyRoot(currentRoot());
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

void NovaToneAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
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
        const auto choirFolder = tree.getProperty("choirFolder").toString();
        applyStateGuarded(tree);
        juce::MessageManager::callAsync([this, choirFolder, alive = juce::WeakReference<NovaToneAudioProcessor>(this)]
        {
            if (alive != nullptr && choirFolder.isNotEmpty()) choir.open(juce::File(choirFolder));
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
    layout.add(std::make_unique<APF>("tune_humanizeAmount", "Humanize",    NR(0.0f, 1.0f, 0.001f), 0.50f, percentAttrs()));

    layout.add(std::make_unique<APF>("tune_center",      "Center",      NR(0.0f, 1.0f, 0.001f), startKnobs.center,      percentAttrs()));
    layout.add(std::make_unique<APF>("tune_variation",   "Variation",   NR(0.0f, 2.0f, 0.001f), startKnobs.variation,   percentAttrs()));
    layout.add(std::make_unique<APF>("tune_transition",  "Transition",  NR(0.0f, 1.0f, 0.001f), startKnobs.transition,  percentAttrs()));
    layout.add(std::make_unique<APF>("tune_sensitivity", "Sensitivity", NR(0.0f, 1.0f, 0.001f), startKnobs.sensitivity, percentAttrs()));

    layout.add(std::make_unique<APB>("tune_snapScale",  "Snap to Scale", true));
    layout.add(std::make_unique<APB>("tune_autoScroll", "Auto-Scroll",   true));

    layout.add(std::make_unique<APB>("tune_humanize",   "Humanize Mode",   false));
    layout.add(std::make_unique<APB>("tune_polyphonic", "Polyphonic Mode", false));
    layout.add(std::make_unique<APB>("tune_midiClean",  "MIDI Clean Mode", false));
    layout.add(std::make_unique<APB>("tune_lookahead", "Lookahead", false));
    layout.add(std::make_unique<APB>("tune_smooth",    "Smooth",    false));
    layout.add(std::make_unique<APB>("tune_sync",      "Sync",      false));
    layout.add(std::make_unique<APF>("tune_character", "Soft/Hard", NR(0.0f, 1.0f, 0.001f), TuneMacro::defaultCharacter, percentAttrs()));
    layout.add(std::make_unique<APF>("tune_vibrato",   "Vibrato",   NR(0.0f, 100.0f, 1.0f), startKnobs.vibratoCents,
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
            juce::StringArray { "Sidechain (beat)", "Key & scale", "MIDI notes" }, 0));
    }

    return layout;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NovaToneAudioProcessor(); }