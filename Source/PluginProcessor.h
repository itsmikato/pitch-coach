#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PitchDetector.h"

// One pitch measurement (every 10 ms).
struct PitchPoint
{
    double seconds = 0.0;     // since the plugin started analysing
    double ppq = 0.0;         // song position in quarter notes (valid if playing)
    float midi = 0.0f;        // fractional MIDI note, 0 = no pitch
    float levelDb = -100.0f;
    int pass = 0;             // increments each time playback starts or jumps back (a new take)
    bool playing = false;
};

// A note of the melody you are meant to sing (from a MIDI file), in song quarter notes.
struct GuideNote
{
    double start = 0.0, end = 0.0;
    int note = 60;
};

struct SongInfo
{
    double bpm = 120.0;
    int numerator = 4, denominator = 4;
    double ppq = 0.0;
    bool playing = false;
};

class PitchCoachProcessor : public juce::AudioProcessor,
                            private juce::Thread
{
public:
    PitchCoachProcessor();
    ~PitchCoachProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override    { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- for the editor
    void copyRecentPoints (std::vector<PitchPoint>& out, double lastSeconds) const;   // time view
    void copyPassPoints (std::vector<PitchPoint>& out, int passesBack) const;         // song view: latest passes
    SongInfo getSongInfo() const;
    PitchPoint getLatestPoint() const;
    void clearHistory();
    double getReferenceA() const { return refParam->load(); }

    // guide melody (optional): a MIDI file with the notes to sing, positioned in song time
    bool loadGuide (const juce::File& midiFile);
    void clearGuide();
    std::vector<GuideNote> getGuide() const;
    juce::String getGuideName() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void run() override;   // analysis thread

    struct BlockMarker { juce::int64 sample; double ppq, bpm; int num, den; bool playing; };

    // audio thread -> analysis thread
    static constexpr int fifoSize = 1 << 18;
    juce::AbstractFifo sampleFifo { fifoSize };
    std::vector<float> fifoBuffer = std::vector<float> ((size_t) fifoSize);
    juce::AbstractFifo markerFifo { 1024 };
    std::vector<BlockMarker> markerBuffer = std::vector<BlockMarker> (1024);
    std::atomic<juce::int64> samplesPushed { 0 };   // position in the sample stream the FIFO carries

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<bool> resetRequested { false };

    // analysis results
    mutable juce::CriticalSection historyLock;
    std::vector<PitchPoint> history;
    SongInfo song;
    static constexpr size_t maxPoints = 200000;   // about 33 minutes

    std::atomic<float>* refParam = nullptr;

    mutable juce::CriticalSection guideLock;
    std::vector<GuideNote> guide;
    juce::File guideFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCoachProcessor)
};
