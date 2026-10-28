// Offline check: runs a recorded vocal take through the real Pitch Coach processor with a simulated
// host playhead (120 BPM, song position given) and prints the held-note statistics it measures.
// Usage: PitchCoachCheck <take.aif> <song-beat-of-sample-0> <fromBeat> <toBeat>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Source/PluginProcessor.h"
#include "../Source/Scale.h"
#include "../Source/TakeAnalysis.h"
#include "../Source/PluginEditor.h"

class FakePlayHead : public juce::AudioPlayHead
{
public:
    double samplePos = 0.0, sampleRate = 48000.0, zeroBeat = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setPpqPosition (zeroBeat + samplePos / sampleRate * 2.0);
        return info;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 5) { std::puts ("usage: PitchCoachCheck take.aif zeroBeat fromBeat toBeat"); return 1; }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (juce::String (argv[1]))));
    if (reader == nullptr) { std::puts ("cannot read file"); return 1; }

    const double sr = reader->sampleRate;
    juce::AudioBuffer<float> file ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&file, 0, (int) reader->lengthInSamples, 0, true, true);

    PitchCoachProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    FakePlayHead head;
    head.sampleRate = sr;
    head.zeroBeat = std::atof (argv[2]);
    proc.setPlayHead (&head);
    proc.prepareToPlay (sr, 512);
    juce::Thread::sleep (50);

    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < file.getNumSamples(); pos += 512)
    {
        const int n = juce::jmin (512, file.getNumSamples() - pos);
        block.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch)
            block.copyFrom (ch, 0, file, juce::jmin (ch, file.getNumChannels() - 1), pos, n);
        head.samplePos = pos;
        proc.processBlock (block, midi);
        if ((pos / 512) % 40 == 0) juce::Thread::sleep (10);   // let the analysis thread keep up
    }
    juce::Thread::sleep (1500);

    if (argc >= 6 && juce::String (argv[5]).isNotEmpty())
        std::printf ("guide loaded: %s\n", proc.loadGuide (juce::File (juce::String (argv[5]))) ? "yes" : "NO");

    std::vector<PitchPoint> pts;
    proc.copyPassPoints (pts, 0);
    const double from = std::atof (argv[3]), to = std::atof (argv[4]);

    // notes and slides, same analysis as the editor
    std::vector<PitchPoint> take;
    for (const auto& p : pts)
        if (p.ppq >= from && p.ppq <= to) take.push_back (p);
    const auto r = TakeAnalysis::analyse (take, 9, Scale::defaultScale);
    int held = 0, within20 = 0, over40 = 0, landings = 0, quick = 0; float sum = 0.0f, landSum = 0.0f;
    for (const auto& n : r.notes)
    {
        if (! n.held) continue;
        ++held; sum += std::abs (n.cents);
        within20 += std::abs (n.cents) <= 20.0f; over40 += std::abs (n.cents) > 40.0f;
        if (n.landingMs >= 0.0f) { ++landings; landSum += n.landingMs; quick += n.landingMs <= 100.0f; }
        std::printf ("  bar %.2f  %-4s %+5.0f ct  held %.2f s  landing %s\n", take[(size_t) n.first].ppq / 4.0 + 1.0,
                     Scale::noteName (Scale::nearestNote (n.midi, 9, Scale::defaultScale)).toRawUTF8(), n.cents, n.seconds,
                     n.landingMs >= 0.0f ? (juce::String (juce::roundToInt (n.landingMs)) + " ms").toRawUTF8() : "-");
    }
    int slidePts = 0; for (auto k : r.kind) slidePts += k == TakeAnalysis::Kind::slide;
    std::printf ("held notes %d, average %.0f ct, within 20 ct %.0f%%, >40 ct: %d | landings %d, average %.0f ms, %.0f%% within 100 ms | slide share %.0f%% of voiced\n",
                 held, held ? sum / held : 0.0f, held ? 100.0 * within20 / held : 0.0, over40,
                 landings, landings ? landSum / landings : 0.0f, landings ? 100.0 * quick / landings : 0.0,
                 100.0 * slidePts / juce::jmax (1, (int) std::count_if (take.begin(), take.end(), [] (const PitchPoint& p) { return p.midi > 0.0f; })));
    if (argc >= 7)
    {
        // render the real plugin window to a PNG (bars shown = argv[7], default 8)
        if (auto* bars = proc.apvts.getParameter ("bars"))
            bars->setValueNotifyingHost (bars->convertTo0to1 (argc >= 8 ? (float) std::atoi (argv[7]) : 8.0f));
        std::unique_ptr<PitchCoachEditor> ed (dynamic_cast<PitchCoachEditor*> (proc.createEditor()));
        ed->setSize (1040, 620);
        for (int i = 0; i < 90; ++i) ed->updateNow();      // let the smooth vertical follow settle
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        const juce::File out { juce::String (argv[6]) };
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::printf ("snapshot written: %s\n", out.getFullPathName().toRawUTF8());
    }
    proc.setPlayHead (nullptr);
    return 0;
}
