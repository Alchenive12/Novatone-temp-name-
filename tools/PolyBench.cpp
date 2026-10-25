// Dev-only polyphonic benchmark: renders chords with known notes, runs the poly
// detector and editor, and scores them. Build with -DNOVATONE_BUILD_TOOLS=ON.
//   NovaTonePolyBench [detect|edit|all]
#include <JuceHeader.h>
#include "DSP/TuneEngine.h"
#include "Poly/PolyAnalyzer.h"
#include <random>
#include <map>
#include <set>
#include <optional>

namespace
{
constexpr double sr = 48000.0;
constexpr int blockSize = 512;

double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

//==============================================================================
// Instruments (all deterministic)
enum class Inst { Guitar, Piano, Strings, Choir };

const char* instName(Inst i)
{
    switch (i) { case Inst::Guitar: return "guitar"; case Inst::Piano: return "piano";
                 case Inst::Strings: return "strings"; case Inst::Choir: return "choir"; }
    return "?";
}

// Karplus-Strong plucked string
void addPluck(std::vector<float>& out, int start, int len, double f0, float amp, std::mt19937& rng)
{
    const double period = sr / f0;
    const int n = (int) std::floor(period);
    const double frac = period - n;
    std::vector<float> buf((size_t) n + 2);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (auto& v : buf) v = u(rng);
    // soften pick
    for (int k = 0; k < 2; ++k)
        for (size_t i = 1; i < buf.size(); ++i) buf[i] = 0.5f * (buf[i] + buf[i - 1]);
    size_t idx = 0;
    float prev = 0.0f;
    const float loss = 0.996f;
    for (int i = 0; i < len && start + i < (int) out.size(); ++i)
    {
        const size_t j = (idx + 1) % (size_t) n;
        float y = loss * (float) ((1.0 - frac) * 0.5 * (buf[idx] + buf[j]) + frac * 0.5 * (buf[j] + prev));
        prev = buf[idx];
        buf[idx] = y;
        idx = j;
        const float env = i < 2000 ? 1.0f : std::exp(-(float) (i - 2000) / (float) (sr * 1.6));
        out[(size_t) (start + i)] += amp * y * env;
    }
}

// Additive instruments
void addAdditive(std::vector<float>& out, int start, int len, double f0, float amp, Inst inst, std::mt19937& rng, double vibOverride = -1.0)
{
    std::uniform_real_distribution<double> ph(0.0, juce::MathConstants<double>::twoPi);
    const int numH = inst == Inst::Choir ? 30 : 16;
    const double B = inst == Inst::Piano ? 0.0004 : 0.0;
    const double vibHz = 5.0 + 1.2 * ph(rng) / 6.28;
    const double vibDepth = vibOverride >= 0.0 ? vibOverride : (inst == Inst::Strings ? 0.008 : (inst == Inst::Choir ? 0.022 : 0.0));
    const double vibPh = ph(rng);
    for (int h = 1; h <= numH; ++h)
    {
        double fh = h * f0 * std::sqrt(1.0 + B * h * h);
        if (fh > 9000.0) break;
        double a;
        if (inst == Inst::Piano)        a = 1.0 / std::pow(h, 1.25) * (h == 1 ? 0.8 : 1.0);
        else if (inst == Inst::Strings) a = 1.0 / h;
        else
        {
            // "ah" vowel formants on a glottal-ish source
            auto formant = [&](double fc, double bw, double g) { const double d = (fh - fc) / bw; return g / (1.0 + d * d); };
            a = (1.0 / std::pow(h, 0.9)) * (0.08 + formant(750, 90, 1.0) + formant(1150, 110, 0.6) + formant(2800, 180, 0.25));
        }
        const double p0 = ph(rng);
        double phase = p0;
        for (int i = 0; i < len && start + i < (int) out.size(); ++i)
        {
            const double t = i / sr;
            const double vib = 1.0 + vibDepth * std::sin(juce::MathConstants<double>::twoPi * vibHz * t + vibPh);
            phase += juce::MathConstants<double>::twoPi * fh * vib / sr;
            double env;
            if (inst == Inst::Piano)
                env = std::min(1.0, t / 0.004) * std::exp(-t * (0.9 + 0.25 * h));
            else
                env = std::min(1.0, t / 0.08) * std::min(1.0, (len - i) / (sr * 0.05));
            out[(size_t) (start + i)] += (float) (amp * a * env * std::sin(phase));
        }
    }
}

struct Ev { int midi; double start, dur; float gain = 1.0f; };
struct Case { std::string name; Inst inst; std::vector<Ev> events; double vibrato = -1.0; double detuneCents = 0.0; };

Case chords(std::string name, Inst inst, std::vector<std::pair<std::vector<int>, double>> cs)
{
    Case c { name, inst, {} };
    double t = 0.1;
    for (auto& [notes, dur] : cs)
    {
        for (size_t k = 0; k < notes.size(); ++k)
        {
            const double strum = inst == Inst::Guitar ? 0.012 * (double) k : 0.0;
            c.events.push_back({ notes[k], t + strum, dur - strum });
        }
        t += dur;
    }
    return c;
}

double caseLength(const Case& c) { double e = 0; for (auto& ev : c.events) e = std::max(e, ev.start + ev.dur); return e + 0.3; }

std::vector<float> renderCase(const Case& c, unsigned seed = 1234)
{
    std::vector<float> out((size_t) (caseLength(c) * sr) + blockSize * 4, 0.0f);
    std::mt19937 rng(seed);
    for (auto& ev : c.events)
    {
        const int start = (int) (ev.start * sr), len = (int) (ev.dur * sr);
        const float amp = 0.12f * ev.gain;
        const double f0 = midiToHz(ev.midi);
        if (c.inst == Inst::Guitar) addPluck(out, start, len, f0, amp * 1.6f, rng);
        else
        {
            addAdditive(out, start, len, f0, amp, c.inst, rng, c.vibrato);
            if (c.detuneCents != 0.0) addAdditive(out, start, len, f0 * std::pow(2.0, c.detuneCents / 1200.0), amp * 0.8f, c.inst, rng, c.vibrato);
        }
    }
    float peak = 0; for (auto v : out) peak = std::max(peak, std::abs(v));
    for (auto& v : out) v *= 0.5f / std::max(1e-6f, peak);
    return out;
}

/** Notes that should be heard at time t, or nullopt if t is too close to an event boundary to score. */
std::optional<std::set<int>> truthAt(const Case& c, double t, double settle, double release)
{
    std::set<int> s;
    for (auto& ev : c.events)
    {
        if (std::abs(t - ev.start) < settle || std::abs(t - (ev.start + ev.dur)) < release) return std::nullopt;
        if (t > ev.start && t < ev.start + ev.dur) s.insert(ev.midi);
    }
    return s;
}

juce::String noteName(int m)
{
    static const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    return juce::String(n[((m % 12) + 12) % 12]) + juce::String(m / 12 - 1);
}

juce::String setStr(const std::set<int>& s)
{
    juce::StringArray a; for (int m : s) a.add(noteName(m)); return a.joinIntoString(" ");
}

void prepareEngine(TuneEngine& e)
{
    e.prepare({ sr, (juce::uint32) blockSize, 2 });
    e.setScale(0x0FFF);
    e.setCenterAmount(0.0f);
    e.setSensitivityAmount(0.65f);
    e.setPolyphonicEnabled(true);
}

//==============================================================================
struct Score { int tp = 0, fp = 0, fn = 0, octave = 0, frames = 0, exact = 0, notesTruth = 0, notesHit = 0, notesDet = 0, notesDetOk = 0; };

void scoreFrame(Score& sc, const std::set<int>& det, const std::set<int>& tr)
{
    ++sc.frames;
    if (det == tr) ++sc.exact;
    for (int m : det) { if (tr.count(m)) ++sc.tp; else { ++sc.fp; if (tr.count(m - 12) || tr.count(m + 12)) ++sc.octave; } }
    for (int m : tr) if (!det.count(m)) ++sc.fn;
}

/** Old real-time detector (TuneEngine poly mode). */
Score runLive(const Case& c)
{
    auto audio = renderCase(c);
    TuneEngine e; prepareEngine(e);
    juce::AudioBuffer<float> buf(2, blockSize);
    Score sc;
    for (int pos = 0; pos + blockSize <= (int) audio.size(); pos += blockSize)
    {
        for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, audio.data() + pos, blockSize);
        juce::dsp::AudioBlock<float> blk(buf);
        e.process(juce::dsp::ProcessContextReplacing<float>(blk));
        // live analysis lags: compare against what was playing ~120 ms earlier
        const double t = (pos + blockSize) / sr - 0.12;
        auto tr = truthAt(c, t, 0.15, 0.08);
        if (!tr) continue;
        std::set<int> det;
        for (int v = 0; v < e.getNumPolyVoices(); ++v)
            if (e.getPolyRawMidi(v) > 0.0f) det.insert((int) std::lround(e.getPolyRawMidi(v)));
        scoreFrame(sc, det, *tr);
    }
    return sc;
}

/** New offline clip analyser. */
Score runClip(const Case& c, bool verbose, double& secondsTaken)
{
    auto audio = renderCase(c);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto an = Poly::analyse(audio.data(), (int) audio.size(), sr);
    secondsTaken = (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001;

    Score sc;
    for (int f = 0; f < an.numFrames; ++f)
    {
        const double t = an.frameToSeconds(f);
        auto tr = truthAt(c, t, 0.06, 0.06);
        if (!tr) continue;
        std::set<int> det;
        for (auto& n : an.notes) if (f >= n.startFrame && f < n.endFrame) det.insert((int) std::lround(n.midi));
        scoreFrame(sc, det, *tr);
    }

    // Note level: a truth note is found if a detected note of the same semitone covers >= 50% of it
    for (auto& ev : c.events)
    {
        ++sc.notesTruth;
        double covered = 0;
        for (auto& n : an.notes)
            if ((int) std::lround(n.midi) == ev.midi)
            {
                const double a = std::max(ev.start, an.frameToSeconds(n.startFrame)), b = std::min(ev.start + ev.dur, an.frameToSeconds(n.endFrame));
                covered += std::max(0.0, b - a);
            }
        if (covered >= 0.5 * ev.dur) ++sc.notesHit;
    }
    for (auto& n : an.notes)
    {
        ++sc.notesDet;
        const double ns = an.frameToSeconds(n.startFrame), ne = an.frameToSeconds(n.endFrame);
        bool ok = false;
        for (auto& ev : c.events)
            if ((int) std::lround(n.midi) == ev.midi && std::min(ne, ev.start + ev.dur) - std::max(ns, ev.start) > 0.5 * (ne - ns)) ok = true;
        if (ok) ++sc.notesDetOk;
        else if (verbose)
            std::cout << "      extra note " << noteName((int) std::lround(n.midi)) << " (" << juce::String(n.midi, 2) << ") at "
                      << juce::String(ns, 2) << "-" << juce::String(ne, 2) << "s\n";
    }
    if (verbose)
        for (auto& ev : c.events)
        {
            double covered = 0;
            for (auto& n : an.notes)
                if ((int) std::lround(n.midi) == ev.midi)
                    covered += std::max(0.0, std::min(ev.start + ev.dur, an.frameToSeconds(n.endFrame)) - std::max(ev.start, an.frameToSeconds(n.startFrame)));
            if (covered < 0.5 * ev.dur)
                std::cout << "      missed note " << noteName(ev.midi) << " at " << juce::String(ev.start, 2) << "s (covered "
                          << juce::roundToInt(100 * covered / ev.dur) << "%)\n";
        }
    return sc;
}

juce::String pct(double v) { return juce::String(juce::roundToInt(v * 100.0)).paddedLeft(' ', 3) + "%"; }

void printScore(const juce::String& name, const Score& s, bool notes)
{
    const double p = s.tp + s.fp ? (double) s.tp / (s.tp + s.fp) : 0, r = s.tp + s.fn ? (double) s.tp / (s.tp + s.fn) : 0;
    const double f1 = p + r > 0 ? 2 * p * r / (p + r) : 0;
    std::cout << juce::String(name).paddedRight(' ', 38) << " F1 " << pct(f1) << "  (prec " << pct(p) << ", recall " << pct(r)
              << ")  exact chord " << pct(s.frames ? (double) s.exact / s.frames : 0) << "  octave err " << juce::String(s.octave).paddedLeft(' ', 4);
    if (notes)
        std::cout << "  | notes found " << s.notesHit << "/" << s.notesTruth << ", extra " << (s.notesDet - s.notesDetOk);
    std::cout << "\n";
}

std::vector<Case> benchCases()
{
    auto G = Inst::Guitar, P = Inst::Piano, S = Inst::Strings, C = Inst::Choir;
    std::vector<Case> cs;
    // --- guitar ---
    cs.push_back(chords("guitar open chords", G, { {{48,52,55,60,64},1.5}, {{43,47,50,55,59,67},1.5}, {{45,52,57,60,64},1.5}, {{41,48,53,57,60,65},1.5} }));
    cs.push_back(chords("guitar barre / high voicings", G, { {{45,52,57,61,64,69},1.5}, {{47,54,59,62,66,71},1.5}, {{52,59,64,68,71,76},1.5} }));
    cs.push_back(chords("guitar power chords (oct doubling)", G, { {{40,47,52},0.8}, {{43,50,55},0.8}, {{45,52,57},0.8}, {{38,45,50},0.8} }));
    {
        Case c { "guitar arpeggio (ringing)", G, {} };
        const int pat[] = { 48, 55, 64, 60, 67, 64, 45, 52, 60, 57, 64, 60 };
        for (int i = 0; i < 12; ++i) c.events.push_back({ pat[i], 0.1 + 0.25 * i, (i < 6 ? 6 : 12) * 0.25 - 0.25 * (i % 6) });
        cs.push_back(c);
    }
    {
        Case c { "guitar single-note riff", G, {} };
        const int riff[] = { 52, 55, 57, 55, 52, 50, 52, 59, 57, 55 };
        for (int i = 0; i < 10; ++i) c.events.push_back({ riff[i], 0.1 + 0.2 * i, 0.19 });
        cs.push_back(c);
    }
    // --- vocal stacks ---
    cs.push_back(chords("vocals: 3-part harmony (block)", C, { {{57,60,64},1.4}, {{55,59,62},1.4}, {{53,57,60},1.4}, {{55,60,64},1.4} }));
    {
        // Independent moving lines: lead melody over two harmony voices, legato
        Case c { "vocals: moving harmony lines", C, {} };
        const int lead[]  = { 64, 65, 67, 65, 64, 62, 64, 64 };
        const int mid[]   = { 60, 62, 64, 62, 60, 59, 60, 60 };
        const int low[]   = { 57, 57, 60, 57, 57, 55, 55, 55 };
        for (int i = 0; i < 8; ++i)
        {
            c.events.push_back({ lead[i], 0.1 + 0.6 * i, 0.6 });
            c.events.push_back({ mid[i], 0.1 + 0.6 * i, 0.6, 0.8f });
            c.events.push_back({ low[i], 0.1 + 0.6 * i, 0.6, 0.7f });
        }
        // merge repeated notes in a voice into one sustained event (legato truth)
        std::vector<Ev> merged;
        for (auto& e : c.events)
        {
            bool joined = false;
            for (auto& m : merged) if (m.midi == e.midi && std::abs(m.start + m.dur - e.start) < 1e-6 && m.gain == e.gain) { m.dur += e.dur; joined = true; break; }
            if (!joined) merged.push_back(e);
        }
        c.events = merged;
        cs.push_back(c);
    }
    { auto c = chords("vocals: doubled lead (unison, 8 c)", C, { {{64},1.0}, {{62},1.0}, {{60},1.0}, {{67},1.0} }); c.detuneCents = 8.0; cs.push_back(c); }
    cs.push_back(chords("vocals: choir SATB", C, { {{48,55,64,72},1.6}, {{50,57,65,74},1.6}, {{43,55,62,71},1.6}, {{48,55,64,67},1.6} }));
    cs.push_back(chords("vocals: wide stack (oct + 5th)", C, { {{48,55,60,64,72},1.6}, {{45,52,57,60,69},1.6} }));
    // --- reference (not the target material, kept to catch regressions) ---
    cs.push_back(chords("piano close triads", P, { {{60,64,67},1.2}, {{62,65,69},1.2}, {{64,67,71},1.2}, {{65,69,72},1.2} }));
    cs.push_back(chords("strings wide pad", S, { {{36,43,52,60,67,76},2.0}, {{41,48,57,65,72},2.0} }));
    return cs;
}

//==============================================================================
// Edit test: move one chord note and measure what actually comes out.
float bandEnergy(const std::vector<float>& x, int start, int len, double f)
{
    // Goertzel over a Hann window
    double s1 = 0, s2 = 0; const double w = juce::MathConstants<double>::twoPi * f / sr, cw = 2 * std::cos(w);
    for (int i = 0; i < len; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * i / (len - 1));
        const double s0 = x[(size_t) (start + i)] * win + cw * s1 - s2; s2 = s1; s1 = s0;
    }
    return (float) (s1 * s1 + s2 * s2 - cw * s1 * s2);
}

float noteEnergy(const std::vector<float>& x, int start, int len, int midi, int harmonics)
{
    float e = 0; for (int h = 1; h <= harmonics; ++h) e += bandEnergy(x, start, len, midiToHz(midi) * h); return e;
}

void runEdit(Inst inst, std::vector<int> chord, int from, int to)
{
    auto audio = renderCase(chords("edit", inst, { { chord, 3.0 } }));
    // Reference: the chord we *want* (from replaced by to), rendered the same way.
    auto wanted = chord; for (auto& m : wanted) if (m == from) m = to;
    auto ref = renderCase(chords("want", inst, { { wanted, 3.0 } }));

    TuneEngine e; prepareEngine(e);
    e.setCenterAmount(1.0f);
    e.setPolyVoiceOverride(0, true, (float) from, (float) to, 1.0f, 1.0f, 0.0f);

    std::vector<float> out(audio.size(), 0.0f);
    juce::AudioBuffer<float> buf(2, blockSize);
    for (int pos = 0; pos + blockSize <= (int) audio.size(); pos += blockSize)
    {
        for (int ch = 0; ch < 2; ++ch) buf.copyFrom(ch, 0, audio.data() + pos, blockSize);
        juce::dsp::AudioBlock<float> blk(buf);
        e.process(juce::dsp::ProcessContextReplacing<float>(blk));
        std::copy(buf.getReadPointer(0), buf.getReadPointer(0) + blockSize, out.begin() + pos);
    }

    const int lat = e.getLatencySamples();
    const int a = (int) (1.2 * sr), len = (int) (1.0 * sr);
    const int H = 6;
    auto db = [](float r) { return 10.0f * std::log10(std::max(1e-9f, r)); };
    const float inFrom = noteEnergy(audio, a, len, from, H), outFrom = noteEnergy(out, a + lat, len, from, H);
    const float refTo  = noteEnergy(ref, a, len, to, H),     outTo  = noteEnergy(out, a + lat, len, to, H);
    float otherIn = 0, otherOut = 0;
    for (int m : chord) if (m != from) { otherIn += noteEnergy(audio, a, len, m, H); otherOut += noteEnergy(out, a + lat, len, m, H); }

    std::cout << "  " << juce::String(instName(inst)).paddedRight(' ', 8) << " [" << setStr({chord.begin(), chord.end()}) << "]  move "
              << noteName(from) << " -> " << noteName(to) << ":\n"
              << "      old note left in output:  " << juce::String(db(outFrom / inFrom), 1) << " dB   (ideal: below -30 dB)\n"
              << "      new note level vs ideal:  " << juce::String(db(outTo / refTo), 1) << " dB   (ideal: about 0 dB)\n"
              << "      other notes change:       " << juce::String(db(otherOut / otherIn), 1) << " dB   (ideal: about 0 dB)\n";
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode = argc > 1 ? argv[1] : "all";
    const juce::String only = argc > 2 ? argv[2] : "";
    const bool verbose = only.isNotEmpty();

    if (mode == "live" || mode == "all")
    {
        std::cout << "=== OLD real-time poly detector ===\n";
        Score total;
        for (auto& c : benchCases())
        {
            auto s = runLive(c);
            printScore(juce::String(c.name), s, false);
            total.tp += s.tp; total.fp += s.fp; total.fn += s.fn; total.octave += s.octave; total.frames += s.frames; total.exact += s.exact;
        }
        printScore("OVERALL", total, false);
    }
    if (mode == "clip" || mode == "all")
    {
        std::cout << "=== NEW clip analyser ===\n";
        Score total;
        double secs = 0, audioSecs = 0;
        for (auto& c : benchCases())
        {
            if (only.isNotEmpty() && only != "v" && ! juce::String(c.name).containsIgnoreCase(only)) continue;
            double took = 0;
            auto s = runClip(c, verbose, took);
            secs += took; audioSecs += caseLength(c);
            printScore(juce::String(c.name), s, true);
            total.tp += s.tp; total.fp += s.fp; total.fn += s.fn; total.octave += s.octave; total.frames += s.frames; total.exact += s.exact;
            total.notesTruth += s.notesTruth; total.notesHit += s.notesHit; total.notesDet += s.notesDet; total.notesDetOk += s.notesDetOk;
        }
        printScore("OVERALL", total, true);
        std::cout << "analysis speed: " << juce::String(audioSecs / std::max(1e-6, secs), 1) << "x real time\n";
    }
    if (mode == "edit" || mode == "all")
    {
        std::cout << "\n=== OLD editing (move one chord note, measure output) ===\n";
        runEdit(Inst::Piano,   { 48, 52, 55 }, 52, 53);
        runEdit(Inst::Choir,   { 57, 60, 64 }, 60, 62);
        runEdit(Inst::Guitar,  { 52, 55, 60 }, 55, 57);
    }
    return 0;
}
