// File: Source/DSP/ShiftGlide.h
#pragma once
#include <algorithm>
#include <cmath>

/**
    Auto-glide for moved notes, in the SHIFT domain.

    A moved note is shifted by a constant number of semitones (new centre - sung centre), so
    everything the singer did inside the note (scoop, drift, vibrato) stays. Where the note
    touches a neighbour, the shift glides (raised cosine) from the neighbour's shift to its
    own, so the singer's own slide between the two notes is stretched or squeezed to land on
    the new pitch instead of being flattened and redrawn. After a gap (breath, silence) the
    note simply starts already shifted.

    Pure C++, no JUCE: the piano roll builds the per-hop curve with it, and the bench tests it.
*/
namespace ShiftGlide
{
    struct Neighbour
    {
        bool  touching = false; ///< voiced right up to the boundary (no gap)
        bool  edited   = false; ///< it has its own shift curve, so the glide is shared half and half
        float shift    = 0.0f;  ///< its shift in semitones (0 for an unedited note)
    };

    /** Shift (semitones) at hop `relHop` (0-based) of a note `span` hops long.
        rampIn / rampOut are the note's ramp handles (fraction of the note), capped at maxGlideHops. */
    inline float shiftAtHop(float ownShift, int relHop, int span, float rampIn, float rampOut,
                            Neighbour prev, Neighbour next, float maxGlideHops) noexcept
    {
        const float pi = 3.14159265358979f;
        auto side = [&](Neighbour nb, float hopsFromEdge, float ramp) noexcept -> float
        {
            if (! nb.touching || std::abs(nb.shift - ownShift) < 1.0e-4f)
                return ownShift;
            // An edited neighbour glides its own half, so this note only does the second half
            // of the S-curve; next to an unedited note the whole glide happens inside this one.
            const float len = std::min(maxGlideHops, ramp * static_cast<float>(span)) * (nb.edited ? 0.5f : 1.0f);
            if (len < 0.5f || hopsFromEdge >= len)
                return ownShift;
            const float u = (hopsFromEdge + 0.5f) / len;                 // 0 at the boundary, 1 inside
            const float phase = nb.edited ? 0.5f + 0.5f * u : u;
            return nb.shift + (ownShift - nb.shift) * (0.5f - 0.5f * std::cos(pi * phase));
        };

        const float in  = side(prev, static_cast<float>(relHop), rampIn);
        const float out = side(next, static_cast<float>(span - 1 - relHop), rampOut);
        // Short note with both glides overlapping: take whichever is further from its own shift.
        return std::abs(in - ownShift) > std::abs(out - ownShift) ? in : out;
    }
}
