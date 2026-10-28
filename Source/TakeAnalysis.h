#pragma once

#include "PluginProcessor.h"
#include "Scale.h"
#include <algorithm>
#include <vector>

// Splits a take into steady notes and slides (transitions) between them.
//  - steady notes are judged by how far they are from the nearest note of the scale (cents)
//  - slides are judged by how long they take to arrive (a good singer gets there quickly)
namespace TakeAnalysis
{
    enum class Kind { unvoiced, note, slide };

    struct Note
    {
        int first = 0, last = 0;          // point indices
        float midi = 0.0f, cents = 0.0f;  // median pitch, distance to the nearest scale note
        float seconds = 0.0f;             // how long it is held
        float landingMs = 0.0f;           // slide time into this note (0 = landed straight on it)
        bool held = false;                // long enough to count in the statistics
        int target = 0;                   // the note it is judged against
        bool fromGuide = false;           // target comes from the guide melody (else: nearest scale note)
        bool wrongNote = false;           // more than 1.5 semitones from the guide note
    };

    struct Result
    {
        std::vector<Kind> kind;           // per point
        std::vector<float> slideMs;       // per point: duration of the slide it belongs to
        std::vector<Note> notes;
    };

    constexpr double maxGapSeconds = 0.05;   // a longer gap ends the phrase
    constexpr double minNoteSeconds = 0.07;  // shorter flat pieces are part of a slide
    constexpr double heldSeconds = 0.25;     // half a beat at 120 BPM
    constexpr float groupRange = 0.6f;       // semitones a note may wander (vibrato) before it counts as moving
    constexpr float settleRange = 0.25f;     // within 25 cents of the note's own pitch = arrived
    constexpr float maxNoteDrift = 0.35f;    // start-to-end change that makes a short piece a slide, not a note

    inline juce::Colour slideColour (float ms)
    {
        if (ms <= 100.0f) return juce::Colour (0xff3ddc84);
        if (ms <= 150.0f) return juce::Colour (0xffb5e655);
        if (ms <= 250.0f) return juce::Colour (0xffffb547);
        return juce::Colour (0xffff5a5a);
    }

    inline float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2];
    }

    // the guide note playing at a song position (nullptr if none)
    inline const GuideNote* guideAt (const std::vector<GuideNote>& guide, double ppq)
    {
        for (const auto& g : guide)
        {
            if (g.start > ppq) break;
            if (ppq < g.end) return &g;
        }
        return nullptr;
    }

    inline Result analyse (const std::vector<PitchPoint>& pts, int root, int scale,
                           const std::vector<GuideNote>* guide = nullptr)
    {
        Result r;
        const int n = (int) pts.size();
        r.kind.assign ((size_t) n, Kind::unvoiced);
        r.slideMs.assign ((size_t) n, 0.0f);

        // 1) pieces: runs of points that stay within groupRange of their running mean
        struct Piece { int first, last; bool steady; };
        std::vector<Piece> pieces;
        int start = -1;
        float sum = 0.0f;
        auto closePiece = [&] (int endIdx)
        {
            if (start < 0) return;
            const double dur = pts[(size_t) endIdx].seconds - pts[(size_t) start].seconds + 0.01;
            const int k = std::min (3, endIdx - start + 1);
            std::vector<float> head, tail;
            for (int i = 0; i < k; ++i) { head.push_back (pts[(size_t) (start + i)].midi); tail.push_back (pts[(size_t) (endIdx - i)].midi); }
            const bool flat = std::abs (median (tail) - median (head)) < maxNoteDrift;
            // held pieces are always notes (drifting while holding is a tuning problem, not a slide)
            pieces.push_back ({ start, endIdx, dur >= heldSeconds || (dur >= minNoteSeconds && flat) });
            start = -1;
        };
        for (int i = 0; i < n; ++i)
        {
            const auto& p = pts[(size_t) i];
            const bool gap = i > 0 && p.seconds - pts[(size_t) i - 1].seconds > maxGapSeconds;
            if (p.midi <= 0.0f || gap)
                closePiece (i - 1);
            if (p.midi <= 0.0f) continue;
            if (start >= 0 && std::abs (p.midi - sum / (float) (i - start)) > groupRange)
                closePiece (i - 1);
            if (start < 0) { start = i; sum = 0.0f; }
            sum += p.midi;
        }
        closePiece (n - 1);

        // 2) notes from steady pieces, slides from everything in between (within one phrase)
        std::vector<int> phrase ((size_t) n, -1);     // phrase id per voiced point
        for (int i = 0, id = 0; i < n; ++i)
        {
            if (pts[(size_t) i].midi <= 0.0f) { ++id; continue; }
            if (i > 0 && pts[(size_t) i].seconds - pts[(size_t) i - 1].seconds > maxGapSeconds) ++id;
            phrase[(size_t) i] = id;
        }

        int prevNoteLast = -1;
        int phraseStart = -1;
        for (size_t pi = 0; pi < pieces.size(); ++pi)
        {
            const auto& pc = pieces[pi];
            if (phraseStart < 0 || phrase[(size_t) phraseStart] != phrase[(size_t) pc.first])
            {
                phraseStart = pc.first;   // a new phrase: the first note's slide starts here
                prevNoteLast = -1;
            }
            if (! pc.steady)
                continue;

            Note note;
            std::vector<float> m;
            for (int i = pc.first; i <= pc.last; ++i) m.push_back (pts[(size_t) i].midi);
            const float target = median (m);

            // the note only counts from where the pitch has settled (scoop before = landing, sag after = fall-off)
            auto settled = [&] (int i) { return std::abs (pts[(size_t) i].midi - target) <= settleRange; };
            int first = pc.first, last = pc.last;
            while (first + 2 <= pc.last && ! (settled (first) && settled (first + 1) && settled (first + 2))) ++first;
            while (last - 2 >= first && ! (settled (last) && settled (last - 1) && settled (last - 2))) --last;
            if (last - first < 2) { first = pc.first; last = pc.last; }
            note.first = first;
            note.last = last;
            m.clear();
            for (int i = first; i <= last; ++i) m.push_back (pts[(size_t) i].midi);
            note.midi = median (m);
            note.target = Scale::nearestNote (note.midi, root, scale);
            if (guide != nullptr && pts[(size_t) first].playing)
                if (const auto* g = guideAt (*guide, pts[(size_t) ((first + last) / 2)].ppq))
                {
                    // same note in whatever octave you sing it
                    note.target = g->note + 12 * (int) std::lround ((note.midi - (float) g->note) / 12.0f);
                    note.fromGuide = true;
                    note.wrongNote = std::abs (note.midi - (float) note.target) > 1.5f;
                }
            note.cents = (note.midi - (float) note.target) * 100.0f;
            note.seconds = (float) (pts[(size_t) note.last].seconds - pts[(size_t) note.first].seconds + 0.01);
            note.held = note.seconds >= heldSeconds;

            // slide into this note: from the previous note (or the phrase start) to here
            // (a direct jump with no points in between, or a phrase that starts right on the note, lands in ~0 ms)
            const int slideFrom = prevNoteLast >= 0 ? prevNoteLast + 1 : phraseStart;
            note.landingMs = 0.0f;
            if (slideFrom < note.first)
            {
                const float ms = (float) (pts[(size_t) note.first].seconds - pts[(size_t) slideFrom].seconds) * 1000.0f;
                note.landingMs = ms;
                for (int i = slideFrom; i < note.first; ++i) { r.kind[(size_t) i] = Kind::slide; r.slideMs[(size_t) i] = ms; }
            }
            for (int i = note.first; i <= note.last; ++i) r.kind[(size_t) i] = Kind::note;
            r.notes.push_back (note);
            prevNoteLast = note.last;
        }

        // 3) voiced points after the last note of a phrase (fall-offs) or phrases without a note
        for (int i = 0; i < n; ++i)
        {
            if (pts[(size_t) i].midi <= 0.0f || r.kind[(size_t) i] != Kind::unvoiced) continue;
            int j = i;
            while (j + 1 < n && pts[(size_t) j + 1].midi > 0.0f && r.kind[(size_t) j + 1] == Kind::unvoiced
                   && pts[(size_t) j + 1].seconds - pts[(size_t) j].seconds <= maxGapSeconds)
                ++j;
            const float ms = (float) (pts[(size_t) j].seconds - pts[(size_t) i].seconds + 0.01) * 1000.0f;
            for (int k = i; k <= j; ++k) { r.kind[(size_t) k] = Kind::slide; r.slideMs[(size_t) k] = ms; }
            i = j;
        }
        return r;
    }
}
