#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdint>

/**
    Result of one Quantizer::process() call.

    noteChanged is true exactly on the hop where the internal hysteresis state machine
    (forced boundary crossing, hold-time transition, or first-ever initialisation) moves
    activeNote to a new value - i.e. exactly the "m_target[n] != m_target[n-1]" reset
    condition the Humanize sustain timer needs (blueprint 2.3). It is false on every hop
    where activeNote is left untouched, including unvoiced/invalid input frames.
*/
struct QuantizerResult
{
    float targetHz;
    bool  noteChanged;
};

/**
    Scale-aware pitch quantizer with hysteresis (blueprint sections 1-3): converts a
    continuously-detected pitch into a stable target note from a user-selected musical
    scale, using a dead-zone-plus-hold-time state machine so vibrato doesn't cause the
    target note to flutter, while genuine note changes still snap promptly.

    Usage: call setScale() to choose the allowed pitch classes, prepare() once streaming
    parameters are known, then feed process() one detected frequency per analysis hop; it
    returns the target frequency to hand to SmoothingFilter, plus a noteChanged flag the
    Humanize sustain-timer state machine uses to detect note onsets.

    Heap allocation never occurs in this class (fixed-size state only). process() performs
    no allocation, locking, or I/O and is safe on the audio thread; the setters are cheap
    (a handful of scalar ops) but are intended to be called from the message thread, not
    concurrently with process().
*/
class Quantizer
{
public:
    Quantizer() = default;

    /** Precomputes the hold-time frame count and resets state. Not real-time safe.
        @param newSampleRate  audio sample rate, in Hz.
        @param newHopSize     H: number of audio samples between successive analysis frames.
    */
    void prepare (double newSampleRate, int newHopSize);

    /** Selects the allowed pitch classes. Bit i (0 = C, 1 = C#, ... 11 = B) set means that
        pitch class is in the scale; at least one bit must be set (blueprint 2: 1 <= K <= 12).
        Defaults to chromatic (all 12 bits set) until called. */
    void setScale (uint16_t pitchClassMask) noexcept;

    /** Sets the hysteresis dead-zone margin delta, in cents (blueprint 3.1, typical 10). */
    void setHysteresisMargin (float newMarginCents) noexcept;

    /** Sets the minimum hold time for the secondary (drift) hysteresis branch, in seconds
        (blueprint 3.1, typical 0.04). Recomputes T_hold; requires prepare() to have run
        at least once first. */
    void setHoldTime (float newHoldTimeSeconds) noexcept;

    /** Resets the state machine (the active note is re-initialised from the next valid
        process() call, per blueprint 3.5). Allocation-free; safe on the audio thread. */
    void reset() noexcept;

    /** Feeds one hop's detected frequency through the quantizer.
        @param inputHz  the detected pitch for this hop; pass <= 0 for an unvoiced/invalid
                         frame, in which case the state machine is left untouched, the
                         currently-held target is returned unchanged, and noteChanged is
                         always false.
        @returns { f_target[n], noteChanged } - f_target[n] is the target frequency to feed
                 the smoother (blueprint 3.7); noteChanged is true iff activeNote actually
                 moved to a new value on this call (see QuantizerResult above). */
    QuantizerResult process (float inputHz) noexcept;

    /** The currently-held target note, as a MIDI note number (69 until the first valid
        process() call). */
    int getActiveNote() const noexcept { return activeNote; }

    /** m = 69 + 12 * log2(f / 440)                                            (blueprint 1) */
    static float hzToMidi (float hz) noexcept;

    /** f = 440 * 2^((m - 69) / 12)                                            (blueprint 1) */
    static float midiToHz (float midiNote) noexcept;

    /** Stateless nearest-allowed-note lookup (does not modify hysteresis state). */
    int  findNearestAllowedNote (float continuousMidi) const noexcept; // blueprint 2

private:
    bool isNoteAllowed (int note) const noexcept;
    int  findLowerNeighbor (int note) const noexcept;                  // m_lo, blueprint 3.2
    int  findUpperNeighbor (int note) const noexcept;                  // m_hi, blueprint 3.2

    double sampleRate = 44100.0;
    int    hopSize    = 0; // H

    uint16_t pitchClassMask = 0x0FFF; // default: chromatic (all 12 pitch classes allowed)

    float marginCents     = 10.0f; // delta
    float marginSemitones = 0.1f;  // delta_m = delta / 100
    float holdTimeSeconds = 0.04f; // t_hold
    int   holdFrames      = 1;     // T_hold = ceil(t_hold * fs / H)

    // ---- persistent state (blueprint 3.1, 3.5) ----
    bool  initialized       = false;
    int   activeNote        = 69; // m_active
    int   previousCandidate = 69; // m_cand[n-1]
    int   holdCounter       = 0;  // h[n]

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Quantizer)
};
