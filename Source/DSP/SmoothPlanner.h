#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

/**
    SMOOTH mode: plans the pitch correction with the future in view.

    TuneEngine delays the audio into the shifter by a lookahead (~60 ms), so
    by the time a hop is shifted, the pitch track for the next `lookaheadHops`
    hops is already known. Per analysis hop the engine pushes what it saw (the
    detected pitch and the scale note it maps to); once a hop has its full
    lookahead, it gets a final correction value:

      - Steady part of a note ("core"): target note minus the note's centre,
        a mean over the core hops either side of it (past AND future, so no
        lag). Vibrato, jitter and expression stay; the centre lands exactly
        on the note.
      - Between two cores (a transition, a scoop, a passing note too short to
        settle): a raised-cosine glide from one core's correction to the
        next. The singer's own transition shape is kept, so the effective
        retune speed follows the singer: small steps glide, quick leaps stay
        quick, and nothing ever steps through a wrong note.
      - Start of a phrase: the first note's correction is applied from its
        very first voiced hop, so a scoop keeps its shape and lands in tune.
      - Unvoiced: holds, and pre-sets the next note's correction once it is
        within the lookahead.

    The tune knobs shape the result (setKnobs, every block):
      - Humanize = how much of that natural plan is used. 1 = the plan above
        (what SMOOTH used to be); 0 = full hard correction (target minus the
        detected pitch, every hop).
      - Speed = how sharp the hard part's note changes are. It is a centred
        (zero-phase) average over +-0..8 hops, so even the slowest setting
        moves around the sung note change instead of trailing it, and the
        fastest is a snap that lands on it.
      - Vibrato = how much vibrato (cents) the hard part leaves in, measured
        around the zero-lag note centre.

    Glitch filter: a hop whose pitch jumps away (octave-type) from both
    neighbours for one or two hops, or whose note flickers to another note
    for one or two hops, is repaired from its neighbours before planning.

    Fixed-size rings; nothing allocates. One instance per voice, audio thread
    only. Clean-room design.
*/
class SmoothPlanner
{
public:
    struct Params
    {
        // 60 ms (5 hops at 44.1/48 kHz). Measured: 120 ms is no better; below
        // ~60 ms slow (~300 ms) slides start to land late (in tune 90 -> 84%).
        double lookaheadSeconds = 0.060;
        double centreHalfSeconds = 0.090;  // +-90 ms: one vibrato cycle in total
        double minNoteSeconds = 0.060;     // shorter = passing note
        float  coreToleranceSemitones = 0.45f;
    };
    /** Call before prepare(). */
    void setParams(const Params& p) noexcept { params = p; }

    /** The Tune knobs (audio thread, any time). speed and humanize 0..1;
        vibratoDepth in semitones. */
    void setKnobs(float speed, float humanize, float vibratoDepth) noexcept
    {
        knobSpeed = std::clamp(speed, 0.0f, 1.0f);
        knobHumanize = std::clamp(humanize, 0.0f, 1.0f);
        knobVibrato = std::max(0.0f, vibratoDepth);
    }

    /** Bench switch for the glitch filter (default on). */
    void setGlitchFilter(bool on) noexcept { glitchFilter = on; }

    void prepare(int hopSamplesIn, double sampleRate) noexcept
    {
        hopSamples = hopSamplesIn;
        hopSeconds = static_cast<double> (hopSamples) / sampleRate;
        lookaheadHops = std::clamp(static_cast<int> (std::lround(params.lookaheadSeconds / hopSeconds)), 2, maxLookahead);
        centreHalfHops = std::clamp(static_cast<int> (std::lround(params.centreHalfSeconds / hopSeconds)), 1, historyHops);
        minRunHops = std::max(2, static_cast<int> (std::lround(params.minNoteSeconds / hopSeconds)));
        reset();
    }

    void reset() noexcept
    {
        pushed = 0;
        finalized = 0;
        lastValue = 0.0f;
    }

    /** Extra delay the audio needs so every shifted hop has its full lookahead. */
    int getLookaheadSamples() const noexcept { return (lookaheadHops + 1) * hopSamples; }

    /** One analysis hop. centreSample: input-sample index of the analysis
        window's centre. midi: detected pitch (ignored when unvoiced).
        targetMidi: the scale note the quantizer chose for it. */
    void push(int64_t centreSample, bool voiced, float midi, float targetMidi, float hz) noexcept
    {
        auto& r = rec(pushed);
        r = { centreSample, voiced, midi, targetMidi, static_cast<int> (std::lround(targetMidi)), hz, 0.0f };
        ++pushed;
        if (glitchFilter && pushed >= 5)
            repair(pushed - 3); // needs two hops either side
        while (finalized + lookaheadHops < pushed)
        {
            rec(finalized).correction = plan(finalized);
            lastValue = rec(finalized).correction;
            ++finalized;
        }
    }

    /** Planned correction, in semitones, at input-sample time `t`, linearly
        interpolated between hops. */
    float correctionAt(double t) const noexcept
    {
        if (finalized == 0)
            return 0.0f;
        const int64_t oldest = std::max<int64_t> (0, finalized - ringSize + lookaheadHops + 2);
        const auto& first = rec(oldest);
        const double pos = (t - static_cast<double> (first.centre)) / static_cast<double> (hopSamples);
        if (pos <= 0.0)
            return first.correction;
        const int64_t j = oldest + static_cast<int64_t> (pos);
        if (j + 1 >= finalized)
            return rec(finalized - 1).correction;
        const float frac = static_cast<float> (pos - std::floor(pos));
        return rec(j).correction + frac * (rec(j + 1).correction - rec(j).correction);
    }

    /** Detected f0 near input-sample time `t` (0 when unvoiced): steers the
        shifter's pitch marks on the delayed audio. */
    float pitchHintAt(double t) const noexcept
    {
        if (pushed == 0)
            return 0.0f;
        const int64_t oldest = std::max<int64_t> (0, pushed - ringSize + 1);
        const double pos = (t - static_cast<double> (rec(oldest).centre)) / static_cast<double> (hopSamples);
        const int64_t j = std::clamp<int64_t> (oldest + static_cast<int64_t> (std::lround(std::max(0.0, pos))), oldest, pushed - 1);
        return rec(j).voiced ? rec(j).hz : 0.0f;
    }

private:
    struct Record
    {
        int64_t centre = 0;
        bool    voiced = false;
        float   midi = 0.0f;
        float   target = 0.0f;
        int     note = 0;        // target as an integer, for run comparisons
        float   hz = 0.0f;
        float   correction = 0.0f;
    };

    Record& rec(int64_t i) noexcept { return ring[static_cast<size_t> (i & (ringSize - 1))]; }
    const Record& rec(int64_t i) const noexcept { return ring[static_cast<size_t> (i & (ringSize - 1))]; }

    bool sameRun(int64_t a, int64_t b) const noexcept
    {
        return rec(a).voiced && rec(b).voiced && rec(a).note == rec(b).note;
    }

    /** Window of hops the plan for hop j may look at. */
    int64_t lo(int64_t j) const noexcept { return std::max<int64_t> ({ 0, j - historyHops, pushed - ringSize + 1 }); }
    int64_t hi(int64_t j) const noexcept { return std::min<int64_t> (pushed - 1, j + lookaheadHops); }

    /** Core = voiced, in a run that is long enough to be a real note (or runs
        off the edge of what we can see), and near the run's median pitch. */
    bool isCore(int64_t k, int64_t wlo, int64_t whi) const noexcept
    {
        if (! rec(k).voiced)
            return false;
        int64_t a = k, b = k;
        while (a > wlo && sameRun(a - 1, k)) --a;
        while (b < whi && sameRun(b + 1, k)) ++b;
        const bool openEnded = (a == wlo && wlo > 0) || b == whi;
        if (b - a + 1 < minRunHops && ! openEnded)
            return false; // a passing note: part of the transition

        // run median
        std::array<float, ringSize> tmp {};
        int n = 0;
        for (int64_t i = a; i <= b && n < ringSize; ++i)
            tmp[static_cast<size_t> (n++)] = rec(i).midi;
        std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
        const float median = tmp[static_cast<size_t> (n / 2)];
        return std::abs(rec(k).midi - median) <= params.coreToleranceSemitones;
    }

    /** Target minus the note's centre at hop k.
        With vibrato, the centre is the mean over exactly one vibrato cycle
        (period measured from this run's own crossings of its median), placed
        inside the run: a whole cycle cancels the vibrato exactly, where a
        fixed window would leak a few percent of it back into the correction
        and exaggerate it. Without vibrato it is the mean of the nearby core
        hops. */
    float coreValue(int64_t k, int64_t wlo, int64_t whi) const noexcept
    {
        int64_t a = k, b = k;
        while (a > wlo && sameRun(a - 1, k)) --a;
        while (b < whi && sameRun(b + 1, k)) ++b;

        float centre = rec(k).midi;
        const float period = vibratoPeriodHops(a, b);
        if (period > 0.0f && static_cast<float> (b - a + 1) >= period)
        {
            const float half = 0.5f * period;
            const double mid = std::clamp(static_cast<double> (k), static_cast<double> (a) + half - 0.5,
                                          static_cast<double> (b) - half + 0.5);
            double sum = 0.0, wsum = 0.0;
            for (int64_t i = a; i <= b; ++i)
            {
                const double w = std::clamp(half + 0.5 - std::abs(static_cast<double> (i) - mid), 0.0, 1.0);
                sum += w * rec(i).midi;
                wsum += w;
            }
            if (wsum > 0.0)
                centre = static_cast<float> (sum / wsum);
        }
        else
        {
            double sum = 0.0;
            int n = 0;
            for (int64_t i = std::max(a, k - centreHalfHops); i <= std::min(b, k + centreHalfHops); ++i)
                if (isCore(i, wlo, whi))
                {
                    sum += rec(i).midi;
                    ++n;
                }
            if (n > 0)
                centre = static_cast<float> (sum / n);
        }
        return std::clamp(rec(k).target - centre, -12.0f, 12.0f);
    }

    /** Vibrato period of run [a, b] in hops, from the spacing of its crossings
        of the run median; 0 when there is no steady 4-12 Hz oscillation. */
    float vibratoPeriodHops(int64_t a, int64_t b) const noexcept
    {
        if (b - a < 6)
            return 0.0f;
        std::array<float, ringSize> tmp {};
        int n = 0;
        for (int64_t i = a; i <= b && n < ringSize; ++i)
            tmp[static_cast<size_t> (n++)] = rec(i).midi;
        std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
        const float median = tmp[static_cast<size_t> (n / 2)];

        int lastSign = 0, crossings = 0;
        double firstCross = -1.0, lastCross = -1.0;
        float swing = 0.0f;
        for (int64_t i = a; i <= b; ++i)
        {
            const float d = rec(i).midi - median;
            swing = std::max(swing, std::abs(d));
            const int sign = d > 0.03f ? 1 : (d < -0.03f ? -1 : 0);
            if (sign != 0 && sign != lastSign)
            {
                if (lastSign != 0)
                {
                    const double x = static_cast<double> (i) - 0.5;
                    if (firstCross < 0.0) firstCross = x;
                    lastCross = x;
                    ++crossings;
                }
                lastSign = sign;
            }
        }
        if (crossings < 3 || swing < 0.12f)
            return 0.0f;
        const float halfHops = static_cast<float> ((lastCross - firstCross) / (crossings - 1));
        const float halfSec = halfHops * static_cast<float> (hopSeconds);
        if (halfSec < 0.040f || halfSec > 0.125f)
            return 0.0f;
        return 2.0f * halfHops;
    }

    /** Glitch filter for hop i (i-2 .. i+2 exist). */
    void repair(int64_t i) noexcept
    {
        auto& r = rec(i);
        if (! r.voiced)
            return;
        auto v = [this](int64_t k) noexcept { return rec(k).voiced; };
        auto m = [this](int64_t k) noexcept { return rec(k).midi; };

        // Pitch outlier, one hop or two: both sides agree, this hop doesn't.
        auto fixPitch = [&](int64_t k, float ref, const Record& like) noexcept
        {
            auto& x = rec(k);
            x.midi = ref;
            x.hz = 440.0f * std::exp2((ref - 69.0f) / 12.0f);
            x.note = like.note;
            x.target = like.target;
        };
        if (v(i - 1) && v(i + 1) && std::abs(m(i - 1) - m(i + 1)) < sideAgree
            && std::abs(m(i) - 0.5f * (m(i - 1) + m(i + 1))) > outlierJump)
            fixPitch(i, 0.5f * (m(i - 1) + m(i + 1)), rec(i - 1));
        else if (v(i - 1) && v(i + 1) && v(i + 2) && std::abs(m(i - 1) - m(i + 2)) < sideAgree
                 && std::abs(m(i) - m(i - 1)) > outlierJump && std::abs(m(i + 1) - m(i - 1)) > outlierJump)
        {
            fixPitch(i, m(i - 1) + (m(i + 2) - m(i - 1)) / 3.0f, rec(i - 1));
            fixPitch(i + 1, m(i - 1) + 2.0f * (m(i + 2) - m(i - 1)) / 3.0f, rec(i - 1));
        }

        // Note flicker, one hop or two, between two hops on the same note.
        if (v(i - 1) && v(i + 1) && r.note != rec(i - 1).note && rec(i + 1).note == rec(i - 1).note)
        {
            r.note = rec(i - 1).note;
            r.target = rec(i - 1).target;
        }
        else if (v(i - 1) && v(i + 1) && v(i + 2) && r.note != rec(i - 1).note
                 && rec(i + 1).note == r.note && rec(i + 2).note == rec(i - 1).note)
        {
            r.note = rec(i + 1).note = rec(i - 1).note;
            r.target = rec(i + 1).target = rec(i - 1).target;
        }
    }

    /** Hard correction for hop k: target minus the detected pitch, leaving
        up to knobVibrato of vibrato around the zero-lag note centre. */
    float hardValue(int64_t k, int64_t wlo, int64_t whi) const noexcept
    {
        const auto& r = rec(k);
        float kept = 0.0f;
        if (knobVibrato > 0.0f && isCore(k, wlo, whi))
        {
            const float centre = r.target - coreValue(k, wlo, whi);
            kept = softLimit(r.midi - centre, knobVibrato);
        }
        return std::clamp(r.target - r.midi + kept, -12.0f, 12.0f);
    }

    static float softLimit(float x, float limit) noexcept
    {
        const float knee = 0.7f * limit;
        const float a = std::abs(x);
        if (a <= knee)
            return x;
        return std::copysign(knee + (limit - knee) * std::tanh((a - knee) / (limit - knee)), x);
    }

    float plan(int64_t j) const noexcept
    {
        const float natural = planNatural(j);
        if (knobHumanize >= 0.999f || ! rec(j).voiced)
            return natural;

        // Speed: centred average of the hard correction over +-snap hops,
        // within this voiced stretch only.
        const int64_t wlo = lo(j), whi = hi(j);
        const int snap = static_cast<int> (std::lround(std::pow(1.0f - knobSpeed, 1.5f) * static_cast<float> (maxSnapHops)));
        double sum = hardValue(j, wlo, whi);
        int n = 1;
        for (int64_t k = j - 1; k >= std::max(wlo, j - snap) && rec(k).voiced; --k) { sum += hardValue(k, wlo, whi); ++n; }
        for (int64_t k = j + 1; k <= std::min(whi, j + snap) && rec(k).voiced; ++k) { sum += hardValue(k, wlo, whi); ++n; }
        const float hard = static_cast<float> (sum / n);

        return knobHumanize * natural + (1.0f - knobHumanize) * hard;
    }

    float planNatural(int64_t j) const noexcept
    {
        const int64_t wlo = lo(j), whi = hi(j);

        if (! rec(j).voiced)
        {
            // Pre-set the next note's correction so its first grain is already right.
            for (int64_t k = j + 1; k <= whi; ++k)
                if (isCore(k, wlo, whi))
                    return coreValue(k, wlo, whi);
            return lastValue;
        }

        if (isCore(j, wlo, whi))
            return coreValue(j, wlo, whi);

        // Nearest cores either side, without crossing silence.
        int64_t a = -1, b = -1;
        for (int64_t k = j - 1; k >= wlo && rec(k).voiced; --k)
            if (isCore(k, wlo, whi)) { a = k; break; }
        for (int64_t k = j + 1; k <= whi && rec(k).voiced; ++k)
            if (isCore(k, wlo, whi)) { b = k; break; }

        if (a >= 0 && b >= 0)
        {
            const float ca = coreValue(a, wlo, whi), cb = coreValue(b, wlo, whi);
            const float x = static_cast<float> (j - a) / static_cast<float> (b - a);
            const float w = 0.5f - 0.5f * std::cos(3.14159265f * x);
            return ca + w * (cb - ca);
        }
        if (b >= 0) return coreValue(b, wlo, whi); // phrase start / scoop: land in tune
        if (a >= 0) return coreValue(a, wlo, whi); // tail of a note
        return lastValue;
    }

    static constexpr int    ringSize = 128;             // power of two
    static constexpr int    maxLookahead = 48;
    static constexpr int    historyHops = 40;
    Params params;
    static constexpr int   maxSnapHops = 8;      // slowest Speed: +-8 hops (~93 ms)
    static constexpr float outlierJump = 3.5f;   // semitones off both neighbours = a glitch
    static constexpr float sideAgree = 1.5f;     // ...when the neighbours agree this well
    float knobSpeed = 1.0f, knobHumanize = 1.0f, knobVibrato = 0.0f;
    bool  glitchFilter = true;

    int hopSamples = 512;
    double hopSeconds = 512.0 / 44100.0;
    int lookaheadHops = 10;
    int centreHalfHops = 8;
    int minRunHops = 5;

    std::array<Record, ringSize> ring {};
    int64_t pushed = 0, finalized = 0;
    float lastValue = 0.0f;
};

