#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <array>
#include <string>
#include "MPMDetector.h"
#include "Quantizer.h"
#include "SmoothingFilter.h"
#include "PsolaShifter.h"
#include "VibratoTracker.h"
#include "SmoothPlanner.h"

class TuneEngine
{
public:
    TuneEngine() = default;

    struct PolyVoiceOverride
    {
        bool  active        = false;
        float detectedRef   = 60.0f;
        float targetMidi    = 60.0f;
        float noteCenter    = 1.0f;
        float noteVariation = 1.0f;
        float gainDb        = 0.0f;
    };

    void setPolyVoiceOverride(int voiceIdx, bool active, float detectedRef, float targetMidi,
                              float noteCenter, float noteVariation, float gainDb) noexcept
    {
        if (voiceIdx < 0 || voiceIdx >= maxPolyVoices)
            return;
        polyOverrides[(size_t)voiceIdx] = { active, detectedRef, targetMidi, noteCenter, noteVariation, gainDb };
    }

    void setPolyphonicEnabled(bool on) noexcept
    {
        if (on == polyphonicEnabled)
            return;
        polyphonicEnabled = on;
        numActivePolyVoices = 0;
        for (auto& vs : polyShifters)
            vs.reset();
    }
    bool isPolyphonicEnabled() const noexcept { return polyphonicEnabled; }

    /** Enables Instrument MIDI Clean Mode: applies local spectral median noise-floor subtraction,
        percussive transient/pick-noise rejection, 3-hop onset confirmation, and 4-hop sustain hold
        bridging for ultra-clean, noise-free instrument MIDI detection & transcription. */
    void setMidiCleanModeEnabled(bool on) noexcept
    {
        if (on == midiCleanModeEnabled)
            return;
        midiCleanModeEnabled = on;
        for (auto& ct : polyCleanTracks)
            ct = PolyCleanTrack{};
    }
    bool isMidiCleanModeEnabled() const noexcept { return midiCleanModeEnabled; }

    int   getNumPolyVoices() const noexcept { return numActivePolyVoices; }
    float getPolyRawMidi(int i) const noexcept { return (i >= 0 && i < maxPolyVoices) ? polyRawMidi[(size_t)i] : 0.0f; }
    float getPolyCorrectedMidi(int i) const noexcept { return (i >= 0 && i < maxPolyVoices) ? polyDesiredMidi[(size_t)i] : 0.0f; }
    float getPolyVoiceRms(int i) const noexcept { return (i >= 0 && i < maxPolyVoices) ? polyVoiceRms[(size_t)i] : 0.0f; }

    /** Classifies the currently active polyphonic voices (bass root + upper triad/extensions)
        into a standard chord symbol (e.g. "Em", "Em(add9)", "G(add9)", "Asus2", "Cmaj7", "D", "Bm7"). */
    std::string getDetectedChordName() const
    {
        if (numActivePolyVoices <= 0 || polyRawMidi[0] <= 0.0f)
            return "--";
        static constexpr const char* pcNames[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        const int bassMidi = static_cast<int>(std::lround(polyRawMidi[0]));
        const int rootPc   = ((bassMidi % 12) + 12) % 12;
        uint16_t relMask   = 0;
        for (int v = 0; v < numActivePolyVoices; ++v)
        {
            if (polyRawMidi[(size_t)v] > 0.0f)
            {
                const int m  = static_cast<int>(std::lround(polyRawMidi[(size_t)v]));
                const int pc = ((m % 12) + 12) % 12;
                const int iv = (pc - rootPc + 12) % 12;
                relMask |= static_cast<uint16_t>(1u << iv);
            }
        }
        const bool hasMin3 = (relMask & (1u << 3)) != 0;
        const bool hasMaj3 = (relMask & (1u << 4)) != 0;
        const bool has2    = (relMask & (1u << 2)) != 0;
        const bool has4    = (relMask & (1u << 5)) != 0;
        const bool hasMin7 = (relMask & (1u << 10)) != 0;
        const bool hasMaj7 = (relMask & (1u << 11)) != 0;

        std::string sym(pcNames[rootPc]);
        if (hasMin3)
        {
            if (hasMin7 && has2)      sym += "m9";
            else if (hasMin7)         sym += "m7";
            else if (has2)            sym += "m(add9)";
            else                      sym += "m";
        }
        else if (hasMaj3)
        {
            if (hasMaj7)              sym += "maj7";
            else if (hasMin7)         sym += "7";
            else if (has2)            sym += "(add9)";
        }
        else if (has2)
        {
            sym += "sus2";
        }
        else if (has4)
        {
            sym += "sus4";
        }
        else if (numActivePolyVoices >= 2)
        {
            sym += "5";
        }
        return sym;
    }

    void setLiveNoteOverride(bool active, float targetMidi, float noteCenter,
                             float noteVariation, float gainDb, float rampIn,
                             float sibilantDb = 0.0f, bool hasCustomPitch = false,
                             float customPitchMidi = 60.0f, float fadeMultiplier = 1.0f,
                             float rampOut = 0.18f) noexcept
    {
        overrideActive          = active;
        overrideTargetMidi      = targetMidi;
        overrideCenter          = noteCenter;
        overrideVariation       = noteVariation;
        overrideGainDb          = gainDb;
        overrideRampIn          = rampIn;
        overrideRampOut         = rampOut;
        overrideSibilantDb      = sibilantDb;
        overrideHasCustomPitch  = hasCustomPitch;
        overrideCustomPitchMidi = customPitchMidi;
        overrideFadeMultiplier  = juce::jlimit(0.0f, 1.0f, fadeMultiplier);
    }

    /** Real-Time MIDI Target Note Override (Waves Tune Real-Time / Auto-Tune Pro 11 style):
        When `midiNote >= 0`, forces the target vocal pitch to the held MIDI note (with
        automatic octave-proximity matching if the singer is > 9 semitones away). */
    void setMidiTargetNote(int midiNote, uint16_t midiMask = 0) noexcept
    {
        midiTargetNote = midiNote;
        midiTargetMask = midiMask;
    }
    int getMidiTargetNote() const noexcept { return midiTargetNote; }

    /** Controls how much slow intra-note pitch drift (< 3 Hz sag/tilt) is preserved
        independently of fast 4-12 Hz vibrato [0 = full sag correction, 1 = keep drift]. */
    /** Fix #5: Vocal Range Lock (Auto-Tune Pro 11 / Waves Tune Real-Time Input Type):
        0 = Full (65..1600 Hz), 1 = Soprano (220..1250 Hz), 2 = Alto (165..920 Hz),
        3 = Tenor (115..620 Hz), 4 = Bass/Baritone (70..420 Hz). */
    void setVocalRange(int rangeMode) noexcept
    {
        vocalRangeMode = juce::jlimit(0, 4, rangeMode);
        switch (vocalRangeMode)
        {
            case 1: activeMinVoiceHz = 220.0f; activeMaxVoiceHz = 1250.0f; break; // Soprano
            case 2: activeMinVoiceHz = 165.0f; activeMaxVoiceHz = 920.0f;  break; // Alto / Mezzo
            case 3: activeMinVoiceHz = 115.0f; activeMaxVoiceHz = 620.0f;  break; // Tenor
            case 4: activeMinVoiceHz = 70.0f;  activeMaxVoiceHz = 420.0f;  break; // Bass / Baritone
            default: activeMinVoiceHz = 65.0f; activeMaxVoiceHz = 1600.0f; break; // Full Range
        }
    }
    int   getVocalRange() const noexcept { return vocalRangeMode; }
    float getMinVoiceHz() const noexcept { return activeMinVoiceHz; }
    float getMaxVoiceHz() const noexcept { return activeMaxVoiceHz; }

    void setDriftAmount(float v) noexcept { driftAmount = juce::jlimit(0.0f, 1.0f, v); }
    float getDriftAmount() const noexcept { return driftAmount; }

    void prepare(const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        windowSize = juce::nextPowerOfTwo(static_cast<int> (std::ceil(2.0 * sampleRate / minDetectHz)));
        hopSize = juce::jmax(128, windowSize / 8);
        hopSeconds = static_cast<float> (hopSize / sampleRate);

        detector.prepare(sampleRate, windowSize);
        for (size_t i = 0; i < fineDetectors.size(); ++i)
        {
            fineDetectors[i].prepare(sampleRate, windowSize >> (i + 1)); // W/2, W/4
            fineDetectors[i].setThresholdRatio(trackerParams.thresholdRatio);
            fineDetectors[i].setClarityFloor(0.0f);
        }
        detector.setThresholdRatio(trackerParams.thresholdRatio);
        detector.setClarityFloor(0.0f);
        quantizer.prepare(sampleRate, hopSize);
        smoother.prepare(sampleRate, hopSize);

        const int correctionLag = windowSize / 2 + hopSize + alignTrimSamples;
        shifter.prepare(spec, correctionLag);
        shifter.setHumanizeMode(humanizeEnabled);

        planner.prepare(hopSize, sampleRate);
        smoothDelaySamples = planner.getLookaheadSamples();
        smoothRingSize = juce::nextPowerOfTwo(smoothDelaySamples + static_cast<int> (subBlockSize) + 1);
        smoothRing.setSize(juce::jmax(1, static_cast<int> (spec.numChannels)), smoothRingSize);

        // Prepare Latency-Aligned Sub-Band Sibilant & Breath Crossover Ring (3.8 kHz TPT split):
        const int maxTotalLatency = shifter.getLatencySamples() + smoothDelaySamples + static_cast<int>(subBlockSize) + 64;
        sibilantRingSize = juce::nextPowerOfTwo(juce::jmax(256, maxTotalLatency));
        sibilantRingMask = sibilantRingSize - 1;
        sibilantDryRing.setSize(juce::jmax(1, static_cast<int>(spec.numChannels)), sibilantRingSize);
        const float gCross = std::tan(juce::MathConstants<float>::pi * juce::jmin(3800.0f, static_cast<float>(sampleRate * 0.42)) / static_cast<float>(sampleRate));
        sibilantLpG = gCross / (1.0f + gCross);

        analysisBuffer.setSize(1, windowSize);
        vibratoCentreAlpha = 1.0f - std::exp(-hopSeconds / (ablateCentreTau > 0.0f ? ablateCentreTau : vibratoCentreTauSeconds));
        slowDriftAlpha     = 1.0f - std::exp(-hopSeconds / 0.165f);
        vibratoTracker.prepare(hopSeconds);

        const int alignDelay = juce::jmax(0, shifter.getAnalysisLatencySamples() - correctionLag + 128);
        correctionHistory.assign(static_cast<size_t> (alignDelay) + 1, 0.0f);

        // 2.0 ms continuous anti-click filter (widened to 2.2 ms when Humanize is active)
        fastInstantSmoothAlpha  = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.0020));
        humanInstantSmoothAlpha = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.0022));
        instantSmoothAlpha      = humanizeEnabled ? humanInstantSmoothAlpha : fastInstantSmoothAlpha;

        // Hop-rate macro parameter smoothing (20 ms) so moving knobs or toggling Humanize never steps pitch
        macroHopSmoothAlpha = 1.0f - std::exp(-hopSeconds / 0.020f);

        noteGainSmoothed.reset(sampleRate, 0.015);
        noteGainSmoothed.setCurrentAndTargetValue(1.0f);
        sibilantGainSmoothed.reset(sampleRate, 0.012);
        sibilantGainSmoothed.setCurrentAndTargetValue(1.0f);
        sibilantBlendSmoothed.reset(sampleRate, 0.006);
        sibilantBlendSmoothed.setCurrentAndTargetValue(0.0f);

        // Prepare Polyphonic (Melodyne-style Chord/Choir/String/Guitar) 8192-sample rolling window + 16384-pt FFT:
        polyWindowSize = 4 * windowSize;
        polyHistoryRing.assign(static_cast<size_t>(polyWindowSize), 0.0f);
        polyHistoryWritePos = 0;
        polyFftOrder = 0;
        polyFftSize  = 1;
        while (polyFftSize < 2 * polyWindowSize)
        {
            polyFftSize <<= 1;
            ++polyFftOrder;
        }
        polyFft.emplace(polyFftOrder);
        polyFftBuffer.assign(static_cast<size_t>(2 * polyFftSize), 0.0f);
        polyMagSpectrum.assign(static_cast<size_t>(polyFftSize / 2 + 1), 0.0f);
        polyHannWindow.assign(static_cast<size_t>(polyWindowSize), 0.0f);
        for (int i = 0; i < polyWindowSize; ++i)
            polyHannWindow[(size_t)i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float)i / (float)juce::jmax(1, polyWindowSize - 1));

        for (auto& vs : polyShifters)
            vs.prepare(sampleRate, static_cast<int>(spec.numChannels));

        applySensitivityToTracker();
        applyFlexToSmoother();
        reset();
    }

    void reset()
    {
        detector.reset();
        for (auto& d : fineDetectors)
            d.reset();
        quantizer.reset();
        smoother.reset();
        shifter.reset();
        shifter.setHumanizeMode(humanizeEnabled);

        analysisBuffer.clear();
        std::fill(polyHistoryRing.begin(), polyHistoryRing.end(), 0.0f);
        polyHistoryWritePos = 0;
        samplesCollected = 0;
        samplesSinceLastHop = 0;

        std::fill(correctionHistory.begin(), correctionHistory.end(), 0.0f);
        historyWritePos = 0;

        lastUncorrectedHz = 0.0f;
        lastCorrectedHz = 0.0f;
        lastRms = 0.0f;
        lastClarity = 0.0f;
        lastZcr = 0.0f;
        currentlyVoiced = false;
        currentlySibilant = false;
        unvoicedHopCount = 0;
        hopCounter = 0;

        inNote = false;
        jumpPending = false;
        pendingJumpMidi = 0.0f;
        prevHopDetectedMidi = 0.0f;
        lastDetectedMidi = 0.0f;
        prevHopDesiredMidi = 0.0f;
        lastDesiredMidi = 0.0f;
        smoothedDesiredCentreMidi = 60.0f;
        noteStartCentreMidi = 60.0f;
        centreSmootherInitialized = false;
        vibratoCentreMidi = 0.0f;
        perceptualNoteAnchorMidi = 60.0f;
        perceptualAnchorWeight = 0.0f;
        slowDriftMidi = 60.0f;
        previousNoteTargetMidi = 60.0f;
        lastTargetMidi = 60.0f;
        previousSungAnchorMidi = 60.0f;
        hopsSincePhraseOnset = 0;
        hopsSinceNoteChange = 0;
        heldNote = false;
        heldGain = 0.0f;
        smoothedInstantCorrection = 0.0f;
        smoothedCenterAmount = centerAmount;
        smoothedVariationAmount = variationAmount;
        smoothedHumanizeGate = getTargetHumanizeDrive();
        humanizeDriftPhase1 = 0.0f;
        humanizeDriftPhase2 = 1.7f;
        humanizeVibPhase    = 0.0f;
        vibratoTracker.reset();

        planner.reset();
        smoothRing.clear();
        smoothWritePos = 0;
        sibilantDryRing.clear();
        sibilantWritePos = 0;
        sibilantWetLpState.fill(0.0f);
        sibilantDryLpState.fill(0.0f);
        inputCounter = 0;
        numActivePolyVoices = 0;
        prevNumPolyVoices = 0;
        prevPolyFrameRms = 0.0f;
        prevPolyRawMidi.fill(0.0f);
        polyRawMidi.fill(0.0f);
        polyDesiredMidi.fill(0.0f);
        polyVoiceRms.fill(0.0f);
        polySmoothedCentre.fill(0.0f);
        polyVoiceGainLin.fill(1.0f);
        for (auto& vs : polyShifters)
            vs.reset();
        noteGainSmoothed.setCurrentAndTargetValue(1.0f);
        sibilantGainSmoothed.setCurrentAndTargetValue(1.0f);
        sibilantBlendSmoothed.setCurrentAndTargetValue(0.0f);
    }

    void process(const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto&& block = context.getOutputBlock();
        const size_t numCh = block.getNumChannels();
        const size_t numSmp = block.getNumSamples();

        if (numSmp == 0 || numCh == 0)
            return;

        const float channelScale = 1.0f / static_cast<float> (numCh);

        const size_t historySize = correctionHistory.size();
        if (historySize == 0)
            return; // not prepared

        if (smoothEnabled)
        {
            const float hDrive = getTargetHumanizeDrive();
            const float effSpeed = transitionAmount * juce::jlimit(0.45f, 1.0f, 1.0f - 0.32f * hDrive);
            const float effHumanize = naturalOnly
                ? 1.0f
                : juce::jlimit(0.0f, 1.0f, 0.5f * hDrive + (1.0f - 0.5f * juce::jmin(1.0f, hDrive)) * ((1.0f - centerAmount) * 0.5f + 0.5f * juce::jlimit(0.0f, 1.0f, variationAmount)));
            planner.setKnobs(effSpeed, effHumanize, vibratoDepthSemitones);
        }

        float targetNoteGainLin = 1.0f;
        float targetSibGainLin  = 1.0f;
        if (overrideActive)
        {
            targetNoteGainLin = juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 12.0f, overrideGainDb)) * overrideFadeMultiplier;
            targetSibGainLin  = juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 12.0f, overrideSibilantDb));
        }
        noteGainSmoothed.setTargetValue(targetNoteGainLin);
        sibilantGainSmoothed.setTargetValue(targetSibGainLin);

        const int dryRingChans = juce::jmin(static_cast<int>(numCh), sibilantDryRing.getNumChannels());
        const int shifterLatency = shifter.getLatencySamples();

        for (size_t start = 0; start < numSmp; start += subBlockSize)
        {
            const size_t len = juce::jmin(subBlockSize, numSmp - start);
            auto sub = block.getSubBlock(start, len);

            for (size_t i = 0; i < len; ++i)
            {
                float monoSample = 0.0f;
                for (size_t ch = 0; ch < numCh; ++ch)
                    monoSample += sub.getSample(static_cast<int> (ch), static_cast<int> (i));

                const float monoScaled = monoSample * channelScale;
                analysisBuffer.setSample(0, samplesCollected, monoScaled);
                if (polyWindowSize > 0)
                {
                    polyHistoryRing[(size_t)polyHistoryWritePos] = monoScaled;
                    polyHistoryWritePos = (polyHistoryWritePos + 1) & (polyWindowSize - 1);
                }
                ++samplesCollected;
                ++samplesSinceLastHop;
                ++inputCounter;

                if (samplesCollected >= windowSize)
                {
                    analyzeAndCalculateCorrection();

                    for (int j = 0; j < windowSize - hopSize; ++j)
                        analysisBuffer.setSample(0, j, analysisBuffer.getSample(0, j + hopSize));

                    samplesCollected = windowSize - hopSize;
                    samplesSinceLastHop = 0;
                }

                // Always advance smoother on every sample so its state never freezes during voiced frames
                const float fallbackSmoothed = smoother.processSample();

                float rawTargetCorrection = fallbackSmoothed;
                if ((currentlyVoiced || inNote) && prevHopDetectedMidi > 0.0f && lastDetectedMidi > 0.0f)
                {
                    const float alpha = juce::jlimit(0.0f, 1.0f,
                        static_cast<float>(samplesSinceLastHop) / static_cast<float>(juce::jmax(1, hopSize)));
                    const float interpDetected = prevHopDetectedMidi + alpha * (lastDetectedMidi - prevHopDetectedMidi);
                    const float interpDesired  = prevHopDesiredMidi  + alpha * (lastDesiredMidi  - prevHopDesiredMidi);
                    rawTargetCorrection = juce::jlimit(-12.0f, 12.0f, interpDesired - interpDetected);
                }
                if (currentlySibilant)
                    rawTargetCorrection = 0.0f;

                // Continuous anti-click follower
                smoothedInstantCorrection += instantSmoothAlpha * (rawTargetCorrection - smoothedInstantCorrection);

                correctionHistory[historyWritePos] = smoothedInstantCorrection;
                historyWritePos = (historyWritePos + 1) % historySize;
            }

            float correction = correctionHistory[historyWritePos];
            if (smoothEnabled)
            {
                delaySubBlock(sub);
                const double grainTime = static_cast<double> (inputCounter - smoothDelaySamples - shifter.getAnalysisLatencySamples() - (hopSize / 2) + smoothTrimSamples);
                const float delayedHintHz = planner.pitchHintAt(static_cast<double> (inputCounter - smoothDelaySamples));
                correction = (delayedHintHz <= 0.0f) ? 0.0f : planner.correctionAt(grainTime);
                shifter.setPeriodHint(delayedHintHz);
            }
            else if (currentlySibilant)
            {
                correction = 0.0f;
            }

            // Record pre-shifter signal into latency-compensation ring so the > 3.8 kHz sibilant/breath
            // band can be blended or trimmed in exact phase alignment after TD-PSOLA:
            if (sibilantRingSize > 0)
            {
                for (int ch = 0; ch < dryRingChans; ++ch)
                {
                    auto* ringPtr = sibilantDryRing.getWritePointer(ch);
                    for (size_t i = 0; i < len; ++i)
                        ringPtr[(sibilantWritePos + static_cast<int>(i)) & sibilantRingMask] = sub.getSample(ch, static_cast<int>(i));
                }
            }

            // In Polyphonic Mode, keep the full-band monophonic TD-PSOLA shifter at 0.0 st (neutral
            // latency-matched pass-through) so multi-string chords never warble when the loudest string changes,
            // and let `processPolyphonicSubBlock` shift each individual string/voice band cleanly!
            const float monoPsolaShift = polyphonicEnabled ? 0.0f : correction;
            appliedCorrection = correction;
            shifter.setPolyphonicPassthrough(polyphonicEnabled);
            shifter.setPitchSemitones(monoPsolaShift);

            juce::dsp::ProcessContextReplacing<float> subContext(sub);
            shifter.process(subContext);

            if (polyphonicEnabled)
            {
                processPolyphonicSubBlock(sub, currentlySibilant ? 0.0f : 0.0f);
            }

            // Upgrade #1: Sub-Band Sibilant & Breath Crossover Protection (> 3.8 kHz) + Per-Note Sibilant Balance:
            // During voiced fricatives ('z', 'v', breathy transitions) or when per-note sibilantDb != 0 dB,
            // keeps < 3.8 kHz fundamental & vowel formants 100% pitch-corrected while preserving unshifted
            // high-frequency consonant clarity and applying per-note sibilant gain trim.
            const float targetSibBlend = currentlySibilant
                ? 1.0f
                : ((currentlyVoiced && lastZcr > 0.085f && lastClarity < 0.78f)
                    ? juce::jlimit(0.0f, 0.85f, (lastZcr - 0.085f) / 0.065f)
                    : 0.0f);
            sibilantBlendSmoothed.setTargetValue(targetSibBlend);

            if (sibilantRingSize > 0)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    const float sibBlend = sibilantBlendSmoothed.getNextValue();
                    const float sibGain  = sibilantGainSmoothed.getNextValue();
                    const float noteGain = noteGainSmoothed.getNextValue();
                    const int readIdx = (sibilantWritePos + static_cast<int>(i) - shifterLatency) & sibilantRingMask;

                    for (int ch = 0; ch < dryRingChans; ++ch)
                    {
                        const float yWet = sub.getSample(ch, static_cast<int>(i));
                        const float xDry = sibilantDryRing.getSample(ch, readIdx);

                        // 1st-order TPT complementary 3.8 kHz crossover (always advances state so filters stay warm)
                        float& lpW = sibilantWetLpState[(size_t)juce::jmin(ch, 1)];
                        float& lpD = sibilantDryLpState[(size_t)juce::jmin(ch, 1)];
                        lpW += sibilantLpG * (yWet - lpW);
                        lpD += sibilantLpG * (xDry - lpD);

                        const float hpWet = yWet - lpW;
                        const float hpDry = xDry - lpD;

                        // Blend unshifted high band on fricatives/breaths and scale by per-note Sibilant Balance:
                        const float shapedHigh = (hpWet + sibBlend * (hpDry - hpWet)) * sibGain;
                        const float outSample  = (lpW + shapedHigh) * noteGain;
                        sub.setSample(ch, static_cast<int>(i), outSample);
                    }
                }
                sibilantWritePos = (sibilantWritePos + static_cast<int>(len)) & sibilantRingMask;
            }
            else if (noteGainSmoothed.isSmoothing() || std::abs(noteGainSmoothed.getTargetValue() - 1.0f) > 1.0e-3f)
            {
                for (size_t i = 0; i < len; ++i)
                {
                    const float g = noteGainSmoothed.getNextValue();
                    for (size_t ch = 0; ch < numCh; ++ch)
                        sub.setSample(static_cast<int>(ch), static_cast<int>(i), sub.getSample(static_cast<int>(ch), static_cast<int>(i)) * g);
                }
            }
        }
    }

    int getLatencySamples() const noexcept { return getLatencySamplesFor(smoothEnabled); }

    int getLatencySamplesFor(bool lookahead) const noexcept
    {
        return shifter.getLatencySamples() + (lookahead ? smoothDelaySamples : 0);
    }

    void setLookaheadEnabled(bool on) noexcept
    {
        if (on == smoothEnabled)
            return;
        smoothEnabled = on;
        reset();
    }
    bool isLookaheadEnabled() const noexcept { return smoothEnabled; }

    void setNaturalOnly(bool on) noexcept { naturalOnly = on; }

    /** Continuous Humanize Drive:
        Slider 0%   (humanizeAmount = 0.0) -> drive = 0.0 (Off)
        Slider 50%  (humanizeAmount = 0.5) -> drive = 1.0 (Standard Humanize, matches previous ON button 1:1)
        Slider 100% (humanizeAmount = 1.0) -> drive = 2.0 (Extreme Humanize) */
    float getTargetHumanizeDrive() const noexcept
    {
        if (humanizeAmount > 0.001f)
            return juce::jlimit(0.0f, 2.0f, humanizeAmount * 2.0f);
        return humanizeEnabled ? 1.0f : 0.0f;
    }

    void setHumanizeEnabled(bool on) noexcept
    {
        if (on == humanizeEnabled)
            return;
        humanizeEnabled = on;
        const bool active = (getTargetHumanizeDrive() > 0.02f);
        instantSmoothAlpha = active ? humanInstantSmoothAlpha : fastInstantSmoothAlpha;
        shifter.setHumanizeMode(active);
        applySensitivityToTracker();
        applyFlexToSmoother();
    }
    bool isHumanizeEnabled() const noexcept { return getTargetHumanizeDrive() > 0.02f; }

    //==============================================================================
    // Classic + NewTone Global Controls
    //==============================================================================
    void setFlexAmount(float v)
    {
        flexAmount = juce::jlimit(0.0f, 1.0f, v);
        transitionAmount = flexAmount;
        applyFlexToSmoother();
    }

    void setHumanizeAmount(float v) noexcept
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        if (std::abs(clamped - humanizeAmount) < 1.0e-4f)
            return;
        humanizeAmount = clamped;
        humanizeEnabled = (humanizeAmount > 0.005f);
        const bool active = (getTargetHumanizeDrive() > 0.02f);
        instantSmoothAlpha = active ? humanInstantSmoothAlpha : fastInstantSmoothAlpha;
        shifter.setHumanizeMode(active);
        applyFlexToSmoother();
    }
    float getHumanizeAmount() const noexcept { return humanizeAmount; }

    void setCenterAmount(float v) noexcept
    {
        centerAmount = juce::jlimit(0.0f, 1.0f, v);
    }
    float getCenterAmount() const noexcept { return centerAmount; }

    void setVariationAmount(float v) noexcept
    {
        variationAmount = juce::jlimit(0.0f, 2.0f, v);
    }
    float getVariationAmount() const noexcept { return variationAmount; }

    void setTransitionAmount(float v) noexcept
    {
        transitionAmount = juce::jlimit(0.0f, 1.0f, v);
        flexAmount = transitionAmount;
        applyFlexToSmoother();
    }
    float getTransitionAmount() const noexcept { return transitionAmount; }

    void setSensitivityAmount(float v) noexcept
    {
        const float clamped = juce::jlimit(0.0f, 1.0f, v);
        if (std::abs(clamped - sensitivityAmount) < 1.0e-4f)
            return;
        sensitivityAmount = clamped;
        applySensitivityToTracker();
    }
    float getSensitivityAmount() const noexcept { return sensitivityAmount; }

    void setVibratoCents(float cents) { vibratoDepthSemitones = juce::jlimit(0.0f, 100.0f, cents) / 100.0f; }
    void setScale(uint16_t mask) noexcept
    {
        activeScaleMask = (mask != 0) ? mask : 0x0FFF;
        quantizer.setScale(activeScaleMask);
    }

    float getUncorrectedPitchHz() const noexcept { return lastUncorrectedHz; }
    float getCorrectedPitchHz()   const noexcept { return lastCorrectedHz; }
    float getLastRms()            const noexcept { return lastRms; }
    float getLastClarity()        const noexcept { return lastClarity; }
    bool  isVoiced()              const noexcept { return currentlyVoiced; }
    bool  isSibilant()            const noexcept { return currentlySibilant; }
    int   getCurrentHopIndex()    const noexcept { return hopCounter; }
    float getAppliedCorrectionSemitones() const noexcept { return appliedCorrection; }

private:
    void applySensitivityToTracker() noexcept
    {
        trackerParams.openClarity = juce::jmap(sensitivityAmount, 0.0f, 1.0f, 0.64f, 0.48f);
        trackerParams.holdClarity = juce::jmap(sensitivityAmount, 0.0f, 1.0f, 0.46f, 0.32f);
        energyGateRms = juce::Decibels::decibelsToGain(juce::jmap(sensitivityAmount, 0.0f, 1.0f, -42.0f, -56.0f));

        quantizer.setHysteresisMargin(juce::jmap(sensitivityAmount, 0.0f, 1.0f, 18.0f, 3.0f));
        quantizer.setHoldTime(juce::jmap(sensitivityAmount, 0.0f, 1.0f, 0.042f, 0.010f));
    }

    void applyFlexToSmoother() noexcept
    {
        const float hDrive   = getTargetHumanizeDrive(); // 0 = Off, 1.0 = 50% (Standard), 2.0 = 100% (Extreme)
        const float hGate    = juce::jlimit(0.0f, 1.0f, hDrive);
        const float hExtreme = juce::jlimit(0.0f, 1.0f, hDrive - 1.0f);

        const float humanizedTrans = juce::jmin(0.84f - 0.16f * hExtreme,
                                                (0.25f - 0.07f * hExtreme) + (0.60f - 0.15f * hExtreme) * transitionAmount);
        const float effTrans = transitionAmount + hGate * (humanizedTrans - transitionAmount);

        const float s = 1.0f - effTrans;
        const double baseMinTau = 0.003 + 0.003 * static_cast<double>(hGate) + 0.004 * static_cast<double>(hExtreme);
        const double timeSecs   = baseMinTau + 0.095 * static_cast<double>(s * s);
        smoother.setTimeConstant(timeSecs);

        const float maxSlew = juce::jmap(hGate, 260.0f, 115.0f - 35.0f * hExtreme);
        smoother.setMaxSlewRate(juce::jmap(effTrans, 36.0f - 8.0f * hExtreme, maxSlew));

        const float tauCentre = (0.001f + 0.0035f * hGate + 0.0045f * hExtreme) + 0.075f * (s * s);
        centreTransitionAlpha = (hDrive <= 0.02f && effTrans >= 0.96f)
            ? 1.0f
            : (1.0f - std::exp(-hopSeconds / juce::jmax(0.001f, tauCentre)));
    }

    void analyzeAndCalculateCorrection() noexcept
    {
        ++hopCounter;
        smoothedCenterAmount    += macroHopSmoothAlpha * (centerAmount    - smoothedCenterAmount);
        smoothedVariationAmount += macroHopSmoothAlpha * (variationAmount - smoothedVariationAmount);
        const float targetHumGate = getTargetHumanizeDrive();
        smoothedHumanizeGate    += macroHopSmoothAlpha * (targetHumGate   - smoothedHumanizeGate);

        analyzeHop();
        if (smoothEnabled)
        {
            const bool v = currentlyVoiced && lastUncorrectedHz > 0.0f;
            const float target = lastCorrectedHz > 0.0f ? Quantizer::hzToMidi(lastCorrectedHz) : 0.0f;
            planner.push(inputCounter - windowSize / 2, v, v ? Quantizer::hzToMidi(lastUncorrectedHz) : 0.0f,
                         target, v ? lastUncorrectedHz : 0.0f);
        }
    }

    void delaySubBlock(juce::dsp::AudioBlock<float>& sub) noexcept
    {
        const int len = static_cast<int> (sub.getNumSamples());
        const int chans = juce::jmin(static_cast<int> (sub.getNumChannels()), smoothRing.getNumChannels());
        const int mask = smoothRingSize - 1;
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* ring = smoothRing.getWritePointer(ch);
            for (int i = 0; i < len; ++i)
            {
                const int w = (smoothWritePos + i) & mask;
                ring[w] = sub.getSample(ch, i);
                sub.setSample(ch, i, ring[(w - smoothDelaySamples) & mask]);
            }
        }
        smoothWritePos = (smoothWritePos + len) & mask;
    }

    void analyzeHop() noexcept
    {
        float rms = 0.0f;
        int zeroCrossings = 0;
        auto* readPtr = analysisBuffer.getReadPointer(0);
        for (int i = 0; i < windowSize; ++i)
        {
            const float s = readPtr[i];
            rms += s * s;
            if (i > 0 && ((s >= 0.0f) != (readPtr[i - 1] >= 0.0f)))
                ++zeroCrossings;
        }
        rms = std::sqrt(rms / static_cast<float> (windowSize));
        lastRms = rms;
        const float zcr = static_cast<float>(zeroCrossings) / static_cast<float>(windowSize);
        lastZcr = zcr;

        const bool hasEnergy = (rms > energyGateRms);

        const MPMResult rawMpm = hasEnergy ? detector.processFrame(readPtr) : MPMResult{};
        lastClarity = rawMpm.clarity;

        // Pre-detect sibilants/fricatives ('s', 'z', 'sh', 't') before pitch tracking so sibilant resonances
        // never masquerade as 400-650 Hz voiced notes or corrupt lastDetectedMidi.
        const bool fricativeFrame = hasEnergy && (zcr > 0.14f || (zcr > 0.09f && rawMpm.pitchHz > 370.0f && rawMpm.clarity < 0.85f));

        MPMResult mpmRes = (hasEnergy && !fricativeFrame) ? trackPitch(rawMpm) : MPMResult{};
        const bool voicedNow = mpmRes.isVoiced;
        if (voicedNow && useFineWindow)
            refinePitch(mpmRes, readPtr);

        currentlySibilant = (fricativeFrame || (!voicedNow && hasEnergy && zcr > 0.12f));

        if (! voicedNow)
        {
            if (polyphonicEnabled && hasEnergy && !currentlySibilant)
            {
                analyzePolyphonicHop(readPtr, rms);
                if (numActivePolyVoices > 0)
                {
                    currentlyVoiced = true;
                    inNote = true;
                    unvoicedHopCount = 0;
                    lastUncorrectedHz = Quantizer::midiToHz(polyRawMidi[0]);
                    lastCorrectedHz   = Quantizer::midiToHz(polyDesiredMidi[0]);
                    const float primaryCorr = juce::jlimit(-12.0f, 12.0f, polyDesiredMidi[0] - polyRawMidi[0]);
                    prevHopDetectedMidi = lastDetectedMidi > 0.0f ? lastDetectedMidi : polyRawMidi[0];
                    lastDetectedMidi    = polyRawMidi[0];
                    prevHopDesiredMidi  = lastDesiredMidi  > 0.0f ? lastDesiredMidi  : polyDesiredMidi[0];
                    lastDesiredMidi     = polyDesiredMidi[0];
                    shifter.setPeriodHint(lastUncorrectedHz);
                    smoother.setTargetSemitones(primaryCorr);
                    return;
                }
            }
            numActivePolyVoices = 0;
            shifter.setPeriodHint(0.0f);
            handleUnvoicedHop(hasEnergy);
            return;
        }

        float detectedMidi = Quantizer::hzToMidi(mpmRes.pitchHz);

        // Harmonic Octave Guard: if an active Piano Roll note override (or Tenor/Bass range lock)
        // shows `detectedMidi` is ~1 octave (+12 st) above the fundamental target, fold to f0
        // so the engine never mistakes a 2nd harmonic for an 11-semitone pitch error!
        if (overrideActive && (detectedMidi - overrideTargetMidi) > 7.5f && (detectedMidi - overrideTargetMidi) < 16.5f)
        {
            detectedMidi   -= 12.0f;
            mpmRes.pitchHz *= 0.5f;
        }
        else if (vocalRangeMode == 3 && inNote && detectedMidi > 59.5f && (detectedMidi - vibratoCentreMidi) > 6.5f)
        {
            detectedMidi   -= 12.0f;
            mpmRes.pitchHz *= 0.5f;
        }

        if (inNote && std::abs(detectedMidi - lastDetectedMidi) > maxJumpSemitones
            && (! jumpPending || std::abs(detectedMidi - pendingJumpMidi) > pendingJumpToleranceSemitones))
        {
            jumpPending = true;
            pendingJumpMidi = detectedMidi;
            prevHopDetectedMidi = lastDetectedMidi;
            prevHopDesiredMidi  = lastDesiredMidi;
            return;
        }
        jumpPending = false;
        shifter.setPeriodHint(mpmRes.pitchHz);

        const bool phraseOnset = ! inNote;
        inNote = true;
        currentlyVoiced = true;
        currentlySibilant = false;
        unvoicedHopCount = 0;

        lastUncorrectedHz = mpmRes.pitchHz;

        if (phraseOnset)
            vibratoTracker.startNote(detectedMidi);
        else
            vibratoTracker.update(detectedMidi);

        const bool vibratoFound = useAutoVibratoCentre && vibratoTracker.isActive()
                               && std::abs(detectedMidi - vibratoTracker.centre()) < 0.45f;
        const float choiceMidi = vibratoFound ? vibratoTracker.centre() : detectedMidi;
        const QuantizerResult qRes = quantizer.process(Quantizer::midiToHz(choiceMidi));

        float targetMidi = Quantizer::hzToMidi(qRes.targetHz);
        float noteCenterMult = 1.0f;
        float noteVarMult = 1.0f;
        float noteRampIn = 0.18f;

        // Upgrade #4: Real-Time MIDI Target Note Override (if a live MIDI note is held on the input)
        if (midiTargetNote >= 0 && midiTargetNote <= 127)
        {
            float midiTargetF = static_cast<float>(midiTargetNote);
            while (midiTargetF - detectedMidi > 6.5f)  midiTargetF -= 12.0f;
            while (detectedMidi - midiTargetF > 6.5f)  midiTargetF += 12.0f;
            targetMidi = midiTargetF;
        }
        else if (overrideActive && std::abs(overrideTargetMidi - detectedMidi) <= 12.0f)
        {
            targetMidi     = overrideTargetMidi;
            noteCenterMult = overrideCenter;
            noteVarMult    = overrideVariation;
            noteRampIn     = overrideRampIn;
        }

        const float range = vibratoFound ? juce::jmax(vibratoDepthSemitones, vibratoTracker.depth()) : 0.0f;
        const bool targetStepped = (!phraseOnset && std::abs(targetMidi - lastTargetMidi) >= 0.45f);
        const bool pitchStepped  = (!phraseOnset && std::abs(detectedMidi - vibratoCentreMidi) > 1.10f);
        const bool newNote = qRes.noteChanged || targetStepped || pitchStepped
            || (range > 0.0f && std::abs(detectedMidi - vibratoTracker.centre()) > range + holdEscapeSemitones);
        lastTargetMidi = targetMidi;

        // Upgrade #1: Clarity-Squared Perceptual Pitch Center & Independent Slow Drift Tracker
        const float hopPerceptualWeight = juce::jmax(1.0e-5f, rms * mpmRes.clarity * mpmRes.clarity);

        if (phraseOnset)
        {
            previousSungAnchorMidi   = perceptualNoteAnchorMidi > 0.0f ? perceptualNoteAnchorMidi : detectedMidi;
            previousNoteTargetMidi   = vibratoCentreMidi > 0.0f ? vibratoCentreMidi : detectedMidi;
            noteStartCentreMidi      = detectedMidi;
            vibratoCentreMidi        = detectedMidi;
            perceptualNoteAnchorMidi = detectedMidi;
            perceptualAnchorWeight   = hopPerceptualWeight;
            slowDriftMidi            = detectedMidi;
            hopsSincePhraseOnset     = 0;
            hopsSinceNoteChange      = 0;
        }
        else if (newNote)
        {
            previousSungAnchorMidi   = perceptualNoteAnchorMidi;
            previousNoteTargetMidi   = smoothedDesiredCentreMidi;
            noteStartCentreMidi      = smoothedDesiredCentreMidi;
            vibratoCentreMidi        = detectedMidi;
            perceptualNoteAnchorMidi = detectedMidi;
            perceptualAnchorWeight   = hopPerceptualWeight;
            slowDriftMidi            = detectedMidi;
            vibratoTracker.startNote(detectedMidi);
            ++hopsSincePhraseOnset;
            hopsSinceNoteChange      = 0;
        }
        else
        {
            vibratoCentreMidi += vibratoCentreAlpha * (detectedMidi - vibratoCentreMidi);
            slowDriftMidi     += slowDriftAlpha     * (detectedMidi - slowDriftMidi);

            // Lock perceptual note anchor onto high-clarity vowel core during the first ~120 ms of a note
            if (hopsSinceNoteChange < 14 && mpmRes.clarity >= 0.65f && std::abs(detectedMidi - perceptualNoteAnchorMidi) < 0.85f)
            {
                perceptualNoteAnchorMidi = (perceptualNoteAnchorMidi * perceptualAnchorWeight + detectedMidi * hopPerceptualWeight)
                                         / (perceptualAnchorWeight + hopPerceptualWeight);
                perceptualAnchorWeight   = juce::jmin(perceptualAnchorWeight + hopPerceptualWeight, 2.5f);
            }
            ++hopsSincePhraseOnset;
            ++hopsSinceNoteChange;
        }
        updateHeldNote(detectedMidi);

        const float desiredMidi = computeDesiredMidiAtHop(
            detectedMidi, targetMidi, phraseOnset, noteCenterMult, noteVarMult, noteRampIn, mpmRes.clarity);

        const float correction = juce::jlimit(-12.0f, 12.0f, desiredMidi - detectedMidi);

        const bool largeStep = std::abs(detectedMidi - lastDetectedMidi) > 3.0f;
        prevHopDetectedMidi = (phraseOnset || largeStep) ? detectedMidi : lastDetectedMidi;
        lastDetectedMidi    = detectedMidi;
        prevHopDesiredMidi  = (phraseOnset || largeStep) ? desiredMidi  : lastDesiredMidi;
        lastDesiredMidi     = desiredMidi;

        lastCorrectedHz = Quantizer::midiToHz(desiredMidi);
        smoother.setTargetSemitones(correction);

        if (polyphonicEnabled)
            analyzePolyphonicHop(readPtr, rms);
        else
            numActivePolyVoices = 0;
    }

    MPMResult trackPitch(const MPMResult& pick) const noexcept
    {
        const auto& p = trackerParams;
        auto inRange = [this](float hz) noexcept { return hz >= activeMinVoiceHz && hz <= activeMaxVoiceHz; };

        const bool pickOk = pick.isVoiced && inRange(pick.pitchHz) && pick.clarity >= p.openClarity;

        auto resolveFundamental = [&](const MPMResult& cand) noexcept -> MPMResult
        {
            if (cand.clarity >= 0.92f)
                return cand;
            const float candMidi = Quantizer::hzToMidi(cand.pitchHz);
            int bestSub = -1;
            float bestSubClarity = 0.0f;
            for (int i = 0; i < detector.getNumCandidates(); ++i)
            {
                const auto& c = detector.getCandidate(i);
                if (! inRange(c.pitchHz) || c.clarity < p.holdClarity)
                    continue;
                const float diff = candMidi - Quantizer::hzToMidi(c.pitchHz);
                const bool isHarmonicRatio = (std::abs(diff - 12.0f) < 1.35f)
                                          || (std::abs(diff - 19.02f) < 1.35f)
                                          || (std::abs(diff - 24.0f) < 1.35f);
                if (isHarmonicRatio && c.clarity > cand.clarity + 0.02f && c.clarity > bestSubClarity)
                {
                    bestSubClarity = c.clarity;
                    bestSub = i;
                }
            }
            return bestSub >= 0 ? detector.getCandidate(bestSub) : cand;
        };

        if (! inNote)
        {
            if (! pickOk)
                return MPMResult{};
            return resolveFundamental(pick);
        }

        int best = -1;
        float bestClarity = 0.0f;
        for (int i = 0; i < detector.getNumCandidates(); ++i)
        {
            const auto& c = detector.getCandidate(i);
            if (! inRange(c.pitchHz) || c.clarity < p.holdClarity)
                continue;
            if (std::abs(Quantizer::hzToMidi(c.pitchHz) - lastDetectedMidi) > p.continuitySemitones)
                continue;
            if (c.clarity > bestClarity)
            {
                bestClarity = c.clarity;
                best = i;
            }
        }

        if (pickOk)
        {
            const MPMResult resolvedPick = resolveFundamental(pick);
            const float jump = Quantizer::hzToMidi(resolvedPick.pitchHz) - lastDetectedMidi;
            auto near = [jump](float semis) noexcept { return std::abs(jump - semis) < 1.35f; };
            const float margin = (near(12.0f) || near(19.0f) || near(24.0f)) ? p.octaveMargin
                               : (near(-12.0f) || near(-19.0f) || near(-24.0f)) ? p.octaveMarginDown : -1.0f;
            if (margin >= 0.0f && best >= 0 && bestClarity >= resolvedPick.clarity - margin)
                return detector.getCandidate(best);
            return resolvedPick;
        }
        return best >= 0 ? detector.getCandidate(best) : MPMResult{};
    }

    void refinePitch(MPMResult& res, const float* frame) noexcept
    {
        const float period = static_cast<float> (sampleRate) / res.pitchHz;
        int level = -1;
        for (int i = static_cast<int> (fineDetectors.size()) - 1; i >= 0; --i)
            if (static_cast<float> (fineDetectors[static_cast<size_t> (i)].getWindowSize()) >= trackerParams.fineWindowPeriods * period)
            {
                level = i;
                break;
            }
        if (level < 0)
            return;

        auto& d = fineDetectors[static_cast<size_t> (level)];
        const int w = d.getWindowSize();
        const MPMResult fine = d.processFrame(frame + (windowSize - w) / 2);

        const float coarseMidi = Quantizer::hzToMidi(res.pitchHz);
        auto agrees = [&](const MPMResult& c) noexcept
        {
            return c.pitchHz > 0.0f && c.clarity >= trackerParams.holdClarity
                && std::abs(Quantizer::hzToMidi(c.pitchHz) - coarseMidi) < 1.0f;
        };
        if (agrees(fine))
        {
            res.pitchHz = fine.pitchHz;
            return;
        }
        for (int i = 0; i < d.getNumCandidates(); ++i)
            if (agrees(d.getCandidate(i)))
            {
                res.pitchHz = d.getCandidate(i).pitchHz;
                return;
            }
    }

    static float quinticSmoothstep(float u) noexcept
    {
        u = juce::jlimit(0.0f, 1.0f, u);
        return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);
    }

    float computeDesiredMidiAtHop(float detectedMidi, float targetMidi, bool phraseOnset,
                                  float noteCenterMult, float noteVarMult, float noteRampIn,
                                  float frameClarity) noexcept
    {
        // Upgrade #3: If the user painted a custom freehand curve (`4. DRAW CURVE`) on the active hop,
        // follow the custom drawn pitch directly:
        if (overrideActive && overrideHasCustomPitch && std::abs(overrideCustomPitchMidi - detectedMidi) <= 12.0f)
        {
            smoothedDesiredCentreMidi = overrideCustomPitchMidi;
            return overrideCustomPitchMidi;
        }

        // Continuous Humanize Drive:
        //   Slider 0%   -> hDrive = 0.0 (hGate = 0.0, hExtreme = 0.0)
        //   Slider 50%  -> hDrive = 1.0 (hGate = 1.0, hExtreme = 0.0 -> matches previous ON button 1:1)
        //   Slider 100% -> hDrive = 2.0 (hGate = 1.0, hExtreme = 1.0 -> Extreme Humanization)
        const float hDrive   = juce::jlimit(0.0f, 2.0f, smoothedHumanizeGate);
        const float hGate    = juce::jlimit(0.0f, 1.0f, hDrive);
        const float hExtreme = juce::jlimit(0.0f, 1.0f, hDrive - 1.0f);

        const bool hasTrueVibrato = (useAutoVibratoCentre || ablateTrackerCentre) && vibratoTracker.isActive()
                                 && std::abs(detectedMidi - vibratoTracker.centre()) < 0.45f;
        const float centre = hasTrueVibrato ? vibratoTracker.centre() : vibratoCentreMidi;

        // Upgrade #1: Separate Slow Pitch Drift (< 3 Hz sag/tilt) from Fast Vibrato (4-12 Hz):
        const float rawVibDev = detectedMidi - centre;
        const float nonVibScale = 0.28f + 0.12f * hGate + 0.18f * hExtreme;
        const float nonVibLimit = 0.12f + 0.04f * hGate + 0.08f * hExtreme;
        const float vibDeviation = hasTrueVibrato
            ? juce::jlimit(-0.35f - 0.15f * hExtreme, 0.35f + 0.15f * hExtreme, rawVibDev)
            : juce::jlimit(-nonVibLimit, nonVibLimit, nonVibScale * rawVibDev);

        const float slowDriftDev = juce::jlimit(-0.45f, 0.45f, slowDriftMidi - perceptualNoteAnchorMidi);

        // 2. Soft-Zone Pitch Attractor + Drift Flattening:
        //    Pulls both the fast vibrato centre and the perceptual note anchor toward `targetMidi`
        //    so `Center = 50%` always cuts off-scale error in half while `Humanize` shapes the pocket & expression.
        const float baseCenter = juce::jlimit(0.0f, 1.0f, smoothedCenterAmount * noteCenterMult);
        const float effCenter  = baseCenter * (1.0f - 0.06f * hDrive);
        const float anchorRef  = (perceptualNoteAnchorMidi > 0.0f)
            ? (0.65f * centre + 0.35f * perceptualNoteAnchorMidi)
            : centre;
        const float centreErr  = targetMidi - anchorRef;
        const float softDeadband = 0.028f * hDrive; // +-2.8c at 50% Humanize, +-5.6c at 100% Extreme Humanize
        const float shapedCentreErr = (softDeadband > 1.0e-4f)
            ? std::copysign(juce::jmax(0.0f, std::abs(centreErr) - softDeadband * std::tanh(std::abs(centreErr) / softDeadband)), centreErr)
            : centreErr;

        const float keptDrift = (1.0f - effCenter) * driftAmount * slowDriftDev;
        const float rawCorrectedCentre = anchorRef + effCenter * shapedCentreErr + keptDrift;

        if (!centreSmootherInitialized || phraseOnset || std::abs(rawCorrectedCentre - smoothedDesiredCentreMidi) > 4.5f)
        {
            smoothedDesiredCentreMidi = rawCorrectedCentre;
            centreSmootherInitialized = true;
        }
        else
        {
            smoothedDesiredCentreMidi += centreTransitionAlpha * (rawCorrectedCentre - smoothedDesiredCentreMidi);
        }

        // 3. Minimum-Jerk S-Curve Inter-Note Glide (2.5 hops at 50%, 4.0 hops at 100% Extreme):
        float activeCentreMidi = rawCorrectedCentre;
        const int maxGlideHops = 3 + static_cast<int>(std::lround(1.5f * hExtreme));
        if (hGate > 0.01f && !phraseOnset && hopsSinceNoteChange < maxGlideHops
            && std::abs(rawCorrectedCentre - noteStartCentreMidi) < 2.5f)
        {
            const float glideHops = 2.5f + 1.5f * hExtreme;
            const float u = (static_cast<float>(hopsSinceNoteChange) + 0.5f) / glideHops;
            const float sCurve = quinticSmoothstep(u);
            const float sGlided = noteStartCentreMidi + sCurve * (rawCorrectedCentre - noteStartCentreMidi);
            activeCentreMidi = rawCorrectedCentre + hGate * 0.55f * (sGlided - rawCorrectedCentre);
        }
        smoothedDesiredCentreMidi = activeCentreMidi;

        // 4. Target-Centered Intent-Proportional Variation ("As If The Artist Went For That Note"):
        //    At 50% (hDrive=1.0): matches the calibrated standard Humanize response 1:1.
        //    At 100% (hDrive=2.0): Extreme Humanization amplifies interval steepness contrast,
        //    deepens target-centered onset articulation, and increases organic vocal modulation
        //    without ever pushing the note's DC pitch center off target!
        const float rawVar = juce::jlimit(0.0f, 2.0f, smoothedVariationAmount * noteVarMult);
        const float effVar = juce::jmax(rawVar, 0.35f * hGate + 0.25f * hExtreme);

        float intentSlopeScale = 1.0f;
        float entryScoopVariation = 0.0f;

        if (hGate > 0.01f)
        {
            const float sungAnchor     = perceptualNoteAnchorMidi > 0.0f ? perceptualNoteAnchorMidi : centre;
            const float pullDelta      = rawCorrectedCentre - sungAnchor;
            const float sungInterval   = sungAnchor - previousSungAnchorMidi;
            const float aimedInterval  = rawCorrectedCentre - previousNoteTargetMidi;

            const float intervalRatio  = (!phraseOnset && std::abs(sungInterval) > 0.08f)
                ? ((std::abs(aimedInterval) + 0.55f) / (std::abs(sungInterval) + 0.55f))
                : (1.0f + 0.22f * std::tanh(pullDelta / 1.5f));

            const float registerSlope  = std::exp2(juce::jlimit(-12.0f, 12.0f, pullDelta) / 36.0f);
            const float rawIntentSlope = juce::jlimit(0.58f, 1.62f, intervalRatio * registerSlope);

            // Asymmetric error-side shaping: strongly damps excursions that sag back toward the singer's
            // original pitch error (`rawVibDev * pullDelta < 0`), while steepening excursions aimed at the target note:
            const float asymDir = (rawVibDev * pullDelta < 0.0f)
                ? (1.0f - (0.35f + 0.25f * hExtreme) * std::tanh(std::abs(pullDelta) / 0.30f))
                : (1.0f + (0.14f + 0.18f * hExtreme) * std::tanh(std::abs(pullDelta) / 0.45f));

            const float baseSlope = juce::jlimit(0.48f, 1.68f, rawIntentSlope * asymDir);
            const float extremeGain = 1.0f + 0.75f * hExtreme;
            intentSlopeScale = juce::jlimit(0.35f, 2.15f, 1.0f + hGate * extremeGain * (baseSlope - 1.0f));

            // Re-targeted onset scoop (only during the first 4..6 hops of a note, strictly relative to `centre`
            // so it never adds a static offset to the note core):
            const int maxScoopHops = 4 + static_cast<int>(std::lround(2.0f * hExtreme));
            if (hopsSinceNoteChange < maxScoopHops)
            {
                const float onsetDecay = 1.0f - quinticSmoothstep(static_cast<float>(hopsSinceNoteChange) / static_cast<float>(maxScoopHops));
                const float onsetSwing = juce::jlimit(-0.35f, 0.35f, rawVibDev);
                entryScoopVariation = hGate * (0.22f + 0.18f * hExtreme) * onsetDecay * rawIntentSlope * onsetSwing;
            }
        }

        float shapedDeviation = juce::jlimit(-0.42f, 0.42f, effVar * intentSlopeScale * vibDeviation + entryScoopVariation);

        if (heldGain > 0.0f && vibratoDepthSemitones > 0.0f && effVar > 0.01f && effVar < 1.0f)
        {
            const float vibCap = heldGain * softLimit(vibDeviation * intentSlopeScale, vibratoDepthSemitones);
            if (std::abs(vibCap) < std::abs(shapedDeviation))
                shapedDeviation = vibCap;
        }

        float organicMicroDrift = 0.0f;
        if (hGate > 0.01f)
        {
            constexpr float twoPi = juce::MathConstants<float>::twoPi;
            humanizeDriftPhase1 += twoPi * 1.37f * hopSeconds;
            if (humanizeDriftPhase1 >= twoPi) humanizeDriftPhase1 -= twoPi;
            humanizeDriftPhase2 += twoPi * 2.83f * hopSeconds;
            if (humanizeDriftPhase2 >= twoPi) humanizeDriftPhase2 -= twoPi;
            humanizeVibPhase    += twoPi * 5.40f * hopSeconds;
            if (humanizeVibPhase >= twoPi)    humanizeVibPhase    -= twoPi;

            const float larynDrift = 0.011f * std::sin(humanizeDriftPhase1)
                                   + 0.007f * std::sin(humanizeDriftPhase2);

            const float noteDurSec = static_cast<float>(hopsSinceNoteChange) * hopSeconds;
            const float bloomEnv   = (!hasTrueVibrato && noteDurSec > 0.18f)
                ? juce::jlimit(0.0f, 1.0f, (noteDurSec - 0.18f) / 0.30f)
                : 0.0f;
            const float gentleVib  = 0.032f * bloomEnv * std::sin(humanizeVibPhase);

            const float driftScale = hGate + 0.75f * hExtreme;
            organicMicroDrift = driftScale * intentSlopeScale * (larynDrift + gentleVib);
        }

        float desiredMidi = activeCentreMidi + shapedDeviation + organicMicroDrift;

        // 5. Quintic Phrase-Onset Scoop Preservation (only when Humanize is OFF and Transition is slow;
        //    when Humanize is ON, `entryScoopVariation` already projects the scoop directly around `activeCentreMidi`!):
        if (! ablateNoOnsetGate && hGate <= 0.01f && transitionAmount < 0.90f && hopsSincePhraseOnset < 5)
        {
            const float rampDurationSec = (1.0f - transitionAmount) * 0.030f * (noteRampIn / 0.18f);
            const float secondsSinceOnset = static_cast<float> (hopsSincePhraseOnset) * hopSeconds;
            if (secondsSinceOnset < rampDurationSec && rampDurationSec > 1.0e-4f)
            {
                const float phase = juce::jlimit(0.0f, 1.0f, secondsSinceOnset / rampDurationSec);
                const float w = quinticSmoothstep(phase);
                desiredMidi = detectedMidi + w * (desiredMidi - detectedMidi);
            }
        }

        // 6. Clarity-Weighted Breathy Tail & Per-Note Ramp-Out Protection:
        const float tailThresh = 0.52f + (overrideActive ? (overrideRampOut - 0.18f) * 0.45f : 0.0f);
        if ((hGate > 0.01f || (overrideActive && overrideRampOut > 0.22f)) && frameClarity < tailThresh)
        {
            const float clarityWeight = juce::jlimit(0.55f, 1.0f, (frameClarity - 0.26f) / juce::jmax(0.10f, tailThresh - 0.26f));
            const float tailGate = juce::jmax(hGate, overrideActive ? juce::jlimit(0.0f, 1.0f, (overrideRampOut - 0.18f) * 3.0f) : 0.0f);
            const float blend = 1.0f - tailGate * (1.0f - clarityWeight);
            desiredMidi = detectedMidi + blend * (desiredMidi - detectedMidi);
        }

        return desiredMidi;
    }

    void updateHeldNote(float detectedMidi) noexcept
    {
        const float secondsSinceOnset = static_cast<float> (hopsSinceNoteChange) * hopSeconds;
        heldNote = vibratoDepthSemitones > 0.0f
                && secondsSinceOnset >= holdSettleSeconds
                && std::abs(detectedMidi - vibratoCentreMidi) <= vibratoDepthSemitones + holdEscapeSemitones;

        const float step = hopSeconds / holdFadeSeconds;
        heldGain = heldNote ? juce::jmin(1.0f, heldGain + step) : juce::jmax(0.0f, heldGain - step);
    }

    static float softLimit(float x, float limit) noexcept
    {
        const float knee = 0.7f * limit;
        const float a = std::abs(x);
        if (a <= knee)
            return x;
        return std::copysign(knee + (limit - knee) * std::tanh((a - knee) / (limit - knee)), x);
    }

    void handleUnvoicedHop(bool hasEnergy) noexcept
    {
        lastUncorrectedHz = 0.0f;
        currentlyVoiced = false;
        jumpPending = false;
        ++unvoicedHopCount;

        const int dynamicHoldHops = hasEnergy ? unvoicedHopsBeforeRelease : 2;

        if (unvoicedHopCount >= dynamicHoldHops)
        {
            inNote = false;
            heldNote = false;
            centreSmootherInitialized = false;
            prevHopDetectedMidi = 0.0f;
            lastDetectedMidi = 0.0f;
            prevHopDesiredMidi = 0.0f;
            lastDesiredMidi = 0.0f;
            lastCorrectedHz = 0.0f;
            smoother.setTargetSemitones(0.0f);
        }
    }

public:
    struct TrackerParams
    {
        float thresholdRatio = 0.93f;     // MPM k (0.93 prevents 2nd-harmonic octave-up false triggers)
        float openClarity = 0.70f;        // to start a note
        float holdClarity = 0.50f;        // to stay in one
        float continuitySemitones = 2.2f; // how far a held candidate may sit from the note
        float octaveMargin = 0.25f;       // octave-up pick vs the note's own octave
        float octaveMarginDown = 0.35f;   // octave-down pick vs the note's own octave
        float fineWindowPeriods = 8.0f;   // refined window length, in periods
    };
    void setTrackerParams(const TrackerParams& tp) noexcept { trackerParams = tp; }

private:
    TrackerParams trackerParams;
public:
    void setUseAutoVibratoCentre(bool b) noexcept { useAutoVibratoCentre = b; }
private:
    bool useAutoVibratoCentre = true;

    SmoothPlanner planner;
    bool smoothEnabled   = false;
    bool humanizeEnabled = false;
    int  smoothDelaySamples = 0;
    int  smoothRingSize = 0;
    int  smoothWritePos = 0;
    int  smoothTrimSamples = 0;
    int64_t inputCounter = 0;
    bool naturalOnly = false;
    bool ablateNoOnsetGate = false, ablateNoDeadband = false, ablateTrackerCentre = false;
    float ablateCentreTau = 0.0f;
public:
    void setAblateCentreTau(float s) noexcept { ablateCentreTau = s; }
    void setHumanizeShape(float deadbandSemitones, float onsetDelaySeconds, float rampSeconds) noexcept
    {
        maxDeadbandSemitones = deadbandSemitones; maxOnsetDelaySeconds = onsetDelaySeconds;
        onsetRampSeconds = juce::jmax(0.005f, rampSeconds);
    }
private:
    float appliedCorrection = 0.0f;
    juce::AudioBuffer<float> smoothRing;

    // Sub-Band Sibilant & Breath Crossover State (3.8 kHz latency-aligned TPT split):
    juce::AudioBuffer<float> sibilantDryRing;
    int   sibilantRingSize = 0;
    int   sibilantRingMask = 0;
    int   sibilantWritePos = 0;
    float sibilantLpG      = 0.2f;
    std::array<float, 2> sibilantWetLpState {};
    std::array<float, 2> sibilantDryLpState {};

public:
    void setSmoothTrimSamples(int n) noexcept { smoothTrimSamples = n; }
    void setSmoothParams(const SmoothPlanner::Params& p) noexcept { planner.setParams(p); }
    void setGlitchFilter(bool on) noexcept { planner.setGlitchFilter(on); }
    void setAblations(bool noOnsetGate, bool noDeadband, bool trackerCentre) noexcept
    {
        ablateNoOnsetGate = noOnsetGate; ablateNoDeadband = noDeadband; ablateTrackerCentre = trackerCentre;
    }
    void setRampTargets(bool b) noexcept { rampTargets = b; }
    void setAlignTrimSamples(int n) noexcept { alignTrimSamples = n; }
    void setUseFineWindow(bool b) noexcept { useFineWindow = b; }
private:
    bool rampTargets = true;
    bool useFineWindow = true;
    std::array<MPMDetector, 2> fineDetectors;
    int  alignTrimSamples = 0;
    VibratoTracker vibratoTracker;
    static constexpr float minVoiceHz = 65.0f;
    static constexpr float maxVoiceHz = 1600.0f;
    int   vocalRangeMode   = 0;
    float activeMinVoiceHz = 82.0f;
    float activeMaxVoiceHz = 1600.0f;

    static constexpr size_t subBlockSize = 16;
    static constexpr double minDetectHz = 60.0;
    static constexpr int    unvoicedHopsBeforeRelease = 10;
    static constexpr float  maxJumpSemitones = 7.0f;
    static constexpr float  pendingJumpToleranceSemitones = 2.5f;
    static constexpr float  vibratoCentreTauSeconds = 0.150f;
    float maxDeadbandSemitones = 0.03f;
    float maxOnsetDelaySeconds = 0.050f;
    float onsetRampSeconds = 0.040f;
    static constexpr float  holdSettleSeconds = 0.150f;
    static constexpr float  holdFadeSeconds = 0.100f;
    static constexpr float  holdEscapeSemitones = 0.30f;
    static constexpr float  zeroCorrectionSemitones = 0.02f;

    double sampleRate = 44100.0;
    int    windowSize = 2048;
    int    hopSize = 256;
    float  hopSeconds = 256.0f / 44100.0f;

    uint16_t         activeScaleMask = 0x0FFF;
    MPMDetector      detector;
    Quantizer        quantizer;
    SmoothingFilter  smoother;
    PsolaShifter     shifter;
    bool  overrideActive          = false;
    float overrideTargetMidi      = 60.0f;
    float overrideCenter          = 1.0f;
    float overrideVariation       = 1.0f;
    float overrideGainDb          = 0.0f;
    float overrideSibilantDb      = 0.0f;
    float overrideRampIn          = 0.18f;
    float overrideRampOut         = 0.18f;
    float overrideFadeMultiplier  = 1.0f;
    bool  overrideHasCustomPitch  = false;
    float overrideCustomPitchMidi = 60.0f;

    // Live MIDI Target Note state:
    int      midiTargetNote = -1;
    uint16_t midiTargetMask = 0;

    juce::AudioBuffer<float> analysisBuffer;
    int samplesCollected = 0;
    int samplesSinceLastHop = 0;

    std::vector<float> correctionHistory;
    size_t historyWritePos = 0;

    float lastUncorrectedHz = 0.0f;
    float lastCorrectedHz = 0.0f;
    float lastRms = 0.0f;
    float lastClarity = 0.0f;
    float lastZcr = 0.0f;
    float energyGateRms = 0.005f;
    bool  currentlyVoiced = false;
    bool  currentlySibilant = false;
    int   unvoicedHopCount = 0;
    int   hopCounter = 0;

    bool  inNote = false;
    bool  jumpPending = false;
    float pendingJumpMidi = 0.0f;
    float prevHopDetectedMidi = 0.0f;
    float lastDetectedMidi = 0.0f;
    float prevHopDesiredMidi = 0.0f;
    float lastDesiredMidi = 0.0f;
    float smoothedDesiredCentreMidi = 60.0f;
    float noteStartCentreMidi = 60.0f;
    float centreTransitionAlpha = 1.0f;
    bool  centreSmootherInitialized = false;
    float vibratoCentreMidi = 0.0f;
    float perceptualNoteAnchorMidi = 60.0f;
    float perceptualAnchorWeight = 0.0f;
    float slowDriftMidi = 60.0f;
    float slowDriftAlpha = 0.05f;
    float driftAmount = 0.15f;
    float previousNoteTargetMidi = 60.0f;
    float lastTargetMidi = 60.0f;
    float previousSungAnchorMidi = 60.0f;
    float vibratoCentreAlpha = 0.0f;
    int   hopsSincePhraseOnset = 0;
    int   hopsSinceNoteChange = 0;
    bool  heldNote = false;
    float heldGain = 0.0f;

    float fastInstantSmoothAlpha  = 0.02f;
    float humanInstantSmoothAlpha = 0.007f;
    float instantSmoothAlpha      = 0.02f;
    float smoothedInstantCorrection = 0.0f;
    float macroHopSmoothAlpha = 0.30f;

    float flexAmount = 0.65f;
    float humanizeAmount = 0.25f;
    float vibratoDepthSemitones = 0.0f;

    // NewTone Global Macro Parameters:
    float centerAmount            = 0.85f;
    float smoothedCenterAmount    = 0.85f;
    float variationAmount         = 1.00f;
    float smoothedVariationAmount = 1.00f;
    float transitionAmount        = 0.65f;
    float sensitivityAmount       = 0.65f;

    // Humanize Engine State:
    float smoothedHumanizeGate    = 0.0f;
    float humanizeDriftPhase1     = 0.0f;
    float humanizeDriftPhase2     = 1.7f;
    float humanizeVibPhase        = 0.0f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> noteGainSmoothed      { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sibilantGainSmoothed  { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> sibilantBlendSmoothed { 0.0f };

    //==============================================================================
    // Polyphonic (Melodyne-Style Chord / Choir / String) Detection & Per-Voice Shifter
    // Supports up to 16 simultaneous polyphonic voices (full extended chords, 6-string
    // guitars, choirs, and symphonic string ensembles).
    //==============================================================================
    static constexpr int maxPolyVoices = 16;

    struct PolyVoiceShifter
    {
        double sampleRate   = 48000.0;
        int    numChannels  = 2;
        float  targetFc     = 220.0f;
        float  smoothedFc   = 220.0f;
        float  slotMidi     = 0.0f;
        float  targetRatio  = 1.0f;
        float  targetGain   = 1.0f;
        bool   slotActive   = false;

        // Zavalishin Topology-Preserving Transform (TPT) SVF state — orthogonal & bounded,
        // so changing `smoothedFc` never causes the +-0.85 impulse spikes of a DF2T biquad!
        std::array<float, 2> ic1eq {}, ic2eq {};

        static constexpr int ringSize = 2048;
        static constexpr int ringMask = ringSize - 1;
        std::array<std::vector<float>, 2> delayRing;
        int    writePos      = 0;
        double phase         = 0.0;
        double smoothedRatio = 1.0;
        float  smoothedGain  = 1.0f;
        float  activeLevel   = 0.0f;

        void prepare(double sr, int chans)
        {
            sampleRate  = sr;
            numChannels = juce::jlimit(1, 2, chans);
            for (int c = 0; c < 2; ++c)
                delayRing[(size_t)c].assign(ringSize, 0.0f);
            reset();
        }

        void reset() noexcept
        {
            ic1eq.fill(0.0f);
            ic2eq.fill(0.0f);
            for (int c = 0; c < 2; ++c)
                std::fill(delayRing[(size_t)c].begin(), delayRing[(size_t)c].end(), 0.0f);
            writePos      = 0;
            phase         = 0.0;
            smoothedRatio = 1.0;
            smoothedGain  = 1.0f;
            activeLevel   = 0.0f;
            slotMidi      = 0.0f;
            targetRatio   = 1.0f;
            targetGain    = 1.0f;
            slotActive    = false;
        }

        void setTargetBandpass(float fc, bool wasIdle) noexcept
        {
            fc = juce::jlimit(65.0f, static_cast<float>(sampleRate * 0.42), fc);
            targetFc = fc;
            if (wasIdle)
                smoothedFc = fc;
        }

        float readHermite(int ch, double delaySamples) const noexcept
        {
            const auto& ring = delayRing[(size_t)ch];
            const double clamped = juce::jlimit(2.0, static_cast<double>(ringSize - 4), delaySamples);
            const double idD = std::floor(clamped);
            const int id = static_cast<int>(idD);
            const float mu = static_cast<float>(clamped - idD);
            const float y0 = ring[(size_t)((writePos - (id - 1)) & ringMask)];
            const float y1 = ring[(size_t)((writePos - id) & ringMask)];
            const float y2 = ring[(size_t)((writePos - (id + 1)) & ringMask)];
            const float y3 = ring[(size_t)((writePos - (id + 2)) & ringMask)];
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * mu + c2) * mu + c1) * mu + y1;
        }

        void processSample(int numCh, float* chSamples) noexcept
        {
            const bool needsShift = slotActive && (std::abs(targetRatio - 1.0f) > 0.0015f || std::abs(targetGain - 1.0f) > 0.01f);
            const float targetAct = needsShift ? 1.0f : 0.0f;

            // Smooth 8 ms fade-in / fade-out envelope and continuous ratio/cutoff smoothing
            activeLevel   += 0.0025f * (targetAct - activeLevel);
            smoothedRatio += 0.0035 * (static_cast<double>(slotActive ? targetRatio : 1.0f) - smoothedRatio);
            smoothedGain  += 0.0035f * ((slotActive ? targetGain : 1.0f) - smoothedGain);
            smoothedFc    += 0.0040f * (targetFc - smoothedFc);

            // Signed phase integration: never flips or teleports read pointers when smoothedRatio crosses 1.0!
            const double wMax = juce::jlimit(128.0, 960.0, sampleRate * 0.016);
            const double dDot = 1.0 - smoothedRatio;
            phase += dDot / wMax;
            phase -= std::floor(phase);
            double phase2 = phase + 0.5;
            phase2 -= std::floor(phase2);

            const double d1 = 2.0 + phase * wMax;
            const double d2 = 2.0 + phase2 * wMax;
            const float s1 = std::sin(juce::MathConstants<float>::pi * static_cast<float>(phase));
            const float s2 = std::sin(juce::MathConstants<float>::pi * static_cast<float>(phase2));
            const float win1 = s1 * s1;
            const float win2 = s2 * s2; // Exact partition of unity: win1 + win2 === 1.0!

            // Zavalishin TPT SVF coefficients (Q = 5.5, exact 0 dB bandpass peak = k * v1)
            const float g = std::tan(juce::MathConstants<float>::pi * juce::jlimit(65.0f, 4000.0f, smoothedFc) / static_cast<float>(sampleRate));
            constexpr float k = 1.0f / 5.5f;
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;

            for (int c = 0; c < juce::jmin(numCh, 2); ++c)
            {
                const float x = chSamples[c];
                const float v3 = x - ic2eq[(size_t)c];
                const float v1 = a1 * ic1eq[(size_t)c] + a2 * v3;
                const float v2 = ic2eq[(size_t)c] + a2 * ic1eq[(size_t)c] + a3 * v3;
                ic1eq[(size_t)c] = 2.0f * v1 - ic1eq[(size_t)c];
                ic2eq[(size_t)c] = 2.0f * v2 - ic2eq[(size_t)c];
                const float yBp = k * v1;

                delayRing[(size_t)c][(size_t)writePos] = yBp;
                if (activeLevel > 1.0e-4f)
                {
                    const float shifted = win1 * readHermite(c, d1) + win2 * readHermite(c, d2);
                    // Blend gently (0.45 max wet delta) so micro-pitch centering never comb-filters transients
                    chSamples[c] = x + (0.45f * activeLevel) * (smoothedGain * shifted - yBp);
                }
            }
            writePos = (writePos + 1) & ringMask;
        }
    };

    float specMagAtMidi(float midiNote, float binHz, int numBins) const noexcept
    {
        const float hz = Quantizer::midiToHz(midiNote);
        const int k = static_cast<int>(std::lround(hz / binHz));
        if (k < 2 || k >= numBins - 2)
            return 0.0f;
        return juce::jmax(polyMagSpectrum[(size_t)(k - 1)],
                          juce::jmax(polyMagSpectrum[(size_t)k], polyMagSpectrum[(size_t)(k + 1)]));
    }

    void analyzePolyphonicHop(const float* /*frame*/, float frameRms) noexcept
    {
        if (!polyFft.has_value() || polyWindowSize <= 0 || frameRms <= energyGateRms * 0.8f)
        {
            numActivePolyVoices = 0;
            return;
        }

        std::fill(polyFftBuffer.begin(), polyFftBuffer.end(), 0.0f);
        const int wMask = polyWindowSize - 1;
        for (int i = 0; i < polyWindowSize; ++i)
        {
            const float s = polyHistoryRing[(size_t)((polyHistoryWritePos + i) & wMask)];
            polyFftBuffer[(size_t)i] = s * polyHannWindow[(size_t)i];
        }

        polyFft->performRealOnlyForwardTransform(polyFftBuffer.data());

        const int numBins = polyFftSize / 2;
        const float binHz = static_cast<float>(sampleRate) / static_cast<float>(polyFftSize);
        float maxSpecMag = 0.0f;

        for (int k = 0; k <= numBins; ++k)
        {
            const float re = polyFftBuffer[(size_t)(2 * k)];
            const float im = polyFftBuffer[(size_t)(2 * k + 1)];
            const float mag = std::sqrt(re * re + im * im);
            polyMagSpectrum[(size_t)k] = mag;
            if (k >= 10 && k < numBins / 4)
                maxSpecMag = juce::jmax(maxSpecMag, mag);
        }

        if (maxSpecMag <= 1.0e-6f)
        {
            numActivePolyVoices = 0;
            return;
        }

        struct SpectralPeak
        {
            float hz          = 0.0f;
            float midi        = 0.0f;
            float origMag     = 0.0f;
            float residualMag = 0.0f;
            float salience    = 0.0f;
            bool  used        = false;
        };

        constexpr int maxPeaks = 96;
        std::array<SpectralPeak, maxPeaks> peaks {};
        int numPeaks = 0;

        const float baseFloor = juce::jmap(sensitivityAmount, 0.0f, 1.0f, 0.032f, 0.015f);
        const float minPeakFloor = maxSpecMag * (midiCleanModeEnabled ? juce::jmax(0.038f, baseFloor * 1.45f) : baseFloor);
        const int kMin = juce::jmax(6, static_cast<int>(std::ceil(74.0f / binHz)));
        const int kMax = juce::jmin(numBins - 4, static_cast<int>(std::floor(1650.0f / binHz)));

        for (int k = kMin; k <= kMax && numPeaks < maxPeaks - 16; ++k)
        {
            const float y0 = polyMagSpectrum[(size_t)k];
            if (y0 < minPeakFloor)
                continue;

            const float yL1 = polyMagSpectrum[(size_t)(k - 1)];
            const float yR1 = polyMagSpectrum[(size_t)(k + 1)];
            const float yL2 = polyMagSpectrum[(size_t)(k - 2)];
            const float yR2 = polyMagSpectrum[(size_t)(k + 2)];

            if (y0 < yL1 || y0 <= yR1 || y0 <= yL2 || y0 <= yR2)
                continue;

            // In MIDI Clean Mode, require peak prominence >= 3.2x (+10.1 dB) above local spectral neighborhood
            // to reject broadband drum hits, cymbal wash, pick scrapes, and amp hiss:
            if (midiCleanModeEnabled)
            {
                float neighSum = 0.0f;
                int neighCnt = 0;
                const int wLo = juce::jmax(4, k - 12);
                const int wHi = juce::jmin(numBins - 2, k + 12);
                for (int m = wLo; m <= wHi; ++m)
                {
                    if (std::abs(m - k) > 2)
                    {
                        neighSum += polyMagSpectrum[(size_t)m];
                        ++neighCnt;
                    }
                }
                const float localMean = (neighCnt > 0) ? (neighSum / static_cast<float>(neighCnt)) : 0.0f;
                if (y0 < localMean * 3.15f)
                    continue;
            }

            const float lL1 = std::log(juce::jmax(1.0e-9f, yL1));
            const float l0  = std::log(juce::jmax(1.0e-9f, y0));
            const float lR1 = std::log(juce::jmax(1.0e-9f, yR1));
            const float denom = lL1 - 2.0f * l0 + lR1;
            float delta = 0.0f;
            if (denom < -1.0e-6f)
                delta = juce::jlimit(-0.5f, 0.5f, 0.5f * (lL1 - lR1) / denom);

            const float exactHz  = (static_cast<float>(k) + delta) * binHz;
            const float exactMag = std::exp(l0 - 0.25f * (lL1 - lR1) * delta);

            if (exactHz >= 74.0f && exactHz <= 1650.0f)
            {
                peaks[(size_t)numPeaks++] = { exactHz, Quantizer::hzToMidi(exactHz), exactMag, exactMag, exactMag, false };
            }
        }

        // Compute harmonic salience (f0 + 2*f0 + 3*f0 + 4*f0) with per-harmonic cap so loud open treble
        // strings (like B3) never hallucinate a sub-harmonic at B3/3 = E2:
        for (int i = 0; i < numPeaks; ++i)
        {
            auto& pk = peaks[(size_t)i];
            float hSum = 0.0f;
            static constexpr float hWeights[3] = { 1.05f, 0.60f, 0.35f };
            static constexpr float hCaps[3]    = { 4.20f, 2.20f, 1.60f };
            for (int h = 2; h <= 4; ++h)
            {
                const float hm = pk.midi + 12.0f * std::log2(static_cast<float>(h));
                float bestH = 0.0f;
                for (int q = i + 1; q < numPeaks; ++q)
                {
                    if (std::abs(peaks[(size_t)q].midi - hm) < 0.50f)
                        bestH = juce::jmax(bestH, peaks[(size_t)q].origMag);
                }
                hSum += hWeights[h - 2] * juce::jmin(bestH, pk.origMag * hCaps[h - 2]);
            }
            pk.salience = pk.origMag * 1.25f + hSum;
        }

        struct VoiceCand
        {
            float midi = 0.0f;
            float salience = 0.0f;
        };
        std::array<VoiceCand, maxPolyVoices> found {};
        int foundCount = 0;

        const uint16_t effPolyMask = (midiTargetMask != 0) ? midiTargetMask : activeScaleMask;

        // =========================================================================
        // STEP 1: Primary Bass String Fundamental Selection [E2 (39.5) .. D#3 (51.5)]
        // Picks the true bass string first and removes its overtone series (h = 2..8)
        // BEFORE upper chord voices are evaluated, eliminating C3->C4, A2->A3, E2->E3,
        // and D3->D4 bass octave confusions!
        // =========================================================================
        int bestBassIdx = -1;
        float bestBassSal = maxSpecMag * 0.12f;
        for (int i = 0; i < numPeaks; ++i)
        {
            const auto& pk = peaks[(size_t)i];
            if (pk.midi >= 39.5f && pk.midi <= 51.5f && pk.origMag >= maxSpecMag * 0.03f)
            {
                if (pk.salience > bestBassSal)
                {
                    bestBassSal = pk.salience;
                    bestBassIdx = i;
                }
            }
        }

        if (bestBassIdx >= 0)
        {
            auto& bassPk = peaks[(size_t)bestBassIdx];
            bassPk.used = true;
            found[(size_t)foundCount++] = { bassPk.midi, bassPk.salience };

            for (int h = 2; h <= 8; ++h)
            {
                const float hm = bassPk.midi + 12.0f * std::log2(static_cast<float>(h));
                const int hmPc = ((static_cast<int>(std::lround(hm)) % 12) + 12) % 12;
                const bool inScale = (effPolyMask & static_cast<uint16_t>(1u << hmPc)) != 0;

                for (int q = 0; q < numPeaks; ++q)
                {
                    if (!peaks[(size_t)q].used && std::abs(peaks[(size_t)q].midi - hm) < 0.62f)
                    {
                        if (h == 3)
                        {
                            // h=3 (+19.02 st) often coincides with a real open treble string (E4 on A2, B3 on E2, G4 on C3)
                            peaks[(size_t)q].residualMag = juce::jmax(0.0f, peaks[(size_t)q].residualMag - bassPk.origMag * 0.08f);
                        }
                        else if (h == 5 && inScale)
                        {
                            // In-scale 5th harmonic (like E5 on C3 in Cmaj7): light attenuation so Step 2 folds E5 -> E4
                            peaks[(size_t)q].residualMag = juce::jmax(0.0f, peaks[(size_t)q].residualMag - bassPk.origMag * 0.12f);
                        }
                        else if (h == 2 && std::abs(peaks[(size_t)q].midi - 59.0f) < 0.65f
                                 && peaks[(size_t)q].origMag > bassPk.origMag * 3.5f)
                        {
                            // Open B3 string ringing loudly above B2 bass
                        }
                        else
                        {
                            peaks[(size_t)q].used = true;
                            peaks[(size_t)q].residualMag = 0.0f;
                        }
                    }
                }
            }
        }

        // =========================================================================
        // STEP 2: Sub-Harmonic Projection for Upper Guitar/Chord Strings [E3 (52.0) .. F#4 (66.6)]
        // On phone/room recordings where an upper string's 2nd harmonic in [G4 (66.6) .. G5 (79.5)]
        // is stronger than its fundamental in [G3 (54.6) .. F#4 (66.6)], fold the 2nd-harmonic
        // energy down 12 semitones into the true fundamental octave!
        // =========================================================================
        const int origNumPeaks = numPeaks;
        for (int i = 0; i < origNumPeaks; ++i)
        {
            auto& pk = peaks[(size_t)i];
            if (!pk.used && pk.midi > 66.6f && pk.midi <= 79.5f)
            {
                const float subMidi = pk.midi - 12.0f;
                pk.used = true;

                bool mergedIntoExisting = false;
                for (int q = 0; q < numPeaks; ++q)
                {
                    if (q != i && std::abs(peaks[(size_t)q].midi - subMidi) < 0.65f)
                    {
                        peaks[(size_t)q].used = false;
                        peaks[(size_t)q].residualMag += 0.75f * pk.residualMag;
                        mergedIntoExisting = true;
                    }
                }

                if (!mergedIntoExisting && numPeaks < maxPeaks)
                {
                    const float subMag = specMagAtMidi(subMidi, binHz, numBins);
                    const float virtMag = juce::jmax(subMag, pk.residualMag * 0.72f);
                    peaks[(size_t)numPeaks++] = {
                        Quantizer::midiToHz(subMidi),
                        subMidi,
                        virtMag,
                        virtMag,
                        virtMag,
                        false
                    };
                }
            }
        }

        float maxUpperMag = maxSpecMag * 0.08f;
        for (int i = 0; i < numPeaks; ++i)
        {
            if (!peaks[(size_t)i].used && peaks[(size_t)i].midi >= 52.0f && peaks[(size_t)i].midi <= 66.6f)
                maxUpperMag = juce::jmax(maxUpperMag, peaks[(size_t)i].residualMag);
        }

        const float minUpperFloor = juce::jmax(maxSpecMag * 0.030f, maxUpperMag * 0.12f);

        for (int pass = 0; pass < 4 && foundCount < maxPolyVoices; ++pass)
        {
            int bestIdx = -1;
            float bestVal = minUpperFloor;

            for (int i = 0; i < numPeaks; ++i)
            {
                const auto& pk = peaks[(size_t)i];
                if (pk.used || pk.midi < 52.0f || pk.midi > 66.6f || pk.residualMag < minUpperFloor)
                    continue;

                bool conflict = false;
                for (int f = 0; f < foundCount; ++f)
                {
                    const float d = std::abs(pk.midi - found[(size_t)f].midi);
                    if (d < 1.45f || std::abs(d - 12.0f) < 0.75f)
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                    continue;

                if (pk.residualMag > bestVal)
                {
                    bestVal = pk.residualMag;
                    bestIdx = i;
                }
            }

            if (bestIdx < 0)
                break;

            auto& chosenU = peaks[(size_t)bestIdx];
            chosenU.used = true;
            found[(size_t)foundCount++] = { chosenU.midi, chosenU.residualMag };

            for (int q = 0; q < numPeaks; ++q)
            {
                if (!peaks[(size_t)q].used && std::abs(peaks[(size_t)q].midi - (chosenU.midi + 12.0f)) < 0.65f)
                    peaks[(size_t)q].used = true;
            }
        }

        // =========================================================================
        // STEP 3: Instrument MIDI Clean Mode (Spectral Noise Gate + Temporal Hysteresis)
        // Filters out 1-2 hop pick/drum transients, bridges 1-4 hop beating dropouts on
        // sustained strings, and locks each confirmed string track to a stable pitch center!
        // =========================================================================
        if (midiCleanModeEnabled)
        {
            // 3a. Reject off-semitone noise peaks (> 36 cents off nearest semitone, or out-of-scale when scale is locked)
            int filteredCount = 0;
            for (int i = 0; i < foundCount; ++i)
            {
                const float m = found[(size_t)i].midi;
                const int nearM = static_cast<int>(std::lround(m));
                const float centErr = std::abs(m - static_cast<float>(nearM));
                const int pc = ((nearM % 12) + 12) % 12;
                const bool inScale = (effPolyMask == 0x0FFF) || ((effPolyMask & static_cast<uint16_t>(1u << pc)) != 0);

                if (centErr <= (inScale ? 0.40f : 0.22f) && (inScale || found[(size_t)i].salience >= maxSpecMag * 0.38f))
                {
                    // Snap 65% toward exact integer semitone for rock-solid instrument MIDI tracking
                    const float cleanM = inScale ? (0.35f * m + 0.65f * static_cast<float>(nearM))
                                                 : static_cast<float>(snapMidiToMask(m, effPolyMask));
                    found[(size_t)filteredCount++] = { cleanM, found[(size_t)i].salience };
                }
            }
            foundCount = filteredCount;

            // 3b. Update Multi-Voice Hysteresis Tracker (`polyCleanTracks`):
            //     - Requires `minConfirmHops = 2` consecutive hops to open a new MIDI voice (kills pick/drum blips)
            //     - Bridges up to `maxHoldHops = 4` missing hops so sustained guitar/piano strings never chop into pieces
            std::array<bool, maxPolyVoices> candUsed {};
            for (auto& ct : polyCleanTracks)
            {
                if (!ct.active)
                    continue;
                int bestC = -1;
                float bestDist = 0.85f;
                for (int c = 0; c < foundCount; ++c)
                {
                    if (!candUsed[(size_t)c] && std::abs(found[(size_t)c].midi - ct.smoothedMidi) < bestDist)
                    {
                        bestDist = std::abs(found[(size_t)c].midi - ct.smoothedMidi);
                        bestC = c;
                    }
                }
                if (bestC >= 0)
                {
                    candUsed[(size_t)bestC] = true;
                    ct.smoothedMidi = 0.72f * ct.smoothedMidi + 0.28f * found[(size_t)bestC].midi;
                    ct.salience     = 0.60f * ct.salience     + 0.40f * found[(size_t)bestC].salience;
                    ct.ageHops      = juce::jmin(1000, ct.ageHops + 1);
                    ct.missHops     = 0;
                }
                else
                {
                    ++ct.missHops;
                    ct.salience *= 0.82f;
                    const int maxHold = (ct.ageHops >= 5) ? 4 : 1;
                    if (ct.missHops > maxHold)
                        ct = PolyCleanTrack{};
                }
            }

            // Spawn candidate tracks for unmatched voices
            for (int c = 0; c < foundCount; ++c)
            {
                if (candUsed[(size_t)c])
                    continue;
                for (auto& ct : polyCleanTracks)
                {
                    if (!ct.active)
                    {
                        ct.active       = true;
                        ct.smoothedMidi = found[(size_t)c].midi;
                        ct.salience     = found[(size_t)c].salience;
                        ct.ageHops      = 1;
                        ct.missHops     = 0;
                        break;
                    }
                }
            }

            // Emit only confirmed tracks (`ageHops >= 2`) sorted by salience (up to 5 clean instrument voices)
            foundCount = 0;
            for (const auto& ct : polyCleanTracks)
            {
                if (ct.active && ct.ageHops >= 2 && foundCount < 5)
                    found[(size_t)foundCount++] = { ct.smoothedMidi, ct.salience };
            }
        }

        std::sort(found.begin(), found.begin() + foundCount,
                  [](const VoiceCand& a, const VoiceCand& b) { return a.midi < b.midi; });

        prevNumPolyVoices = foundCount;
        for (int i = 0; i < maxPolyVoices; ++i)
            prevPolyRawMidi[(size_t)i] = (i < foundCount) ? found[(size_t)i].midi : 0.0f;

        numActivePolyVoices = foundCount;
        float salSum = 0.0f;
        for (int i = 0; i < foundCount; ++i)
            salSum += found[(size_t)i].salience;
        salSum = juce::jmax(1.0e-5f, salSum);

        const float hDrive = juce::jlimit(0.0f, 2.0f, smoothedHumanizeGate);
        const float hGate  = juce::jlimit(0.0f, 1.0f, hDrive);
        const float effCenter = smoothedCenterAmount * (1.0f - 0.04f * hDrive);
        const float effVar    = juce::jmax(smoothedVariationAmount, 0.35f * hDrive);

        for (auto& vs : polyShifters)
            vs.slotActive = false;

        std::array<bool, maxPolyVoices> slotClaimed {};

        for (int i = 0; i < maxPolyVoices; ++i)
        {
            if (i >= foundCount)
            {
                polyRawMidi[(size_t)i]     = 0.0f;
                polyDesiredMidi[(size_t)i] = 0.0f;
                polyVoiceRms[(size_t)i]    = 0.0f;
                continue;
            }

            const float rawM = found[(size_t)i].midi;
            polyRawMidi[(size_t)i]  = rawM;
            polyVoiceRms[(size_t)i] = frameRms * (found[(size_t)i].salience / salSum);

            // Match to the persistent PolyVoiceShifter slot with the closest pitch (within 2.2 st)
            int bestSlot = -1;
            float bestSlotDist = 2.2f;
            for (int s = 0; s < maxPolyVoices; ++s)
            {
                if (!slotClaimed[(size_t)s] && polyShifters[(size_t)s].slotMidi > 0.0f)
                {
                    const float d = std::abs(rawM - polyShifters[(size_t)s].slotMidi);
                    if (d < bestSlotDist)
                    {
                        bestSlotDist = d;
                        bestSlot = s;
                    }
                }
            }
            bool wasIdle = false;
            if (bestSlot < 0)
            {
                wasIdle = true;
                // Pick the most idle shifter slot
                float minAct = 999.0f;
                for (int s = 0; s < maxPolyVoices; ++s)
                {
                    if (!slotClaimed[(size_t)s] && polyShifters[(size_t)s].activeLevel < minAct)
                    {
                        minAct = polyShifters[(size_t)s].activeLevel;
                        bestSlot = s;
                    }
                }
            }
            if (bestSlot < 0)
                bestSlot = i;

            slotClaimed[(size_t)bestSlot] = true;
            auto& vs = polyShifters[(size_t)bestSlot];

            if (wasIdle || polySmoothedCentre[(size_t)bestSlot] <= 0.0f || std::abs(rawM - polySmoothedCentre[(size_t)bestSlot]) > 1.4f)
                polySmoothedCentre[(size_t)bestSlot] = rawM;
            else
                polySmoothedCentre[(size_t)bestSlot] += vibratoCentreAlpha * (rawM - polySmoothedCentre[(size_t)bestSlot]);

            float targetM = static_cast<float>(snapMidiToMask(polySmoothedCentre[(size_t)bestSlot], effPolyMask));
            float voiceCenterMult = 1.0f;
            float voiceVarMult    = 1.0f;
            float voiceGainLin    = 1.0f;

            for (const auto& pov : polyOverrides)
            {
                if (pov.active && std::abs(pov.detectedRef - rawM) < 1.4f)
                {
                    targetM         = pov.targetMidi;
                    voiceCenterMult = pov.noteCenter;
                    voiceVarMult    = pov.noteVariation;
                    voiceGainLin    = juce::Decibels::decibelsToGain(juce::jlimit(-18.0f, 12.0f, pov.gainDb));
                    break;
                }
            }

            const float vCentre = polySmoothedCentre[(size_t)bestSlot];
            const float dev     = juce::jlimit(-0.25f, 0.25f, (rawM - vCentre) * 0.45f);
            const float pulledCentre = vCentre + juce::jlimit(0.0f, 1.0f, effCenter * voiceCenterMult) * (targetM - vCentre);
            const float polyPull = pulledCentre - vCentre;
            const float polyIntentSlope = 1.0f + hGate * (juce::jlimit(0.65f, 1.45f, std::exp2(polyPull / 30.0f) * ((dev * polyPull < 0.0f) ? 0.82f : 1.12f)) - 1.0f);
            const float desiredM = pulledCentre + juce::jlimit(0.0f, 2.0f, effVar * voiceVarMult) * polyIntentSlope * dev;
            polyDesiredMidi[(size_t)i]  = desiredM;
            polyVoiceGainLin[(size_t)i] = voiceGainLin;

            const float diffSemis = juce::jlimit(-12.0f, 12.0f, desiredM - rawM);
            vs.slotMidi    = rawM;
            vs.targetRatio = std::exp2(diffSemis / 12.0f);
            vs.targetGain  = voiceGainLin;
            vs.slotActive  = !currentlySibilant;
            vs.setTargetBandpass(Quantizer::midiToHz(rawM), wasIdle && vs.activeLevel < 1.0e-3f);
        }
    }

    int snapMidiToMask(float midiNote, uint16_t mask) const noexcept
    {
        if (mask == 0)
            mask = activeScaleMask;
        const int base = static_cast<int>(std::floor(midiNote)) - 6;
        int bestNote = static_cast<int>(std::lround(midiNote));
        float bestDist = 1000.0f;
        for (int m = base; m <= base + 12; ++m)
        {
            const int pc = ((m % 12) + 12) % 12;
            if ((mask & static_cast<uint16_t>(1u << pc)) == 0)
                continue;
            const float d = std::abs(midiNote - static_cast<float>(m));
            if (d < bestDist)
            {
                bestDist = d;
                bestNote = m;
            }
        }
        return bestNote;
    }

    int snapMidiToActiveScale(float midiNote) const noexcept
    {
        return snapMidiToMask(midiNote, activeScaleMask);
    }

    void processPolyphonicSubBlock(juce::dsp::AudioBlock<float>& sub, float /*baseCorrectionSemitones*/) noexcept
    {
        const int numCh  = static_cast<int>(sub.getNumChannels());
        const int numSmp = static_cast<int>(sub.getNumSamples());
        if (numCh <= 0 || numSmp <= 0)
            return;

        bool anySlotRunning = false;
        for (int s = 0; s < maxPolyVoices; ++s)
        {
            if (polyShifters[(size_t)s].slotActive || polyShifters[(size_t)s].activeLevel > 1.0e-4f)
            {
                anySlotRunning = true;
                break;
            }
        }
        if (!anySlotRunning)
            return;

        for (int i = 0; i < numSmp; ++i)
        {
            float chBuf[2] = {
                sub.getSample(0, i),
                numCh > 1 ? sub.getSample(1, i) : sub.getSample(0, i)
            };

            for (int s = 0; s < maxPolyVoices; ++s)
            {
                if (polyShifters[(size_t)s].slotActive || polyShifters[(size_t)s].activeLevel > 1.0e-4f)
                    polyShifters[(size_t)s].processSample(numCh, chBuf);
            }

            sub.setSample(0, i, chBuf[0]);
            if (numCh > 1)
                sub.setSample(1, i, chBuf[1]);
        }
    }

    struct PolyCleanTrack
    {
        bool  active       = false;
        float smoothedMidi = 0.0f;
        float salience     = 0.0f;
        int   ageHops      = 0;
        int   missHops     = 0;
    };

    bool polyphonicEnabled     = false;
    bool midiCleanModeEnabled  = false;
    int  numActivePolyVoices   = 0;
    std::array<PolyCleanTrack, maxPolyVoices> polyCleanTracks {};
    int  polyWindowSize      = 0;
    int  polyHistoryWritePos = 0;
    std::vector<float> polyHistoryRing;
    int  polyFftOrder        = 0;
    int  polyFftSize         = 0;
    std::optional<juce::dsp::FFT> polyFft;
    std::vector<float> polyFftBuffer;
    std::vector<float> polyMagSpectrum;
    std::vector<float> polyHannWindow;

    int   prevNumPolyVoices = 0;
    float prevPolyFrameRms  = 0.0f;
    std::array<float, maxPolyVoices> prevPolyRawMidi    {};
    std::array<float, maxPolyVoices> polyRawMidi        {};
    std::array<float, maxPolyVoices> polyDesiredMidi    {};
    std::array<float, maxPolyVoices> polyVoiceRms       {};
    std::array<float, maxPolyVoices> polySmoothedCentre {};
    std::array<float, maxPolyVoices> polyVoiceGainLin   {};
    std::array<PolyVoiceOverride, maxPolyVoices> polyOverrides {};
    std::array<PolyVoiceShifter, maxPolyVoices>  polyShifters  {};
};