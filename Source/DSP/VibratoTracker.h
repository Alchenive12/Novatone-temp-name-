#pragma once
#include <array>
#include <cmath>
#include <algorithm>

/**
    Finds vibrato in the detected pitch curve, one value per analysis hop, and
    reports the note it is centred on.

    A slow follower (tau 150 ms) gives the rough centre; each time the pitch
    crosses it, the time since the last crossing is a half cycle. Two half
    cycles in a row inside the 4-12 Hz vibrato range, each swinging at least
    12 cents, mark vibrato as active. While it is, centre() is the mean pitch
    over exactly the last full cycle, which cancels the oscillation instead of
    just damping it (a one-pole would still wobble about +-20 cents on a
    100-cent vibrato), and depth() is the recent peak swing.

    Plain arithmetic on a fixed ring; no allocation. Clean-room: built from the
    textbook description of vibrato (periodic 4-8 Hz pitch modulation), not
    from any product.
*/
class VibratoTracker
{
public:
    void prepare(float hopSecondsIn) noexcept
    {
        hopSeconds = hopSecondsIn;
        slowAlpha = 1.0f - std::exp(-hopSeconds / 0.150f);
        reset();
    }

    void reset() noexcept
    {
        count = 0;
        writePos = 0;
        active = false;
        goodHalves = 0;
        lastSign = 0;
        hopsSinceCross = 0;
        peakThisHalf = 0.0f;
        lastHalfHops = 0;
        depthSemitones = 0.0f;
    }

    /** A new note starts at this pitch: forget the previous note's vibrato. */
    void startNote(float midi) noexcept
    {
        reset();
        slowCentre = midi;
        push(midi);
    }

    void update(float midi) noexcept
    {
        // A move well past any vibrato is a new note: start over. A quantizer
        // flip in the middle of a vibrato is not, so the caller doesn't reset us.
        if (std::abs(midi - slowCentre) > newNoteSemitones)
        {
            startNote(midi);
            return;
        }
        push(midi);
        slowCentre += slowAlpha * (midi - slowCentre);

        const float d = midi - slowCentre;
        const int sign = d > signDeadband ? 1 : (d < -signDeadband ? -1 : 0);
        ++hopsSinceCross;
        peakThisHalf = std::max(peakThisHalf, std::abs(d));

        if (sign != 0 && sign != lastSign)
        {
            if (lastSign != 0)
            {
                const float half = static_cast<float> (hopsSinceCross) * hopSeconds;
                const bool ok = half >= minHalfSeconds && half <= maxHalfSeconds && peakThisHalf >= minSwingSemitones;
                goodHalves = ok ? goodHalves + 1 : 0;
                if (ok)
                {
                    lastHalfHops = hopsSinceCross;
                    depthSemitones = depthSemitones > 0.0f ? 0.5f * (depthSemitones + peakThisHalf) : peakThisHalf;
                }
            }
            lastSign = sign;
            hopsSinceCross = 0;
            peakThisHalf = 0.0f;
        }

        if (static_cast<float> (hopsSinceCross) * hopSeconds > maxHalfSeconds * 1.5f)
            goodHalves = 0; // the oscillation stopped

        active = goodHalves >= 2;
    }

    bool isActive() const noexcept { return active; }

    /** Peak swing either side of the centre, in semitones (valid while active). */
    float depth() const noexcept { return depthSemitones; }

    /** Mean pitch over the last full vibrato cycle while active; the slow
        follower otherwise. */
    float centre() const noexcept
    {
        if (! active || lastHalfHops == 0)
            return slowCentre;
        const int n = std::clamp(2 * lastHalfHops, 2, std::min(count, ringSize));
        float sum = 0.0f;
        for (int i = 1; i <= n; ++i)
            sum += ring[static_cast<size_t> ((writePos - i + ringSize) % ringSize)];
        return sum / static_cast<float> (n);
    }

private:
    void push(float midi) noexcept
    {
        ring[static_cast<size_t> (writePos)] = midi;
        writePos = (writePos + 1) % ringSize;
        count = std::min(count + 1, ringSize);
    }

    static constexpr int   ringSize = 64;            // ~0.7 s at a 11.6 ms hop
    static constexpr float minHalfSeconds = 0.040f;   // 12.5 Hz
    static constexpr float maxHalfSeconds = 0.125f;   // 4 Hz
    static constexpr float minSwingSemitones = 0.12f;
    static constexpr float signDeadband = 0.03f;
    static constexpr float newNoteSemitones = 1.6f;   // past the widest vibrato (~1.2 st) plus margin

    float hopSeconds = 0.0116f;
    float slowAlpha = 0.08f;
    float slowCentre = 0.0f;

    std::array<float, ringSize> ring {};
    int count = 0, writePos = 0;

    bool  active = false;
    int   goodHalves = 0;
    int   lastSign = 0;
    int   hopsSinceCross = 0;
    float peakThisHalf = 0.0f;
    int   lastHalfHops = 0;
    float depthSemitones = 0.0f;
};

