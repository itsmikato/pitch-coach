#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

// Which notes count as "right", and how far a sung pitch is from the nearest of them.
namespace Scale
{
    inline const juce::StringArray& noteNames()
    {
        static const juce::StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names;
    }

    inline const juce::StringArray& scaleNames()
    {
        static const juce::StringArray names { "Major", "Minor", "Harmonic minor", "Melodic minor",
                                               "Major pentatonic", "Minor pentatonic", "Blues",
                                               "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Chromatic" };
        return names;
    }

    constexpr int defaultScale = 1;   // Minor

    // bit mask of allowed pitch classes relative to the root
    inline int intervalMask (int scaleIndex)
    {
        auto mask = [] (std::initializer_list<int> steps) { int m = 0; for (int s : steps) m |= 1 << s; return m; };
        switch (scaleIndex)
        {
            case 0:  return mask ({ 0, 2, 4, 5, 7, 9, 11 });   // major (Ionian)
            case 1:  return mask ({ 0, 2, 3, 5, 7, 8, 10 });   // natural minor (Aeolian)
            case 2:  return mask ({ 0, 2, 3, 5, 7, 8, 11 });   // harmonic minor
            case 3:  return mask ({ 0, 2, 3, 5, 7, 9, 11 });   // melodic minor (ascending)
            case 4:  return mask ({ 0, 2, 4, 7, 9 });          // major pentatonic
            case 5:  return mask ({ 0, 3, 5, 7, 10 });         // minor pentatonic
            case 6:  return mask ({ 0, 3, 5, 6, 7, 10 });      // blues
            case 7:  return mask ({ 0, 2, 3, 5, 7, 9, 10 });   // Dorian
            case 8:  return mask ({ 0, 1, 3, 5, 7, 8, 10 });   // Phrygian
            case 9:  return mask ({ 0, 2, 4, 6, 7, 9, 11 });   // Lydian
            case 10: return mask ({ 0, 2, 4, 5, 7, 9, 10 });   // Mixolydian
            case 11: return mask ({ 0, 1, 3, 5, 6, 8, 10 });   // Locrian
            default: return 0xFFF;                             // chromatic
        }
    }

    inline bool isInScale (int midiNote, int root, int scaleIndex)
    {
        const int pc = ((midiNote - root) % 12 + 12) % 12;
        return (intervalMask (scaleIndex) >> pc) & 1;
    }

    // nearest allowed note to a fractional MIDI pitch
    inline int nearestNote (float midi, int root, int scaleIndex)
    {
        const int centre = (int) std::lround (midi);
        int best = centre;
        float bestDist = 1.0e9f;
        for (int n = centre - 2; n <= centre + 2; ++n)
            if (isInScale (n, root, scaleIndex) && std::abs ((float) n - midi) < bestDist)
            {
                best = n;
                bestDist = std::abs ((float) n - midi);
            }
        return best;
    }

    inline juce::String noteName (int midiNote)
    {
        return noteNames()[((midiNote % 12) + 12) % 12] + juce::String (midiNote / 12 - 1);
    }

    // traffic-light colour for a deviation in cents
    inline juce::Colour centsColour (float cents)
    {
        const float a = std::abs (cents);
        if (a <= 10.0f) return juce::Colour (0xff3ddc84);
        if (a <= 20.0f) return juce::Colour (0xffb5e655);
        if (a <= 35.0f) return juce::Colour (0xffffb547);
        return juce::Colour (0xffff5a5a);
    }
}
