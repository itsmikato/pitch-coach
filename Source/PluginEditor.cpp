#include "PluginEditor.h"
#include "Scale.h"

#include <map>

namespace
{
    const juce::Colour bg (0xff15131c), panel (0xff1f1c29), card (0xff26222f), grid (0xff2f2a3d),
                       text (0xfff1edf9), dim (0xff9a94ae), whiteKey (0xffd9d5e3), blackKey (0xff4a4458);
    constexpr double timeWindowSeconds = 10.0;
    constexpr int toolbarH = 42, headerH = 112, footerH = 30, keysW = 58;

    juce::Colour percentColour (float pct)
    {
        if (pct >= 85.0f) return juce::Colour (0xff3ddc84);
        if (pct >= 65.0f) return juce::Colour (0xffb5e655);
        if (pct >= 45.0f) return juce::Colour (0xffffb547);
        return juce::Colour (0xffff5a5a);
    }

    juce::String signedCents (float c) { return (c >= 0 ? "+" : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))) + juce::String (std::abs (juce::roundToInt (c))); }
}

PitchCoachEditor::PitchCoachEditor (PitchCoachProcessor& p)
    : AudioProcessorEditor (&p), coach (p)
{
    rootBox.addItemList (Scale::noteNames(), 1);
    scaleBox.addItemList (Scale::scaleNames(), 1);
    rootAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, "root", rootBox);
    scaleAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, "scale", scaleBox);

    refSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    refSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    refSlider.setTextValueSuffix (" Hz");
    refAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, "ref", refSlider);
    refLabel.setText ("A4", juce::dontSendNotification);
    refLabel.setJustificationType (juce::Justification::centredRight);
    refLabel.setColour (juce::Label::textColourId, dim);

    guideButton.onClick = [this] { chooseGuide(); };
    clearButton.onClick = [this] { coach.clearHistory(); points.clear(); current.clear(); analysis = {}; stats = {}; repaint(); };
    previousToggle.setToggleState (true, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &rootBox, &scaleBox, &refSlider, &refLabel, &guideButton, &clearButton, &previousToggle })
        addAndMakeVisible (c);

    setResizable (true, true);
    setResizeLimits (760, 460, 2400, 1400);
    setSize (1040, 620);
    startTimerHz (30);
}

PitchCoachEditor::~PitchCoachEditor() = default;

void PitchCoachEditor::chooseGuide()
{
    if (! coach.getGuideName().isEmpty())
    {
        juce::PopupMenu m;
        m.addItem (1, "Load another guide MIDI file...");
        m.addItem (2, "Remove guide");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&guideButton), [this] (int r)
        {
            if (r == 2) coach.clearGuide();
            if (r == 1) { coach.clearGuide(); chooseGuide(); }
        });
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Guide melody (MIDI file in song time)", juce::File(), "*.mid;*.midi");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile() && ! coach.loadGuide (f))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Pitch Coach",
                                                                          "Could not read that MIDI file.");
                          });
}

// ---------------------------------------------------------------- layout helpers
juce::Rectangle<float> PitchCoachEditor::headerArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop ((float) toolbarH).removeFromTop ((float) headerH).reduced (10.0f, 4.0f);
}

juce::Rectangle<float> PitchCoachEditor::graphArea() const
{
    auto r = getLocalBounds().toFloat().withTrimmedTop ((float) (toolbarH + headerH)).withTrimmedBottom ((float) footerH).reduced (10.0f, 6.0f);
    return r.withTrimmedLeft ((float) keysW);
}

float PitchCoachEditor::yForMidi (float midi) const
{
    const auto g = graphArea();
    const float low = centreMidi - spanSemis / 2.0f;
    return g.getBottom() - (midi - low + 0.5f) / spanSemis * g.getHeight();
}

float PitchCoachEditor::xForPos (double pos) const
{
    const auto g = graphArea();
    return g.getX() + (float) ((pos - viewStart) / viewLength) * g.getWidth();
}

// ---------------------------------------------------------------- data
void PitchCoachEditor::timerCallback()
{
    song = coach.getSongInfo();
    latest = coach.getLatestPoint();
    guide = coach.getGuide();
    const auto guideName = coach.getGuideName();
    guideButton.setButtonText (guideName.isEmpty() ? "Guide: none" : "Guide: " + guideName);

    const double beatsPerBar = song.numerator * 4.0 / juce::jmax (1, song.denominator);
    const int bars = (int) coach.apvts.getRawParameterValue ("bars")->load();

    std::vector<PitchPoint> take;
    coach.copyPassPoints (take, previousToggle.getToggleState() ? 1 : 0);
    songMode = ! take.empty();

    if (songMode)
    {
        points = std::move (take);
        latestPass = points.back().pass;
        viewLength = bars * beatsPerBar;
        if (song.playing)
            following = true;
        if (following && song.playing)
            viewStart = song.ppq - viewLength * 0.75;
        else if (following)
        {
            double lastPpq = 0.0;
            for (auto it = points.rbegin(); it != points.rend(); ++it)
                if (it->pass == latestPass) { lastPpq = it->ppq; break; }
            viewStart = lastPpq - viewLength * 0.75;
        }
    }
    else
    {
        coach.copyRecentPoints (points, timeWindowSeconds);
        viewLength = timeWindowSeconds;
        viewStart = (points.empty() ? 0.0 : points.back().seconds) - timeWindowSeconds;
    }

    // latest take on screen (plus a little before, so a slide into the first note is complete)
    current.clear();
    for (const auto& p : points)
    {
        const double pos = songMode ? p.ppq : p.seconds;
        if ((! songMode || p.pass == latestPass) && pos >= viewStart - 1.0 && pos <= viewStart + viewLength)
            current.push_back (p);
    }

    // vertical range follows the singing; the guide is drawn in the octave you sing in
    std::vector<float> sung;
    for (const auto& p : current)
        if (p.midi > 0.0f) sung.push_back (p.midi);
    const float sungMedian = sung.empty() ? centreMidi : TakeAnalysis::median (sung);
    if (! sung.empty())
        centreMidi += (sungMedian + 1.0f - centreMidi) * 0.08f;
    std::vector<float> guideInView;
    for (const auto& g : guide)
        if (g.end >= viewStart && g.start <= viewStart + viewLength) guideInView.push_back ((float) g.note);
    guideShift = guideInView.empty() || sung.empty() ? 0
               : 12 * (int) std::lround ((sungMedian - TakeAnalysis::median (guideInView)) / 12.0f);
    if (guideInView.size() >= 1 && sung.empty())
        centreMidi += (TakeAnalysis::median (guideInView) + (float) guideShift - centreMidi) * 0.08f;

    const int root = (int) coach.apvts.getRawParameterValue ("root")->load();
    const int scale = (int) coach.apvts.getRawParameterValue ("scale")->load();
    analysis = TakeAnalysis::analyse (current, root, scale, guide.empty() ? nullptr : &guide);
    computeStats();
    repaint();
}

void PitchCoachEditor::computeStats()
{
    stats = {};
    float sumAbs = 0.0f, sumSigned = 0.0f, landingSum = 0.0f;
    std::map<int, std::pair<float, int>> perNote;   // target note -> (sum of cents, count)
    juce::StringArray wrongNotes;

    for (const auto& n : analysis.notes)
    {
        const auto& p = current[(size_t) n.first];
        if (! n.held || (songMode ? p.ppq : p.seconds) < viewStart) continue;
        ++stats.held;
        stats.guided = stats.guided || n.fromGuide;
        if (n.wrongNote)
        {
            ++stats.wrong;
            wrongNotes.add (Scale::noteName (juce::roundToInt (n.midi)) + " instead of " + Scale::noteName (n.target));
            continue;   // a wrong note is not a tuning error; listed separately
        }
        sumAbs += std::abs (n.cents);
        sumSigned += n.cents;
        if (std::abs (n.cents) <= 20.0f) ++stats.inTune;
        auto& acc = perNote[n.target];
        acc.first += n.cents;
        ++acc.second;
        if (n.landingMs >= 0.0f)
        {
            ++stats.landings;
            landingSum += n.landingMs;
            if (n.landingMs <= 100.0f) ++stats.quickLandings;
        }
    }
    const int tuned = stats.held - stats.wrong;
    stats.avgAbs = tuned > 0 ? sumAbs / (float) tuned : 0.0f;
    stats.avgSigned = tuned > 0 ? sumSigned / (float) tuned : 0.0f;
    stats.avgLandingMs = stats.landings > 0 ? landingSum / (float) stats.landings : 0.0f;

    // notes you keep missing: largest average error, at least 20 cents
    std::vector<std::pair<float, juce::String>> misses;
    for (const auto& [note, acc] : perNote)
    {
        const float mean = acc.first / (float) acc.second;
        if (std::abs (mean) >= 20.0f)
            misses.push_back ({ std::abs (mean), Scale::noteName (note) + " " + signedCents (mean) + " ct ("
                                                  + (mean < 0 ? "flat" : "sharp") + (acc.second > 1 ? ", " + juce::String (acc.second) + "x)" : ")") });
    }
    std::sort (misses.begin(), misses.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
    juce::StringArray parts;
    for (size_t i = 0; i < juce::jmin<size_t> (3, misses.size()); ++i) parts.add (misses[i].second);
    for (int i = 0; i < juce::jmin (2, wrongNotes.size()); ++i) parts.add ("wrong note: " + wrongNotes[i]);
    stats.watch = parts.joinIntoString ("     ");
}

// ---------------------------------------------------------------- drawing
void PitchCoachEditor::drawCard (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title, const juce::String& value,
                                 juce::Colour valueColour, const juce::String& sub) const
{
    g.setColour (card);
    g.fillRoundedRectangle (r, 10.0f);
    r = r.reduced (16.0f, 10.0f);
    g.setColour (dim);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (title, r.removeFromTop (18.0f), juce::Justification::centredLeft);
    g.setColour (valueColour);
    g.setFont (juce::FontOptions (36.0f, juce::Font::bold));
    g.drawText (value, r.removeFromTop (44.0f), juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (juce::FontOptions (14.0f));
    g.drawFittedText (sub, r.toNearestInt(), juce::Justification::topLeft, 2);
}

void PitchCoachEditor::drawPill (juce::Graphics& g, const juce::String& t, float cx, float cy, juce::Colour colour, float size) const
{
    juce::Font f (juce::FontOptions (size, juce::Font::bold));
    const float w = juce::GlyphArrangement::getStringWidth (f, t) + 10.0f, h = size + 6.0f;
    const juce::Rectangle<float> r (cx - w / 2.0f, cy - h / 2.0f, w, h);
    g.setColour (bg.withAlpha (0.85f));
    g.fillRoundedRectangle (r, h / 2.0f);
    g.setColour (colour);
    g.setFont (f);
    g.drawText (t, r, juce::Justification::centred);
}

void PitchCoachEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    const int root = (int) coach.apvts.getRawParameterValue ("root")->load();
    const int scale = (int) coach.apvts.getRawParameterValue ("scale")->load();

    // ---------- header: live readout + three cards
    auto header = headerArea();
    auto live = header.removeFromLeft (250.0f);
    g.setColour (card);
    g.fillRoundedRectangle (live, 10.0f);
    live = live.reduced (16.0f, 10.0f);
    g.setColour (dim);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("NOW", live.removeFromTop (18.0f), juce::Justification::centredLeft);
    if (latest.midi > 0.0f)
    {
        int target = Scale::nearestNote (latest.midi, root, scale);
        if (songMode && song.playing && ! guide.empty())
            if (const auto* gn = TakeAnalysis::guideAt (guide, latest.ppq))
                target = gn->note + 12 * (int) std::lround ((latest.midi - (float) gn->note) / 12.0f);
        const float cents = (latest.midi - (float) target) * 100.0f;
        auto row = live.removeFromTop (44.0f);
        g.setColour (text);
        g.setFont (juce::FontOptions (38.0f, juce::Font::bold));
        g.drawText (Scale::noteName (target), row.removeFromLeft (96.0f), juce::Justification::centredLeft);
        g.setColour (std::abs (cents) > 150.0f ? dim : Scale::centsColour (cents));
        g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
        g.drawText (signedCents (cents) + " ct", row, juce::Justification::centredLeft);
        auto bar = live.removeFromTop (14.0f).withTrimmedTop (2.0f);
        g.setColour (grid);
        g.fillRoundedRectangle (bar, 4.0f);
        g.setColour (dim);
        g.fillRect (bar.getCentreX() - 0.75f, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f);
        const float x = bar.getCentreX() + juce::jlimit (-50.0f, 50.0f, cents) / 50.0f * bar.getWidth() / 2.0f;
        g.setColour (Scale::centsColour (cents));
        g.fillRoundedRectangle (x - 5.0f, bar.getY() - 4.0f, 10.0f, bar.getHeight() + 8.0f, 3.0f);
    }
    else
    {
        g.setColour (dim);
        g.setFont (juce::FontOptions (26.0f));
        g.drawText ("sing...", live.removeFromTop (44.0f), juce::Justification::centredLeft);
    }

    header.removeFromLeft (10.0f);
    const float cw = (header.getWidth() - 20.0f) / 3.0f;
    auto c1 = header.removeFromLeft (cw), c2 = header.withTrimmedLeft (10.0f).removeFromLeft (cw),
         c3 = header.withTrimmedLeft (20.0f + cw).removeFromLeft (cw);
    const int tuned = stats.held - stats.wrong;
    if (stats.held > 0 && tuned > 0)
    {
        const float pct = 100.0f * (float) stats.inTune / (float) tuned;
        drawCard (g, c1, stats.guided ? "ON TARGET" : "IN TUNE", juce::String (juce::roundToInt (pct)) + "%", percentColour (pct),
                  "of " + juce::String (tuned) + " held notes within 20 ct"
                      + (stats.wrong > 0 ? "  |  " + juce::String (stats.wrong) + " wrong note" + (stats.wrong > 1 ? "s" : "") : juce::String())
                      + "\ngoal: 85%+");
        const juce::String tendency = std::abs (stats.avgSigned) < 5.0f ? "no flat/sharp tendency"
                                    : juce::String ("tends ") + (stats.avgSigned < 0 ? "flat " : "sharp ") + signedCents (stats.avgSigned) + " ct";
        drawCard (g, c2, "AVERAGE OFF", juce::String (juce::roundToInt (stats.avgAbs)) + " ct", Scale::centsColour (stats.avgAbs),
                  tendency + "\ngoal: 10-15 ct");
        if (stats.landings > 0)
            drawCard (g, c3, "LANDING", juce::String (juce::roundToInt (stats.avgLandingMs)) + " ms", TakeAnalysis::slideColour (stats.avgLandingMs),
                      juce::String (juce::roundToInt (100.0f * (float) stats.quickLandings / (float) stats.landings)) + "% of notes within 100 ms\ngoal: under 100 ms");
        else
            drawCard (g, c3, "LANDING", "-", dim, "time to arrive on a note\ngoal: under 100 ms");
    }
    else
    {
        const juce::String hint = songMode ? "Play the song and sing" : "Sing - or play the song in Live";
        drawCard (g, c1, "IN TUNE", "-", dim, "held notes within 20 ct\n" + hint);
        drawCard (g, c2, "AVERAGE OFF", "-", dim, "how far your held notes are off\nand whether you tend flat or sharp");
        drawCard (g, c3, "LANDING", "-", dim, "how quickly you arrive on a note\ngoal: under 100 ms");
    }

    // ---------- graph background, keyboard, lanes
    const auto ga = graphArea();
    g.setColour (panel);
    g.fillRoundedRectangle (ga.withTrimmedLeft (-(float) keysW).expanded (2.0f), 10.0f);
    g.saveState();
    g.reduceClipRegion (ga.withTrimmedLeft (-(float) keysW).toNearestInt());

    const float low = centreMidi - spanSemis / 2.0f, high = centreMidi + spanSemis / 2.0f;
    const float rowH = ga.getHeight() / spanSemis;
    for (int n = (int) std::floor (low) - 1; n <= (int) std::ceil (high) + 1; ++n)
    {
        const float y = yForMidi ((float) n);
        const auto row = juce::Rectangle<float> (ga.getX(), y - rowH / 2.0f, ga.getWidth(), rowH);
        const bool inScale = Scale::isInScale (n, root, scale);
        const bool isRoot = ((n - root) % 12 + 12) % 12 == 0;
        const int pc = ((n % 12) + 12) % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;

        if (inScale)
        {
            g.setColour (juce::Colours::white.withAlpha (isRoot ? 0.07f : 0.035f));
            g.fillRect (row);
        }
        g.setColour (grid.withAlpha (0.6f));
        g.drawHorizontalLine ((int) (y + rowH / 2.0f), ga.getX(), ga.getRight());

        // piano key
        auto key = juce::Rectangle<float> (ga.getX() - (float) keysW, y - rowH / 2.0f, (float) keysW - 4.0f, rowH).reduced (0.0f, 0.5f);
        g.setColour (black ? blackKey : whiteKey.withAlpha (inScale ? 1.0f : 0.6f));
        g.fillRoundedRectangle (black ? key.withTrimmedRight (16.0f) : key, 2.0f);
        if (inScale && rowH >= 9.0f)
        {
            g.setColour (black ? text : bg);
            g.setFont (juce::FontOptions (juce::jmin (13.0f, rowH - 1.0f), isRoot ? juce::Font::bold : juce::Font::plain));
            g.drawText (Scale::noteName (n), key.withTrimmedRight (black ? 18.0f : 4.0f), juce::Justification::centredRight);
        }
    }

    g.restoreState();
    g.saveState();
    g.reduceClipRegion (ga.toNearestInt());   // everything below stays inside the graph, off the keyboard

    // bar or second lines
    if (songMode)
    {
        const double beatsPerBar = song.numerator * 4.0 / juce::jmax (1, song.denominator);
        for (double b = std::floor (viewStart / beatsPerBar) * beatsPerBar; b <= viewStart + viewLength; b += beatsPerBar / song.numerator)
        {
            const float x = xForPos (b);
            const bool isBar = std::abs (std::fmod (b, beatsPerBar)) < 1.0e-6;
            g.setColour (grid.withAlpha (isBar ? 1.0f : 0.35f));
            g.drawVerticalLine ((int) x, ga.getY(), ga.getBottom());
            if (isBar)
            {
                g.setColour (dim);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText ("bar " + juce::String ((int) std::lround (b / beatsPerBar) + 1), (int) x + 6, (int) ga.getY() + 4, 70, 16,
                            juce::Justification::left);
            }
        }
    }
    else
    {
        for (double t = std::ceil (viewStart); t <= viewStart + viewLength; t += 1.0)
        {
            g.setColour (grid.withAlpha (0.45f));
            g.drawVerticalLine ((int) xForPos (t), ga.getY(), ga.getBottom());
        }
    }

    // guide melody: target bars
    if (songMode)
        for (const auto& gn : guide)
        {
            if (gn.end < viewStart || gn.start > viewStart + viewLength) continue;
            const float y = yForMidi ((float) (gn.note + guideShift));
            const juce::Rectangle<float> r (xForPos (gn.start), y - rowH * 0.42f, xForPos (gn.end) - xForPos (gn.start), rowH * 0.84f);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 4.0f, 1.2f);
        }

    auto posOf = [&] (const PitchPoint& p) { return songMode ? p.ppq : p.seconds; };
    auto connected = [] (const PitchPoint& a, const PitchPoint& b)
    {
        return a.midi > 0.0f && b.midi > 0.0f && b.seconds - a.seconds < TakeAnalysis::maxGapSeconds;
    };

    // previous take: thin grey line
    if (songMode && previousToggle.getToggleState())
    {
        g.setColour (juce::Colours::white.withAlpha (0.2f));
        const PitchPoint* prev = nullptr;
        for (const auto& p : points)
        {
            if (p.pass == latestPass) { prev = nullptr; continue; }
            if (prev != nullptr && connected (*prev, p))
                g.drawLine (xForPos (posOf (*prev)), yForMidi (prev->midi), xForPos (posOf (p)), yForMidi (p.midi), 2.0f);
            prev = p.midi > 0.0f ? &p : nullptr;
        }
    }

    // latest take: notes coloured by tuning, slides by how quickly they land
    std::vector<float> noteCents (current.size(), 0.0f);
    std::vector<char> noteWrong (current.size(), 0);
    for (const auto& n : analysis.notes)
        for (int i = n.first; i <= n.last; ++i) { noteCents[(size_t) i] = n.cents; noteWrong[(size_t) i] = n.wrongNote; }
    for (size_t i = 1; i < current.size(); ++i)
    {
        const auto& a = current[i - 1];
        const auto& b = current[i];
        if (! connected (a, b)) continue;
        const bool isNote = analysis.kind[i] == TakeAnalysis::Kind::note && analysis.kind[i - 1] == TakeAnalysis::Kind::note;
        const auto colour = isNote ? (noteWrong[i] ? juce::Colour (0xffff5a5a) : Scale::centsColour (noteCents[i]))
                                   : TakeAnalysis::slideColour (analysis.kind[i] == TakeAnalysis::Kind::slide ? analysis.slideMs[i]
                                                                                                             : analysis.slideMs[i - 1]);
        g.setColour (colour);
        g.drawLine (xForPos (posOf (a)), yForMidi (a.midi), xForPos (posOf (b)), yForMidi (b.midi), isNote ? 4.0f : 2.5f);
    }

    // labels on held notes: cents above, landing time below
    for (const auto& n : analysis.notes)
    {
        if (! n.held) continue;
        const float x = xForPos ((posOf (current[(size_t) n.first]) + posOf (current[(size_t) n.last])) * 0.5);
        const float y = yForMidi (n.midi);
        if (n.wrongNote)
            drawPill (g, "wrong: " + Scale::noteName (n.target), x, y - 20.0f, juce::Colour (0xffff5a5a), 13.0f);
        else
            drawPill (g, signedCents (n.cents), x, y - 20.0f, Scale::centsColour (n.cents), 14.0f);
        if (n.landingMs >= 20.0f)
            drawPill (g, juce::String (juce::roundToInt (n.landingMs)) + " ms", x, y + 19.0f, TakeAnalysis::slideColour (n.landingMs), 12.0f);
    }

    // playhead
    if (songMode && song.playing)
    {
        g.setColour (juce::Colours::white.withAlpha (0.65f));
        g.drawVerticalLine ((int) xForPos (song.ppq), ga.getY(), ga.getBottom());
    }
    g.restoreState();

    // ---------- footer: what to watch + colour legend
    auto footer = getLocalBounds().toFloat().removeFromBottom ((float) footerH).reduced (12.0f, 4.0f);
    auto legend = footer.removeFromRight (470.0f);
    g.setFont (juce::FontOptions (13.0f));
    auto dot = [&] (juce::Colour c, const juce::String& label)
    {
        g.setColour (c);
        g.fillEllipse (legend.getX(), legend.getCentreY() - 5.0f, 10.0f, 10.0f);
        g.setColour (dim);
        g.drawText (label, legend.withTrimmedLeft (14.0f).removeFromLeft (54.0f), juce::Justification::centredLeft);
        legend.removeFromLeft (64.0f);
    };
    g.setColour (dim);
    g.drawText ("notes", legend.removeFromLeft (42.0f), juce::Justification::centredLeft);
    dot (Scale::centsColour (0), "10 ct");
    dot (Scale::centsColour (15), "20 ct");
    dot (Scale::centsColour (30), "35 ct");
    g.setColour (dim);
    g.drawText ("slides", legend.removeFromLeft (44.0f), juce::Justification::centredLeft);
    dot (TakeAnalysis::slideColour (50), "100 ms");
    dot (TakeAnalysis::slideColour (300), "slow");

    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    if (stats.watch.isNotEmpty())
    {
        g.setColour (juce::Colour (0xffffb547));
        g.drawText ("Watch:", footer.removeFromLeft (58.0f), juce::Justification::centredLeft);
        g.setColour (text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (stats.watch, footer, juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (songMode ? "Drag the graph to look back, double-click to follow again, mouse wheel to zoom."
                             : "Tip: play your song in Live with Pitch Coach on the vocal track to see bars and your previous take.",
                    footer, juce::Justification::centredLeft);
    }
}

void PitchCoachEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (toolbarH).reduced (10, 7);
    rootBox.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (8);
    scaleBox.setBounds (bar.removeFromLeft (200));
    bar.removeFromLeft (8);
    refLabel.setBounds (bar.removeFromLeft (30));
    refSlider.setBounds (bar.removeFromLeft (190));
    clearButton.setBounds (bar.removeFromRight (70));
    bar.removeFromRight (8);
    previousToggle.setBounds (bar.removeFromRight (130));
    bar.removeFromRight (8);
    guideButton.setBounds (bar.removeFromRight (juce::jmin (260, juce::jmax (140, bar.getWidth() - 10))));
}

void PitchCoachEditor::mouseDown (const juce::MouseEvent&)
{
    dragStartView = viewStart;
}

void PitchCoachEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! songMode || song.playing)
        return;
    following = false;
    viewStart = dragStartView - e.getDistanceFromDragStartX() / graphArea().getWidth() * viewLength;
    repaint();
}

void PitchCoachEditor::mouseDoubleClick (const juce::MouseEvent&)
{
    following = true;
}

void PitchCoachEditor::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (auto* bars = coach.apvts.getParameter ("bars"))
    {
        const int now = (int) coach.apvts.getRawParameterValue ("bars")->load();
        const int next = juce::jlimit (1, 16, now + (wheel.deltaY > 0 ? -1 : 1));
        bars->setValueNotifyingHost (bars->convertTo0to1 ((float) next));
    }
}
