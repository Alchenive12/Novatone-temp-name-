#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "ChordTracker.h"
#include <algorithm>
#include <memory>
#include <vector>

/**
    Chords read from a MIDI file for the Harmony tab ("MIDI file" source): an alternative to detecting
    chords from the sidechain. Times are in quarter notes (PPQ) from the file's start, so they follow
    the host's song position; an offset (beats) lines the chords up with the lead vocal.

    Built once on the message thread, then immutable (the audio thread reads a shared_ptr snapshot).
*/
struct MidiChordTrack
{
    struct Segment
    {
        double   start = 0.0, end = 0.0;   // PPQ
        uint16_t mask = 0;                 // pitch classes sounding (bit 0 = C)
        int      root = -1, quality = 0;   // best chord name (root -1 = unnamed: the mask is still used)
        int      bass = -1;                // lowest pitch class
    };

    std::vector<Segment> segments;
    double lengthPpq = 0.0;
    juce::String fileName;
    juce::MemoryBlock fileData;            // original bytes, saved with the project

    //==========================================================================
    // Editing (chord write mode): edits make a modified copy, which is then published.

    static Segment makeChord (int root, int quality, double start, double end)
    {
        Segment s; s.start = start; s.end = end; s.root = root; s.quality = quality; s.bass = root;
        s.mask = ChordTracker::chordMask (root, quality);
        return s;
    }

    /** Puts `seg` in, replacing whatever was under [seg.start, seg.end) (others are trimmed or split). */
    void overwrite (const Segment& seg)
    {
        std::vector<Segment> out;
        for (const auto& o : segments)
        {
            if (o.end <= seg.start || o.start >= seg.end) { out.push_back (o); continue; }
            if (o.start < seg.start) { auto l = o; l.end = seg.start; out.push_back (l); }
            if (o.end > seg.end)     { auto r = o; r.start = seg.end; out.push_back (r); }
        }
        out.push_back (seg);
        std::sort (out.begin(), out.end(), [] (const Segment& a, const Segment& b) { return a.start < b.start; });
        segments = std::move (out);
        lengthPpq = segments.empty() ? 0.0 : segments.back().end;
    }

    void remove (size_t index)
    {
        if (index < segments.size()) segments.erase (segments.begin() + (std::ptrdiff_t) index);
        lengthPpq = segments.empty() ? 0.0 : segments.back().end;
    }

    /** Index of the segment at `ppq` (file time), or -1. */
    int indexAt (double ppq) const noexcept
    {
        for (size_t i = 0; i < segments.size(); ++i)
            if (ppq >= segments[i].start && ppq < segments[i].end) return (int) i;
        return -1;
    }

    /** Start of the first chord at or after `ppq`, or a large number. */
    double nextStartAfter (double ppq) const noexcept
    {
        for (const auto& s : segments) if (s.start >= ppq - 1.0e-9) return s.start;
        return 1.0e9;
    }

    //==========================================================================
    // Saved with the project: "start,end,mask,root,quality,bass;..."
    juce::String toString() const
    {
        juce::StringArray parts;
        for (const auto& sg : segments)
            parts.add (juce::String (sg.start, 4) + "," + juce::String (sg.end, 4) + "," + juce::String ((int) sg.mask) + ","
                       + juce::String (sg.root) + "," + juce::String (sg.quality) + "," + juce::String (sg.bass));
        return parts.joinIntoString (";");
    }

    static std::shared_ptr<const MidiChordTrack> fromString (const juce::String& text, const juce::String& name)
    {
        auto t = std::make_shared<MidiChordTrack>();
        t->fileName = name;
        for (const auto& part : juce::StringArray::fromTokens (text, ";", ""))
        {
            const auto f = juce::StringArray::fromTokens (part, ",", "");
            if (f.size() < 6) continue;
            Segment sg;
            sg.start = f[0].getDoubleValue(); sg.end = f[1].getDoubleValue();
            sg.mask = (uint16_t) f[2].getIntValue(); sg.root = f[3].getIntValue(); sg.quality = f[4].getIntValue(); sg.bass = f[5].getIntValue();
            if (sg.end > sg.start && sg.mask != 0) t->segments.push_back (sg);
        }
        std::sort (t->segments.begin(), t->segments.end(), [] (const Segment& a, const Segment& b) { return a.start < b.start; });
        t->lengthPpq = t->segments.empty() ? 0.0 : t->segments.back().end;
        return t;
    }

    /** Segment sounding at `ppq`, or nullptr (gap / outside the file). */
    const Segment* at (double ppq) const noexcept
    {
        auto it = std::upper_bound (segments.begin(), segments.end(), ppq,
                                    [] (double p, const Segment& s) { return p < s.start; });
        if (it == segments.begin()) return nullptr;
        --it;
        return ppq < it->end ? &*it : nullptr;
    }

    static juce::String chordName (int root, int quality)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        if (root < 0) return "?";
        return juce::String (names[root % 12]) + ChordTracker::qualitySuffix (quality);
    }

    /** Names a pitch-class set: the chord whose tones are all present with the most tones, preferring the bass as root. */
    static void nameChord (uint16_t mask, int bass, int& root, int& quality)
    {
        root = -1; quality = 0;
        int best = -1000;
        for (int r = 0; r < 12; ++r)
            for (int q = 0; q < ChordTracker::numQualities; ++q)
            {
                const uint16_t cm = ChordTracker::chordMask (r, q);
                const int tones = juce::countNumberOfBits ((uint32_t) cm);
                const int present = juce::countNumberOfBits ((uint32_t) (cm & mask));
                const int extra = juce::countNumberOfBits ((uint32_t) (mask & ~cm));
                if (present < juce::jmin (tones, 2)) continue;
                int score = present * 4 - (tones - present) * 5 - extra * 2 + (r == bass ? 3 : 0);
                if (q == ChordTracker::power) score -= 2;     // prefer a real triad when one fits
                if (score > best) { best = score; root = r; quality = q; }
            }
    }

    /** Reads chords from a MIDI file (all tracks merged, drum channel 10 ignored). nullptr if there are no notes. */
    static std::shared_ptr<const MidiChordTrack> load (const juce::File& file, juce::String& error)
    {
        juce::MemoryBlock data;
        if (! file.loadFileAsData (data)) { error = "Couldn't read the file."; return nullptr; }
        auto t = fromData (data, error);
        if (t != nullptr) const_cast<MidiChordTrack&> (*t).fileName = file.getFileName();
        return t;
    }

    static std::shared_ptr<const MidiChordTrack> fromData (const juce::MemoryBlock& data, juce::String& error)
    {
        juce::MemoryInputStream in (data, false);
        juce::MidiFile mf;
        if (! mf.readFrom (in)) { error = "That isn't a MIDI file."; return nullptr; }
        const short tf = mf.getTimeFormat();
        const bool ticks = tf > 0;
        if (! ticks) mf.convertTimestampTicksToSeconds();   // SMPTE files: seconds, mapped at 120 BPM below

        struct Ev { double ppq; int note; bool on; };
        std::vector<Ev> evs;
        for (int tr = 0; tr < mf.getNumTracks(); ++tr)
        {
            const auto* seq = mf.getTrack (tr);
            for (const auto* e : *seq)
            {
                const auto& m = e->message;
                if (m.getChannel() == 10) continue;
                const double ppq = ticks ? m.getTimeStamp() / (double) tf : m.getTimeStamp() * 2.0;
                if (m.isNoteOn())       evs.push_back ({ ppq, m.getNoteNumber(), true });
                else if (m.isNoteOff()) evs.push_back ({ ppq, m.getNoteNumber(), false });
            }
        }
        if (evs.empty()) { error = "No notes in that MIDI file."; return nullptr; }
        std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b)
                          { return a.ppq < b.ppq || (a.ppq == b.ppq && ! a.on && b.on); });

        auto track = std::make_shared<MidiChordTrack>();
        track->fileData = data;
        std::array<int, 128> count {};
        std::vector<Segment> raw;
        size_t i = 0;
        while (i < evs.size())
        {
            const double t = evs[i].ppq;
            for (; i < evs.size() && evs[i].ppq == t; ++i)
                count[(size_t) evs[i].note] = juce::jmax (0, count[(size_t) evs[i].note] + (evs[i].on ? 1 : -1));
            const double next = i < evs.size() ? evs[i].ppq : t;
            if (next <= t) continue;
            Segment s; s.start = t; s.end = next;
            int low = 128;
            for (int n = 0; n < 128; ++n)
                if (count[(size_t) n] > 0) { s.mask |= (uint16_t) (1u << (n % 12)); low = juce::jmin (low, n); }
            if (s.mask == 0) continue;
            s.bass = low % 12;
            raw.push_back (s);
        }

        // Merge: same pitch classes back to back; very short pieces (< 1/8 beat: passing / overlapping notes)
        // join the previous chord instead of becoming chords of their own.
        for (const auto& s : raw)
        {
            if (! track->segments.empty())
            {
                auto& p = track->segments.back();
                const bool touching = s.start - p.end < 0.05;
                if (touching && (s.mask == p.mask || s.end - s.start < 0.125)) { p.end = s.end; continue; }
            }
            track->segments.push_back (s);
        }
        for (auto& s : track->segments)
            nameChord (s.mask, s.bass, s.root, s.quality);
        track->lengthPpq = track->segments.back().end;
        return track;
    }

    /** Best offset (beats, added to the chords' position) so the lead's notes land on chord tones.
        leadNotes: (startPpq, endPpq, midi). Searches +-maxShift beats in 1/8-beat steps; ties go to the
        smallest shift. Returns false if there isn't enough of the lead to judge. */
    struct LeadNote { double start, end; float midi; };
    bool bestOffset (const std::vector<LeadNote>& lead, double maxShift, double currentOffset, double& out, float& fit) const
    {
        double total = 0.0;
        for (const auto& n : lead) total += juce::jmax (0.0, n.end - n.start);
        if (total < 2.0 || segments.empty()) return false;

        // For a shift: (on chord tones - 0.6 x off them) per beat of lead the file covers. Comparing ratios
        // (not sums) keeps shifts that slide more of the lead into the file from winning just by coverage.
        struct Res { double off, ratio, covered, hit; };
        auto eval = [&] (double off)
        {
            double s = 0.0, hit = 0.0, covered = 0.0;
            for (const auto& n : lead)
            {
                const int pc = ((int) std::lround (n.midi) % 12 + 12) % 12;
                // sample each note every 1/8 beat (at the piece's middle) so long notes count across chord changes
                for (double p = n.start; p < n.end; p += 0.125)
                {
                    const double w = juce::jmin (0.125, n.end - p);
                    if (const auto* seg = at (p + 0.5 * w - off))
                    {
                        covered += w;
                        if ((seg->mask >> pc) & 1u) { s += w; hit += w; }
                        else s -= w * 0.6;
                    }
                }
            }
            return Res { off, covered > 0.0 ? s / covered : -1.0, covered, hit };
        };
        std::vector<Res> all;
        double maxCovered = 0.0;
        for (double off = -maxShift; off <= maxShift + 1.0e-9; off += 0.125)
        {
            all.push_back (eval (off));
            maxCovered = juce::jmax (maxCovered, all.back().covered);
        }
        all.push_back (eval (currentOffset));
        if (maxCovered < 1.0) return false;
        const Res* best = nullptr;
        for (const auto& r : all)
        {
            if (r.covered < 0.6 * maxCovered) continue;
            if (best == nullptr || r.ratio > best->ratio + 1.0e-9
                || (std::abs (r.ratio - best->ratio) < 1.0e-9 && std::abs (r.off) < std::abs (best->off)))
                best = &r;
        }
        if (best == nullptr) return false;
        // second pass: 1/32-beat steps around the winner, kept on a 1/16 grid
        Res fine = *best;
        for (double off = best->off - 0.125; off <= best->off + 0.125 + 1.0e-9; off += 0.03125)
        {
            const auto r = eval (std::round (off * 16.0) / 16.0);
            if (r.covered >= 0.6 * maxCovered && r.ratio > fine.ratio + 1.0e-9) fine = r;
        }
        best = &fine;
        out = best->off;
        const double hit = best->hit;
        total = best->covered;
        fit = (float) juce::jlimit (0.0, 1.0, hit / total);   // share of the lead that sits on chord tones
        return true;
    }
};
