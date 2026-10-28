#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "TakeAnalysis.h"

class PitchCoachEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit PitchCoachEditor (PitchCoachProcessor&);
    ~PitchCoachEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    void updateNow() { timerCallback(); }   // for offline snapshots

private:
    struct Stats
    {
        int held = 0, inTune = 0, wrong = 0, landings = 0, quickLandings = 0;
        float avgAbs = 0.0f, avgSigned = 0.0f, avgLandingMs = 0.0f;
        bool guided = false;
        juce::String watch;           // notes you keep missing
    };

    void timerCallback() override;
    void computeStats();
    void chooseGuide();
    void drawCard (juce::Graphics&, juce::Rectangle<float>, const juce::String& title, const juce::String& value,
                   juce::Colour valueColour, const juce::String& sub) const;
    void drawPill (juce::Graphics&, const juce::String& text, float cx, float cy, juce::Colour colour, float size) const;
    float yForMidi (float midi) const;
    float xForPos (double pos) const;   // pos = ppq (song mode) or seconds (time mode)
    juce::Rectangle<float> graphArea() const;
    juce::Rectangle<float> headerArea() const;

    PitchCoachProcessor& coach;

    juce::ComboBox rootBox, scaleBox;
    juce::Slider refSlider;
    juce::Label refLabel;
    juce::TextButton guideButton { "Guide: none" }, clearButton { "Clear" };
    juce::ToggleButton previousToggle { "Previous take" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rootAttach, scaleAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> refAttach;
    std::unique_ptr<juce::FileChooser> chooser;

    std::vector<PitchPoint> points;     // latest (and previous) take, or the recent seconds
    std::vector<PitchPoint> current;    // latest take on screen
    TakeAnalysis::Result analysis;      // notes and slides of `current`
    std::vector<GuideNote> guide;
    int guideShift = 0;                 // octaves (in semitones) to draw the guide near your voice
    Stats stats;
    SongInfo song;
    PitchPoint latest;
    bool songMode = false, following = true;
    int latestPass = 0;
    double viewStart = 0.0, viewLength = 16.0;   // quarter notes (song mode) or seconds (time mode)
    float centreMidi = 57.0f;                    // A3
    static constexpr float spanSemis = 24.0f;
    double dragStartView = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchCoachEditor)
};
