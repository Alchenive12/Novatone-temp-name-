#pragma once
#include <algorithm>
#include <cmath>

/**
    SYNC macro: one SOFT <-> HARD amount sets both the classic Tune knobs and
    the NewTone-style global knobs (Center, Variation, Transition, Sensitivity)
    together, so a producer gets a musical combination at every point.

      HARD (1): Center 100%, Variation 0% (flat), Transition 100% (fast snap),
                Sensitivity 85%, Speed 1, Humanize 0, Vibrato off.
      SOFT (0): Center 65%, Variation 100% (natural expression), Transition 25%
                (smooth legato), Sensitivity 55%, slow Speed, Humanize up.
*/
struct TuneKnobSettings
{
    float speed;        // tune_flexAmount / tune_transition, 0..1
    float humanize;     // tune_humanizeAmount, 0..1
    float vibratoCents; // tune_vibrato, 0..100
    float center;       // tune_center, 0..1 (NewTone Global Pitch Center)
    float variation;    // tune_variation, 0..2 (NewTone Global Pitch Variation, 1.0 = 100% natural)
    float transition;   // tune_transition, 0..1 (NewTone Global Note Transition speed)
    float sensitivity;  // tune_sensitivity, 0..1 (NewTone Note Segmentation & Lock Sensitivity)
};

namespace TuneMacro
{
    struct Curves
    {
        float softSpeed;        // Speed at SOFT
        float speedCurve;       // >1: Speed rises early. speed = soft + (1-soft)(1 - (1-x)^c)
        float humanizeCurve;    // humanize = (1 - x)^c
        float softVibratoCents; // vibrato = soft * (1 - x)
        float softHumanize = 1.0f; // humanize = softHumanize * (1 - x)^c
    };

    constexpr Curves lookaheadCurves { 0.20f, 1.1f, 1.6f, 45.0f };
    constexpr Curves directCurves    { 0.26f, 1.6f, 2.0f, 45.0f, 0.9f };

    /** Where the plugin starts: the knobs open at SYNC 65% (LOOKAHEAD off)
        with SYNC itself off, the best all-round setting on the bench. */
    constexpr float defaultCharacter = 0.65f;

    /** x: 0 = SOFT, 1 = HARD. */
    inline TuneKnobSettings forCharacter(float x, const Curves& c) noexcept
    {
        x = std::clamp(x, 0.0f, 1.0f);
        const float s = 1.0f - x;
        const float spd = c.softSpeed + (1.0f - c.softSpeed) * (1.0f - std::pow(s, c.speedCurve));
        const float hum = c.softHumanize * std::pow(s, c.humanizeCurve);
        const float vib = std::round(c.softVibratoCents * s);

        // NewTone global macro mappings:
        const float center      = std::clamp(0.56f + 0.44f * std::pow(x, 0.85f), 0.0f, 1.0f);
        const float variation   = std::clamp(1.05f * std::pow(s, 1.15f), 0.0f, 2.0f);
        const float transition  = spd;
        const float sensitivity = std::clamp(0.50f + 0.35f * x, 0.0f, 1.0f);

        return { spd, hum, vib, center, variation, transition, sensitivity };
    }

    inline TuneKnobSettings forCharacter(float x, bool lookahead) noexcept
    {
        return forCharacter(x, lookahead ? lookaheadCurves : directCurves);
    }
}
