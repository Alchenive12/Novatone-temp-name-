#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <random>
#include "../DSP/MPMDetector.h"
#include "../DSP/SignalsmithShifter.h"
#include "ChordTracker.h"

/**
    Chord-aware harmony generator for the Harmony tab.

    The lead vocal (main input) is pitch-tracked (MPM). Each harmony voice picks a target note
    from the current chord tones (detected from the sidechain by ChordTracker) or, without a
    sidechain, from the Tune tab's key & scale, relative to the note the lead is singing
    (e.g. "next chord tone above"). The voice is a formant-preserving shift of the lead
    (Signalsmith) that keeps the lead's own vibrato and expression around the target note.

    Timing: pitch analysis runs ahead of a short pre-delay so every shift is computed for the
    exact audio being shifted; a post-delay pads the voices to the plugin's reported latency so
    harmonies line up with the (latency-compensated) lead.
*/
class HarmonyEngine
{
public:
    static constexpr int numVoices = 8;  ///< 4 shown by default, more via the + button

    // New modes are appended so saved sessions keep their choices.
    enum Mode { chordAbove = 0, chordAbove2, chordBelow, chordBelow2, octaveUp, octaveDown, chordRoot, unison,
                maj7Colour, maj9Colour, sus2Voice, sus4Voice, randomInKey, numModes };
    static juce::StringArray modeNames()
    {
        return { "Chord tone above", "2 chord tones above", "Chord tone below", "2 chord tones below",
                 "Octave above", "Octave below", "Chord root (below)", "Unison double",
                 "Maj7 colour", "Maj9 colour", "Sus2", "Sus4", "Random (in key)" };
    }

    struct VoiceSettings
    {
        bool  on = false;
        int   mode = chordAbove;
        float levelDb = -6.0f;
        float pan = 0.0f;          ///< -1 .. +1
        float formant = 0.0f;      ///< semitones, 0 = keep the singer's formants
    };

    void prepare (double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        detWin = juce::nextPowerOfTwo (juce::roundToInt (sampleRate * 0.04));
        detHop = detWin / 8;
        detector.prepare (sampleRate, detWin);
        detRing.assign ((size_t) detWin, 0.0f);
        frame.assign ((size_t) detWin, 0.0f);

        const juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) maxBlockSize, 1 };
        for (auto& v : voices)
        {
            v.shifter.prepare (monoSpec);
            v.buffer.setSize (1, maxBlockSize);
            v.humanDelay.assign ((size_t) juce::nextPowerOfTwo ((int) (sampleRate * 0.05)), 0.0f);
            v.out.assign ((size_t) maxBlockSize, 0.0f);
        }
        shifterLatency = voices[0].shifter.getLatencySamples();

        preDelaySamples = detWin / 2 + detHop;
        const int maxDelay = juce::nextPowerOfTwo ((int) (sampleRate * 1.5));
        preRing.assign ((size_t) maxDelay, 0.0f);
        postRing[0].assign ((size_t) maxDelay, 0.0f);
        postRing[1].assign ((size_t) maxDelay, 0.0f);
        for (auto& v : voices) v.postRing.assign ((size_t) maxDelay, 0.0f);
        leadRing.assign ((size_t) maxDelay, 0.0f);
        leadOut.assign ((size_t) maxBlockSize, 0.0f);
        mixL.assign ((size_t) maxBlockSize, 0.0f);
        mixR.assign ((size_t) maxBlockSize, 0.0f);
        monoIn.assign ((size_t) maxBlockSize, 0.0f);
        chords.prepare (sampleRate);

        std::mt19937 seedRng (7);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        for (auto& v : voices) { v.humanSeed = u (seedRng); v.driftPhase = u (seedRng) * 6.28f; }
        reset();
    }

    void reset()
    {
        std::fill (detRing.begin(), detRing.end(), 0.0f);
        std::fill (preRing.begin(), preRing.end(), 0.0f);
        for (auto& r : postRing) std::fill (r.begin(), r.end(), 0.0f);
        std::fill (leadRing.begin(), leadRing.end(), 0.0f);
        for (auto& v : voices)
        {
            std::fill (v.postRing.begin(), v.postRing.end(), 0.0f);
            v.shifter.reset();
            std::fill (v.humanDelay.begin(), v.humanDelay.end(), 0.0f);
            v.shift = v.shiftTarget = 0.0f;
            v.targetNote = -1;
        }
        detPos = preWrite = postWrite = sinceHop = 0;
        leadMidi = 0.0f; leadSlow = 0.0f; voiced = false; lastLeadNote = -1;
        chords.reset();
    }

    //==========================================================================
    // Parameters (call from the audio thread before process)
    void setVoice (int i, const VoiceSettings& s) noexcept { voices[(size_t) i].settings = s; }
    void setHumanize (float amount) noexcept { humanize = juce::jlimit (0.0f, 1.0f, amount); }
    void setHarmonyGainDb (float db) noexcept { harmonyGain = db <= -59.0f ? 0.0f : juce::Decibels::decibelsToGain (db); }
    void setLeadGainDb (float db) noexcept { leadGain = db <= -59.0f ? 0.0f : juce::Decibels::decibelsToGain (db); }
    void setUseSidechainChords (bool b) noexcept { useSidechain = b; }
    void setScaleMask (uint16_t m) noexcept { scaleMask = m == 0 ? 0x0FFF : m; }
    void setKeyRoot (int pc) noexcept { keyRoot = ((pc % 12) + 12) % 12; }
    void setTotalLatency (int samples) noexcept { totalLatency = samples; }
    /** MIDI mode: voices sing the held notes (lowest note -> first voice on), not intervals from the lead.
        Nothing held = harmonies fade out. Notes beyond the voices are dropped; voices beyond the notes double an octave up. */
    void setMidiMode (bool b) noexcept { midiMode = b; }
    void setMidiNotes (const bool* held128) noexcept
    {
        midiCount = 0;
        for (int k = 0; k < 128 && midiCount < numVoices; ++k) if (held128[k]) midiHeld[(size_t) midiCount++] = k;
    }

    bool anyVoiceOn() const noexcept
    {
        for (auto& v : voices) if (v.settings.on) return true;
        return false;
    }

    /** Sidechain (beat / instrumental) for chord detection. */
    void processSidechain (const juce::AudioBuffer<float>& side) noexcept
    {
        const int n = juce::jmin (side.getNumSamples(), (int) monoIn.size());
        const int ch = side.getNumChannels();
        if (ch <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < ch; ++c) s += side.getSample (c, i);
            monoIn[(size_t) i] = s / (float) ch;
        }
        chords.process (monoIn.data(), n);
        lastSidechainMs = juce::Time::getMillisecondCounter();
    }

    /** `dryInput` = the lead before tuning (post input gain). `io` = the processed lead, which gets
        lead gain applied and the harmonies added. */
    void process (const juce::AudioBuffer<float>& dryInput, juce::AudioBuffer<float>& io) noexcept
    {
        const int n = juce::jmin (io.getNumSamples(), (int) mixL.size());
        const int inCh = juce::jmax (1, dryInput.getNumChannels());
        const int postDelay = juce::jlimit (0, (int) postRing[0].size() - 1, totalLatency - preDelaySamples - shifterLatency);
        const uint16_t chordTones = currentToneMask();

        // 1. Mono lead -> detector ring + pre-delay
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            for (int c = 0; c < inCh; ++c) s += dryInput.getSample (c, i);
            s /= (float) inCh;
            detRing[(size_t) detPos] = s;
            detPos = (detPos + 1) % detWin;
            if (++sinceHop >= detHop) { sinceHop = 0; analyseHop (chordTones); }

            preRing[(size_t) preWrite] = s;
            const int mask = (int) preRing.size() - 1;
            monoIn[(size_t) i] = preRing[(size_t) ((preWrite - preDelaySamples) & mask)];
            preWrite = (preWrite + 1) & mask;
        }

        // 2. Voices
        std::fill (mixL.begin(), mixL.begin() + n, 0.0f);
        std::fill (mixR.begin(), mixR.begin() + n, 0.0f);
        const float blockSec = (float) n / (float) sr;
        const float gate0 = midiGate;   // MIDI mode: ~15 ms fade in/out with the held notes
        midiGate = (midiMode && midiCount == 0) ? 0.0f : 1.0f;
        midiGate = gate0 + (1.0f - std::exp (-blockSec / 0.015f)) * (midiGate - gate0);
        for (auto& v : voices)
        {
            if (! v.settings.on) { v.active = false; std::fill (v.out.begin(), v.out.begin() + n, 0.0f); continue; }
            if (! v.active) { v.shifter.reset(); v.shift = v.shiftTarget; v.active = true; }

            // glide ~25 ms between notes, plus slow humanize drift (+-8 cents at 100%)
            const float coeff = 1.0f - std::exp (-blockSec / 0.025f);
            v.shift += coeff * (v.shiftTarget - v.shift);
            v.driftPhase += juce::MathConstants<float>::twoPi * (0.21f + 0.17f * v.humanSeed) * blockSec;
            const float drift = humanize * 0.08f * std::sin (v.driftPhase);
            v.shifter.setPitchSemitones (juce::jlimit (-24.0f, 24.0f, v.shift + drift));
            v.shifter.setFormantSemitones (v.settings.formant);

            v.buffer.copyFrom (0, 0, monoIn.data(), n);
            juce::dsp::AudioBlock<float> blk (v.buffer);
            auto sub = blk.getSubBlock (0, (size_t) n);
            v.shifter.process (juce::dsp::ProcessContextReplacing<float> (sub));

            const float g = juce::Decibels::decibelsToGain (v.settings.levelDb) * harmonyGain * midiGate;
            const float p = juce::jlimit (-1.0f, 1.0f, v.settings.pan);
            const float gl = g * std::cos ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            const float gr = g * std::sin ((p + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
            // humanize: each voice sits 0..22 ms late (a different singer's timing)
            const int hMask = (int) v.humanDelay.size() - 1;
            const int hd = juce::roundToInt (humanize * (0.006f + 0.016f * v.humanSeed) * (float) sr);
            const float* src = v.buffer.getReadPointer (0);
            for (int i = 0; i < n; ++i)
            {
                v.humanDelay[(size_t) v.humanPos] = src[i];
                const float s = v.humanDelay[(size_t) ((v.humanPos - hd) & hMask)];
                v.humanPos = (v.humanPos + 1) & hMask;
                v.out[(size_t) i] = s;
                mixL[(size_t) i] += gl * s;
                mixR[(size_t) i] += gr * s;
            }
        }

        // 3. Post delay to the plugin latency, then mix with the (scaled) lead
        const int pMask = (int) postRing[0].size() - 1;
        // stems for Choir: each voice (before level / pan) and the lead, on the same latency as the output
        const int leadDelay = juce::jlimit (0, pMask, postDelay + shifterLatency);
        for (int i = 0; i < n; ++i)
        {
            const int w = (postWrite + i) & pMask;
            leadRing[(size_t) w] = monoIn[(size_t) i];
            leadOut[(size_t) i] = leadRing[(size_t) ((w - leadDelay) & pMask)];
            for (auto& v : voices)
            {
                v.postRing[(size_t) w] = v.out[(size_t) i];
                v.out[(size_t) i] = v.postRing[(size_t) ((w - postDelay) & pMask)];
            }
        }
        for (int i = 0; i < n; ++i)
        {
            postRing[0][(size_t) postWrite] = mixL[(size_t) i];
            postRing[1][(size_t) postWrite] = mixR[(size_t) i];
            const int r = (postWrite - postDelay) & pMask;
            mixL[(size_t) i] = postRing[0][(size_t) r];
            mixR[(size_t) i] = postRing[1][(size_t) r];
            postWrite = (postWrite + 1) & pMask;
        }
        for (int c = 0; c < io.getNumChannels(); ++c)
        {
            float* d = io.getWritePointer (c);
            const auto& h = (io.getNumChannels() == 1) ? mixL : (c == 0 ? mixL : mixR);
            for (int i = 0; i < n; ++i)
                d[i] = d[i] * leadGain + (io.getNumChannels() == 1 ? 0.5f * (mixL[(size_t) i] + mixR[(size_t) i]) : h[(size_t) i]);
        }
    }

    /** Harmony voices only (no lead), latency-aligned, for the last process() block. */
    const float* getHarmonyOutL() const noexcept { return mixL.data(); }
    const float* getHarmonyOutR() const noexcept { return mixR.data(); }
    /** Choir stems for the last block: the lead (mono, dry) and each voice (mono, before level / pan). */
    const float* getLeadStem() const noexcept { return leadOut.data(); }
    const float* getVoiceStem (int i) const noexcept { return voices[(size_t) i].out.data(); }
    int getVoiceMode (int i) const noexcept { return voices[(size_t) i].settings.mode; }
    bool isMidiMode() const noexcept { return midiMode; }

    //==========================================================================
    // GUI readouts (lock-free)
    int   getChord() const noexcept { return chords.getChord(); }
    float getSidechainLevel() const noexcept { return chords.getLevel(); }
    bool  isSidechainActive() const noexcept { return juce::Time::getMillisecondCounter() - lastSidechainMs.load() < 500 && chords.getLevel() > 1.0e-4f; }
    float getChroma (int pc) const noexcept { return chords.getChroma (pc); }
    float getLeadMidi() const noexcept { return publishedLead.load (std::memory_order_relaxed); }
    int   getVoiceNote (int i) const noexcept { return voices[(size_t) i].publishedNote.load (std::memory_order_relaxed); }
    int   getLatencyShortfall() const noexcept { return juce::jmax (0, preDelaySamples + shifterLatency - totalLatency); }

private:
    struct Voice
    {
        VoiceSettings settings;
        SignalsmithShifter shifter;
        juce::AudioBuffer<float> buffer;
        std::vector<float> humanDelay, postRing, out;
        int humanPos = 0;
        float shift = 0.0f, shiftTarget = 0.0f;
        float humanSeed = 0.5f, driftPhase = 0.0f;
        int targetNote = -1;
        int randomFor = -1000, randomTarget = 0; ///< Random mode: note picked for this lead note
        bool active = false;
        std::atomic<int> publishedNote { -1 };
    };

    uint16_t currentToneMask() const noexcept
    {
        if (useSidechain)
        {
            const int c = chords.getChord();
            if (c >= 0) return ChordTracker::chordMask (c / ChordTracker::numQualities, c % ChordTracker::numQualities);
        }
        return scaleMask;
    }

    /** True when harmonies follow the key's scale (no sidechain chord). */
    bool scaleOnly() const noexcept { return ! (useSidechain && chords.getChord() >= 0); }

    static int toneAbove (int note, uint16_t mask, int count) noexcept
    {
        int n = note;
        for (int k = 0; k < count; ++k)
        {
            int m = n + 1;
            for (int s = 0; s < 24 && ! ((mask >> (((m % 12) + 12) % 12)) & 1u); ++s) ++m;
            n = m;
        }
        return n;
    }

    static int toneBelow (int note, uint16_t mask, int count) noexcept
    {
        int n = note;
        for (int k = 0; k < count; ++k)
        {
            int m = n - 1;
            for (int s = 0; s < 24 && ! ((mask >> (((m % 12) + 12) % 12)) & 1u); ++s) --m;
            n = m;
        }
        return n;
    }

    /** Root of the current chord (sidechain) or key, and whether it's a minor-type chord. */
    void currentRootQuality (uint16_t tones, int& root, bool& minorish) const noexcept
    {
        const int c = useSidechain ? chords.getChord() : -1;
        if (c >= 0)
        {
            root = c / ChordTracker::numQualities;
            const int q = c % ChordTracker::numQualities;
            minorish = q == ChordTracker::minor || q == ChordTracker::min7 || q == ChordTracker::dim || q == ChordTracker::dom7;
            return;
        }
        root = keyRoot;
        minorish = ((tones >> ((root + 3) % 12)) & 1u) && ! ((tones >> ((root + 4) % 12)) & 1u);
    }

    /** Nearest note strictly above `note` with pitch class `pc`. */
    static int pcAbove (int note, int pc) noexcept
    {
        int t = note + 1;
        while (((t % 12) + 12) % 12 != ((pc % 12) + 12) % 12) ++t;
        return t;
    }

    int targetFor (int mode, int leadNote, uint16_t tones) const noexcept
    {
        switch (mode)
        {
            case maj7Colour:
            case maj9Colour:
            case sus2Voice:
            case sus4Voice:
            {
                int root = 0; bool minorish = false;
                currentRootQuality (tones, root, minorish);
                if (mode == maj7Colour) return pcAbove (leadNote, root + (minorish ? 10 : 11)); // maj7, or b7 over minor chords
                if (mode == maj9Colour) return pcAbove (leadNote + 2, root + 2);  // the 9th, kept at least a 3rd above
                return toneAbove (leadNote, ChordTracker::chordMask (root, mode == sus2Voice ? ChordTracker::sus2 : ChordTracker::sus4), 1);
            }
            // With a chord, tones = chord tones (next one up = a 3rd/4th away). Without one, tones = the whole
            // scale, so step 2 scale notes per chord tone: a diatonic 3rd, and a 5th for the second tone.
            case chordAbove:  return toneAbove (leadNote, tones, scaleOnly() ? 2 : 1);
            case chordAbove2: return toneAbove (leadNote, tones, scaleOnly() ? 4 : 2);
            case chordBelow:  return toneBelow (leadNote, tones, scaleOnly() ? 2 : 1);
            case chordBelow2: return toneBelow (leadNote, tones, scaleOnly() ? 4 : 2);
            case octaveUp:    return leadNote + 12;
            case octaveDown:  return leadNote - 12;
            case chordRoot:
            {
                int root = 0;
                if (useSidechain && chords.getChord() >= 0) root = chords.getChord() / ChordTracker::numQualities;
                else { for (int pc = 0; pc < 12; ++pc) if ((tones >> pc) & 1u) { root = pc; break; } }
                int t = leadNote - 1;
                while (((t % 12) + 12) % 12 != root) --t;
                return t;
            }
            case unison:
            default: return leadNote;
        }
    }

    void analyseHop (uint16_t tones) noexcept
    {
        for (int i = 0; i < detWin; ++i)
            frame[(size_t) i] = detRing[(size_t) ((detPos + i) % detWin)];
        const auto r = detector.processFrame (frame.data());
        voiced = r.isVoiced && r.pitchHz > 60.0f && r.pitchHz < 1400.0f;
        if (! voiced) return; // keep the last shifts through consonants and breaths

        leadMidi = 69.0f + 12.0f * std::log2 (r.pitchHz / 440.0f);
        // slow centre (~120 ms) keeps vibrato in the harmony while choosing a stable note
        const float a = 1.0f - std::exp (-(float) detHop / ((float) sr * 0.12f));
        leadSlow = (lastLeadNote < 0 || std::abs (leadMidi - leadSlow) > 1.2f) ? leadMidi : leadSlow + a * (leadMidi - leadSlow);
        publishedLead.store (leadMidi, std::memory_order_relaxed);

        int leadNote = (int) std::lround (leadSlow);
        if (lastLeadNote >= 0 && std::abs (leadSlow - (float) lastLeadNote) < 0.62f) leadNote = lastLeadNote; // hysteresis
        lastLeadNote = leadNote;

        int onIndex = 0;
        for (auto& v : voices)
        {
            if (! v.settings.on) { v.publishedNote.store (-1); continue; }
            int t;
            if (midiMode)
            {
                if (midiCount == 0) { ++onIndex; continue; }   // keep the last note while fading out
                t = midiHeld[(size_t) (onIndex % midiCount)] + 12 * (onIndex / midiCount);
                ++onIndex;
            }
            else if (v.settings.mode == randomInKey)
            {
                // a new random in-key note (a 3rd .. a 6th ABOVE the lead) for every new lead note;
                // never below the lead, so it can't become the bottom of the stack and muddy the low end
                if (leadNote != v.randomFor)
                {
                    v.randomFor = leadNote;
                    int cand[24]; int nc = 0;
                    for (int d = 3; d <= 9; ++d)
                        if (((scaleMask >> (((leadNote + d) % 12 + 12) % 12)) & 1u)) cand[nc++] = leadNote + d;
                    rng = rng * 1664525u + 1013904223u;
                    v.randomTarget = nc > 0 ? cand[(rng >> 8) % (juce::uint32) nc] : leadNote + 12;
                }
                t = v.randomTarget;
            }
            else
                t = targetFor (v.settings.mode, leadNote, tones);
            v.targetNote = t;
            v.shiftTarget = (float) t - leadSlow;
            v.publishedNote.store (t, std::memory_order_relaxed);
        }
    }

    double sr = 48000.0;
    int detWin = 2048, detHop = 256, detPos = 0, sinceHop = 0;
    MPMDetector detector;
    std::vector<float> detRing, frame, preRing, monoIn, mixL, mixR;
    std::array<std::vector<float>, 2> postRing;
    std::vector<float> leadRing, leadOut;
    int preWrite = 0, postWrite = 0, preDelaySamples = 0, shifterLatency = 0, totalLatency = 0;
    std::array<Voice, numVoices> voices;
    ChordTracker chords;

    float leadMidi = 0.0f, leadSlow = 0.0f;
    bool voiced = false;
    int lastLeadNote = -1;
    std::atomic<float> publishedLead { 0.0f };
    std::atomic<juce::uint32> lastSidechainMs { 0 };

    float humanize = 0.4f, harmonyGain = 1.0f, leadGain = 1.0f;
    bool useSidechain = true;
    uint16_t scaleMask = 0x0AB5;
    int keyRoot = 0;
    bool midiMode = false;
    int midiCount = 0;
    std::array<int, numVoices> midiHeld {};
    float midiGate = 1.0f;
    juce::uint32 rng = 0x2545F491u;
};
