#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Scale.h"

#include <deque>

PitchCoachProcessor::PitchCoachProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("Pitch Coach analysis"),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    refParam = apvts.getRawParameterValue ("ref");
    history.reserve (maxPoints);
    startThread (juce::Thread::Priority::low);
}

PitchCoachProcessor::~PitchCoachProcessor()
{
    stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchCoachProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "root", 1 }, "Key", Scale::noteNames(), 9));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "scale", 2 }, "Scale", Scale::scaleNames(), Scale::defaultScale));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "ref", 1 }, "A4 reference",
                                                       NormalisableRange<float> (430.0f, 450.0f, 0.1f), 440.0f,
                                                       AudioParameterFloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "bars", 1 }, "Bars shown", 1, 16, 4));
    return layout;
}

bool PitchCoachProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void PitchCoachProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    resetRequested = true;
}

void PitchCoachProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    if (numSamples == 0 || numChannels == 0)
        return;

    // song position for this block (sample = position in the FIFO stream, so it stays aligned if the FIFO is full)
    BlockMarker marker { samplesPushed.load(), 0.0, 120.0, 4, 4, false };
    if (auto* playHead = getPlayHead())
        if (auto pos = playHead->getPosition())
        {
            marker.playing = pos->getIsPlaying() || pos->getIsRecording();
            if (auto ppq = pos->getPpqPosition()) marker.ppq = *ppq;
            if (auto bpm = pos->getBpm()) marker.bpm = *bpm;
            if (auto sig = pos->getTimeSignature()) { marker.num = sig->numerator; marker.den = sig->denominator; }
        }
    if (markerFifo.getFreeSpace() > 0)
    {
        const auto scope = markerFifo.write (1);
        if (scope.blockSize1 > 0) markerBuffer[(size_t) scope.startIndex1] = marker;
        else if (scope.blockSize2 > 0) markerBuffer[(size_t) scope.startIndex2] = marker;
    }

    // mono copy of the input for the analysis thread; the audio itself passes through untouched
    if (sampleFifo.getFreeSpace() >= numSamples)
    {
        const auto scope = sampleFifo.write (numSamples);
        auto fill = [&] (int start, int size, int offset)
        {
            for (int i = 0; i < size; ++i)
            {
                float s = 0.0f;
                for (int ch = 0; ch < numChannels; ++ch)
                    s += buffer.getReadPointer (ch)[offset + i];
                fifoBuffer[(size_t) (start + i)] = s / (float) numChannels;
            }
        };
        fill (scope.startIndex1, scope.blockSize1, 0);
        fill (scope.startIndex2, scope.blockSize2, scope.blockSize1);
        samplesPushed += numSamples;
    }
}

void PitchCoachProcessor::run()
{
    PitchDetector detector;
    std::vector<float> raw, dec;
    std::deque<BlockMarker> markers;
    double sr = 0.0, detSr = 0.0;
    int factor = 1, hop = 240, window = 1024;
    juce::int64 rawStart = 0;      // stream index of raw[0]
    juce::int64 decStart = 0;      // decimated stream index of dec[0]
    juce::int64 nextHop = 0;       // decimated stream index of the next analysis frame
    int pass = 0;
    bool lastPlaying = false;
    double lastPpq = 0.0;
    juce::int64 pendingSkip = 0;

    while (! threadShouldExit())
    {
        if (resetRequested.exchange (false) || sr != currentSampleRate.load())
        {
            sr = currentSampleRate.load();
            if (sr <= 0.0) { wait (20); continue; }
            factor = juce::jmax (1, (int) std::lround (sr / 24000.0));
            detSr = sr / factor;
            window = (int) std::lround (detSr * 0.0425);                      // ~43 ms analysis window
            detector.prepare (detSr, window);
            hop = juce::jmax (1, (int) std::lround (detSr * 0.010));          // 10 ms steps
            // restart the decimated stream on a sample boundary that divides by the new factor
            rawStart += (juce::int64) raw.size();
            raw.clear(); dec.clear();
            const auto skip = (factor - (int) (rawStart % factor)) % factor;
            pendingSkip = skip;
            decStart = (rawStart + skip) / factor;
            nextHop = decStart;
        }

        // markers
        while (markerFifo.getNumReady() > 0)
        {
            const auto scope = markerFifo.read (1);
            markers.push_back (markerBuffer[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)]);
        }

        // samples
        if (const int ready = sampleFifo.getNumReady(); ready > 0)
        {
            const auto scope = sampleFifo.read (ready);
            raw.insert (raw.end(), fifoBuffer.begin() + scope.startIndex1, fifoBuffer.begin() + scope.startIndex1 + scope.blockSize1);
            raw.insert (raw.end(), fifoBuffer.begin() + scope.startIndex2, fifoBuffer.begin() + scope.startIndex2 + scope.blockSize2);
        }

        // align to the decimation grid after a reset
        if (pendingSkip > 0 && ! raw.empty())
        {
            const auto n = (size_t) juce::jmin<juce::int64> (pendingSkip, (juce::int64) raw.size());
            raw.erase (raw.begin(), raw.begin() + (long) n);
            rawStart += (juce::int64) n;
            pendingSkip -= (juce::int64) n;
        }

        // decimate (simple averaging is plenty for voice pitch below ~1 kHz)
        const size_t whole = raw.size() / (size_t) factor * (size_t) factor;
        for (size_t i = 0; i < whole; i += (size_t) factor)
        {
            float s = 0.0f;
            for (int k = 0; k < factor; ++k) s += raw[i + (size_t) k];
            dec.push_back (s / (float) factor);
        }
        raw.erase (raw.begin(), raw.begin() + (long) whole);
        rawStart += (juce::int64) whole;

        const int frame = detector.frameSize();
        bool added = false;

        while (nextHop >= decStart && nextHop + frame <= decStart + (juce::int64) dec.size())
        {
            const float* x = dec.data() + (nextHop - decStart);
            float clarity = 0.0f;
            const float hz = detector.detect (x, clarity);

            double energy = 0.0;
            for (int i = 0; i < window; ++i) energy += (double) x[i] * x[i];
            const float levelDb = (float) (10.0 * std::log10 (energy / juce::jmax (1, window) + 1.0e-12));

            const juce::int64 rawIdx = (nextHop + window / 2) * factor;
            while (markers.size() > 1 && markers[1].sample <= rawIdx)
                markers.pop_front();

            PitchPoint p;
            p.seconds = (double) rawIdx / sr;
            p.levelDb = levelDb;
            if (! markers.empty())
            {
                const auto& m = markers.front();
                p.playing = m.playing;
                p.ppq = m.ppq + (double) (rawIdx - m.sample) / sr * m.bpm / 60.0;
                const juce::ScopedLock sl (historyLock);
                song = { m.bpm, m.num, m.den, p.ppq, m.playing };
            }
            if (hz > 0.0f && levelDb > -55.0f && clarity > 0.7f)
                p.midi = (float) (69.0 + 12.0 * std::log2 (hz / refParam->load()));

            if (p.playing && (! lastPlaying || p.ppq < lastPpq - 0.25))
                ++pass;
            lastPlaying = p.playing;
            lastPpq = p.ppq;
            p.pass = pass;

            {
                const juce::ScopedLock sl (historyLock);
                if (history.size() >= maxPoints)
                    history.erase (history.begin(), history.begin() + (long) (maxPoints / 10));
                history.push_back (p);
            }
            nextHop += hop;
            added = true;
        }

        // keep only what the next frames still need
        if (nextHop > decStart && dec.size() > (size_t) (frame * 4))
        {
            const auto drop = (size_t) juce::jmin<juce::int64> (nextHop - decStart, (juce::int64) dec.size());
            dec.erase (dec.begin(), dec.begin() + (long) drop);
            decStart += (juce::int64) drop;
        }

        if (! added)
            wait (5);
    }
}

void PitchCoachProcessor::copyRecentPoints (std::vector<PitchPoint>& out, double lastSeconds) const
{
    out.clear();
    const juce::ScopedLock sl (historyLock);
    if (history.empty()) return;
    const double from = history.back().seconds - lastSeconds;
    auto it = history.end();
    while (it != history.begin() && (it - 1)->seconds >= from) --it;
    out.assign (it, history.end());
}

void PitchCoachProcessor::copyPassPoints (std::vector<PitchPoint>& out, int passesBack) const
{
    out.clear();
    const juce::ScopedLock sl (historyLock);
    int latest = -1;
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (it->playing) { latest = it->pass; break; }
    if (latest < 0) return;
    auto it = history.end();
    while (it != history.begin() && (it - 1)->pass >= latest - passesBack) --it;
    for (; it != history.end(); ++it)
        if (it->playing) out.push_back (*it);
}

SongInfo PitchCoachProcessor::getSongInfo() const
{
    const juce::ScopedLock sl (historyLock);
    return song;
}

PitchPoint PitchCoachProcessor::getLatestPoint() const
{
    const juce::ScopedLock sl (historyLock);
    return history.empty() ? PitchPoint() : history.back();
}

void PitchCoachProcessor::clearHistory()
{
    const juce::ScopedLock sl (historyLock);
    history.clear();
}

bool PitchCoachProcessor::loadGuide (const juce::File& midiFile)
{
    juce::FileInputStream in (midiFile);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in))
        return false;
    const int tpq = mf.getTimeFormat();
    if (tpq <= 0)
        return false;   // SMPTE time is not supported

    std::vector<GuideNote> notes;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto* e = seq.getEventPointer (i);
            if (e->message.isNoteOn() && e->noteOffObject != nullptr)
                notes.push_back ({ e->message.getTimeStamp() / tpq, e->noteOffObject->message.getTimeStamp() / tpq,
                                   e->message.getNoteNumber() });
        }
    }
    std::sort (notes.begin(), notes.end(), [] (const GuideNote& a, const GuideNote& b) { return a.start < b.start; });

    const juce::ScopedLock sl (guideLock);
    guide = std::move (notes);
    guideFile = midiFile;
    return true;
}

void PitchCoachProcessor::clearGuide()
{
    const juce::ScopedLock sl (guideLock);
    guide.clear();
    guideFile = juce::File();
}

std::vector<GuideNote> PitchCoachProcessor::getGuide() const
{
    const juce::ScopedLock sl (guideLock);
    return guide;
}

juce::String PitchCoachProcessor::getGuideName() const
{
    const juce::ScopedLock sl (guideLock);
    return guideFile == juce::File() ? juce::String() : guideFile.getFileNameWithoutExtension();
}

void PitchCoachProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    {
        const juce::ScopedLock sl (guideLock);
        state.setProperty ("guidePath", guideFile.getFullPathName(), nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchCoachProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            const juce::String path = state.getProperty ("guidePath").toString();
            apvts.replaceState (state);
            if (path.isNotEmpty() && juce::File (path).existsAsFile())
                loadGuide (juce::File (path));
        }
}

juce::AudioProcessorEditor* PitchCoachProcessor::createEditor()
{
    return new PitchCoachEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchCoachProcessor();
}
