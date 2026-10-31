#include "SamplerPage.h"

using namespace juce;
using namespace UI;

// ===================================================================================== WaveformView

void WaveformView::setSample (SamplePtr s)
{
    sample = std::move (s);
    markers.clear();
    selectedSlice = -1;
    playhead = -1.0;
    viewStart = 0.0;
    viewLen = jmax (1, length());
    peaksDirty = true;
    repaint();
    if (onMarkersChanged) onMarkersChanged();
    if (onViewChanged) onViewChanged();
}

void WaveformView::restore (SamplePtr s, std::vector<int> m)
{
    if (s != sample) { setSample (std::move (s)); }
    markers = std::move (m);
    sortMarkers();
    repaint();
    if (onMarkersChanged) onMarkersChanged();
}

void WaveformView::setView (double start, double len)
{
    const double total = jmax (1, length());
    viewLen = jlimit (jmin (total, 256.0), total, len);
    viewStart = jlimit (0.0, total - viewLen, start);
    peaksDirty = true;
    repaint();
    if (onViewChanged) onViewChanged();
}

void WaveformView::zoomAround (double centre, double factor)
{
    const double newLen = viewLen * factor;
    const double rel = viewLen > 0 ? (centre - viewStart) / viewLen : 0.5;
    setView (centre - rel * newLen, newLen);
}

void WaveformView::setPlayhead (double s)
{
    if (s == playhead) return;
    playhead = s;
    if (s >= 0 && (s > viewStart + viewLen || s < viewStart))      // keep the playhead in view
        setView (s - viewLen * 0.05, viewLen);
    repaint();
}

bool WaveformView::addMarker (int pos)
{
    if (sample == nullptr || pos < 64 || pos >= length() - 64) return false;
    const int minGap = (int) (sample->sampleRate * 0.01);         // 10 ms
    for (int m : markers) if (std::abs (m - pos) < minGap) return false;
    markers.push_back (pos);
    sortMarkers();
    repaint();
    if (onMarkersChanged) onMarkersChanged();
    return true;
}

void WaveformView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (sample == nullptr) return;
    const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
        setView (viewStart - d * viewLen * 0.5, viewLen);              // scroll
    else
        zoomAround (sampleForX ((float) e.x), d > 0 ? 0.8 : 1.25);     // zoom around the mouse
}

std::vector<std::pair<int, int>> WaveformView::slices() const
{
    std::vector<std::pair<int, int>> out;
    if (sample == nullptr) return out;
    const int len = sample->audio.getNumSamples();
    int prev = 0;
    for (int m : markers) { if (m > prev && m < len) { out.push_back ({ prev, m }); prev = m; } }
    out.push_back ({ prev, len });
    return out;
}

void WaveformView::sortMarkers()
{
    std::sort (markers.begin(), markers.end());
    markers.erase (std::unique (markers.begin(), markers.end()), markers.end());
}

void WaveformView::buildPeaks()
{
    peaksDirty = false;
    const int w = jmax (1, getWidth());
    peakMin.assign ((size_t) w, 0.0f);
    peakMax.assign ((size_t) w, 0.0f);
    if (sample == nullptr) return;
    const auto& a = sample->audio;
    const int len = a.getNumSamples();
    const float* l = a.getReadPointer (0);
    const float* r = a.getReadPointer (1);
    for (int x = 0; x < w; ++x)
    {
        const int s0 = (int) (viewStart + viewLen * x / w);
        const int s1 = jmax (s0 + 1, (int) (viewStart + viewLen * (x + 1) / w));
        float mn = 1.0f, mx = -1.0f;
        for (int i = s0; i < s1 && i < len; ++i)
        {
            const float v = 0.5f * (l[i] + r[i]);
            mn = jmin (mn, v); mx = jmax (mx, v);
        }
        peakMin[(size_t) x] = mn > mx ? 0.0f : mn;
        peakMax[(size_t) x] = mn > mx ? 0.0f : mx;
    }
}

float WaveformView::xForSample (int s) const
{
    if (sample == nullptr) return 0.0f;
    return (float) ((s - viewStart) / jmax (1.0, viewLen) * getWidth());
}

int WaveformView::sampleForX (float x) const
{
    if (sample == nullptr) return 0;
    return jlimit (0, sample->audio.getNumSamples() - 1, (int) (viewStart + x / (double) jmax (1, getWidth()) * viewLen));
}

int WaveformView::markerNear (float x) const
{
    for (int i = 0; i < (int) markers.size(); ++i)
        if (std::abs (xForSample (markers[(size_t) i]) - x) < 6.0f) return i;
    return -1;
}

void WaveformView::paint (Graphics& g)
{
    g.setColour (panel.brighter (0.03f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    if (sample == nullptr)
    {
        g.setColour (muted);
        g.setFont (Font (14.0f));
        g.drawFittedText ("Drop a sample here (from the browser or Explorer), double-click one in the browser,\n"
                          "or record something above.", getLocalBounds().reduced (20), Justification::centred, 3);
        return;
    }
    if (peaksDirty) buildPeaks();

    auto sl = slices();
    // alternate slice shading + selected slice
    for (int i = 0; i < (int) sl.size(); ++i)
    {
        const float x0 = xForSample (sl[(size_t) i].first), x1 = xForSample (sl[(size_t) i].second);
        if (i == selectedSlice) { g.setColour (accent.withAlpha (0.18f)); g.fillRect (x0, 0.0f, x1 - x0, (float) getHeight()); }
        else if (i % 2 == 1)    { g.setColour (Colours::white.withAlpha (0.025f)); g.fillRect (x0, 0.0f, x1 - x0, (float) getHeight()); }
    }

    const float mid = getHeight() * 0.5f, scale = getHeight() * 0.46f;
    g.setColour (right.withAlpha (0.85f));
    for (int x = 0; x < (int) peakMin.size(); ++x)
        g.drawVerticalLine (x, mid - peakMax[(size_t) x] * scale, mid - peakMin[(size_t) x] * scale + 1.0f);

    // beat grid: faint beat lines, brighter bars with numbers
    if (bpm > 0.0)
    {
        const double beat = sample->sampleRate * 60.0 / bpm;
        const long long k0 = (long long) std::floor ((viewStart - gridOffset) / beat), k1 = (long long) std::ceil ((viewStart + viewLen - gridOffset) / beat);
        if (k1 - k0 < getWidth() / 3)
        {
            g.setFont (Font (10.0f));
            for (long long k = jmax (0LL, k0); k <= k1; ++k)
            {
                const float x = xForSample ((int) (gridOffset + k * beat));
                const bool bar = k % 4 == 0;
                g.setColour (Colours::white.withAlpha (bar ? 0.16f : 0.06f));
                g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
                if (bar) { g.setColour (muted); g.drawText (String (k / 4 + 1), (int) x + 3, getHeight() - 15, 30, 13, Justification::centredLeft); }
            }
        }
    }

    // markers with slice numbers
    g.setFont (Font (11.0f, Font::bold));
    for (int i = 0; i < (int) sl.size(); ++i)
    {
        const float x = xForSample (sl[(size_t) i].first);
        if (i > 0) { g.setColour (accent); g.drawLine (x, 0.0f, x, (float) getHeight(), 1.5f); }
        g.setColour (accent);
        g.fillRoundedRectangle (x + 2.0f, 3.0f, 22.0f, 15.0f, 3.0f);
        g.setColour (Colours::black);
        g.drawText (String (i + 1), (int) x + 2, 3, 22, 15, Justification::centred);
    }

    if (playhead >= 0.0)
    {
        const float x = xForSample ((int) playhead);
        g.setColour (Colours::white);
        g.drawLine (x, 0.0f, x, (float) getHeight(), 2.0f);
    }

    // zoom indicator
    if (viewLen < length() - 1)
    {
        g.setColour (muted);
        g.setFont (Font (11.0f));
        g.drawText (String (viewStart / sample->sampleRate, 2) + " - " + String ((viewStart + viewLen) / sample->sampleRate, 2) + " s",
                    getLocalBounds().reduced (8, 4), Justification::bottomRight);
    }
}

void WaveformView::mouseDown (const MouseEvent& e)
{
    if (sample == nullptr) return;
    const int m = markerNear ((float) e.x);
    if (e.mods.isPopupMenu())
    {
        if (m >= 0)
        {
            if (onEditStart) onEditStart();
            markers.erase (markers.begin() + m); repaint(); if (onMarkersChanged) onMarkersChanged();
        }
        return;
    }
    if (m >= 0) { if (onEditStart) onEditStart(); dragMarker = m; return; }

    // select (and hear) the slice under the mouse
    const int s = sampleForX ((float) e.x);
    auto sl = slices();
    for (int i = 0; i < (int) sl.size(); ++i)
        if (s >= sl[(size_t) i].first && s < sl[(size_t) i].second)
        {
            selectedSlice = i;
            repaint();
            if (onSliceClicked) onSliceClicked (i);
            break;
        }
}

void WaveformView::mouseDrag (const MouseEvent& e)
{
    if (dragMarker < 0 || sample == nullptr) return;
    int pos = jlimit (1, sample->audio.getNumSamples() - 1, sampleForX ((float) e.x));
    if (snap) pos = snap (pos);
    markers[(size_t) dragMarker] = pos;
    sortMarkers();
    dragMarker = (int) (std::find (markers.begin(), markers.end(), pos) - markers.begin());
    repaint();
    if (onMarkersChanged) onMarkersChanged();
}

void WaveformView::mouseDoubleClick (const MouseEvent& e)
{
    if (sample == nullptr || markerNear ((float) e.x) >= 0) return;
    if (onEditStart) onEditStart();
    int pos = jmax (1, sampleForX ((float) e.x));
    if (snap) pos = snap (pos);
    markers.push_back (pos);
    sortMarkers();
    repaint();
    if (onMarkersChanged) onMarkersChanged();
}

void WaveformView::filesDropped (const StringArray& files, int, int)
{
    if (! files.isEmpty() && onFileDropped) onFileDropped (File (files[0]));
}

void WaveformView::itemDropped (const SourceDetails&)
{
    auto sel = ctx.browserSelection();
    if (! sel.isEmpty() && onFileDropped) onFileDropped (sel[0]);
}

// ===================================================================================== chop helpers

std::vector<int> SamplerPage::equalSlices (int length, int count)
{
    std::vector<int> m;
    for (int i = 1; i < count; ++i) m.push_back ((int) ((int64) length * i / count));
    return m;
}

std::vector<int> SamplerPage::detectTransients (const SampleData& s, float sensitivity)
{
    // Energy-rise onset detector: frames of 256 samples, compare each frame's level (dB) with
    // the average of the previous few frames. Higher sensitivity = smaller rise needed.
    std::vector<int> out;
    const int len = s.audio.getNumSamples();
    const int hop = 256;
    const int frames = len / hop;
    if (frames < 4) return out;
    const float* l = s.audio.getReadPointer (0);
    const float* r = s.audio.getReadPointer (1);

    std::vector<float> db ((size_t) frames);
    float peakDb = -100.0f;
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0;
        for (int i = f * hop; i < (f + 1) * hop; ++i) { const float v = 0.5f * (l[i] + r[i]); sum += v * v; }
        db[(size_t) f] = Decibels::gainToDecibels ((float) std::sqrt (sum / hop), -100.0f);
        peakDb = jmax (peakDb, db[(size_t) f]);
    }
    const float riseNeeded = jmap (jlimit (0.0f, 1.0f, sensitivity), 18.0f, 3.0f);
    const float floorDb = peakDb - 45.0f;
    const int minGap = (int) (0.07 * s.sampleRate / hop) + 1;    // 70 ms between hits
    int last = -minGap;
    for (int f = 2; f < frames; ++f)
    {
        float avg = 0.0f; int n = 0;
        for (int k = jmax (0, f - 4); k < f; ++k) { avg += db[(size_t) k]; ++n; }
        avg /= (float) jmax (1, n);
        if (db[(size_t) f] > floorDb && db[(size_t) f] - avg > riseNeeded && f - last >= minGap)
        {
            // refine: first sample in this frame (or the one before) above half of the frame's peak
            int start = (f - 1) * hop;
            float framePeak = 0.0f;
            for (int i = start; i < (f + 1) * hop; ++i) framePeak = jmax (framePeak, std::abs (0.5f * (l[i] + r[i])));
            for (int i = start; i < (f + 1) * hop; ++i)
                if (std::abs (0.5f * (l[i] + r[i])) >= framePeak * 0.5f) { start = i; break; }
            start = jmax (0, start - 32);     // keep a hair of pre-attack
            if (start > hop) out.push_back (start);
            last = f;
        }
    }
    return out;
}


std::vector<int> SamplerPage::gridSlices (int length, double sampleRate, double bpm, double beatsPerSlice, double offset)
{
    std::vector<int> m;
    if (bpm <= 0.0 || beatsPerSlice <= 0.0) return m;
    const double step = sampleRate * 60.0 / bpm * beatsPerSlice;
    if (step < 64.0) return m;
    if (offset > 64.0) m.push_back ((int) offset);
    for (double p = offset + step; p < length - 64.0; p += step) m.push_back ((int) std::round (p));
    return m;
}

double SamplerPage::bpmFromName (const String& name)
{
    // "88bpm", "88 BPM", "bpm88", "88_bpm"
    const auto lower = name.toLowerCase();
    for (int at = lower.indexOf ("bpm"); at >= 0; at = lower.indexOf (at + 3, "bpm"))
    {
        auto digitsBefore = [&]
        {
            int e = at; while (e > 0 && (lower[e - 1] == ' ' || lower[e - 1] == '_' || lower[e - 1] == '-')) --e;
            int b = e;  while (b > 0 && (CharacterFunctions::isDigit (lower[b - 1]) || lower[b - 1] == '.')) --b;
            return lower.substring (b, e);
        };
        auto digitsAfter = [&]
        {
            int b = at + 3; while (b < lower.length() && (lower[b] == ' ' || lower[b] == '_' || lower[b] == '-')) ++b;
            int e = b;      while (e < lower.length() && (CharacterFunctions::isDigit (lower[e]) || lower[e] == '.')) ++e;
            return lower.substring (b, e);
        };
        for (auto d : { digitsBefore(), digitsAfter() })
        {
            const double v = d.getDoubleValue();
            if (d.isNotEmpty() && v >= 50.0 && v <= 250.0) return v;
        }
    }
    return 0.0;
}

double SamplerPage::detectBpm (const SampleData& s)
{
    // Onset strength (rise in log energy per 512-sample frame), then autocorrelation over 70-180 BPM.
    const int hop = 512;
    const int len = s.audio.getNumSamples();
    const int frames = len / hop;
    const double fr = s.sampleRate / hop;
    if (frames < 32) return 0.0;
    const float* l = s.audio.getReadPointer (0);
    const float* r = s.audio.getReadPointer (1);
    std::vector<float> en ((size_t) frames), on ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        double sum = 0;
        for (int i = f * hop; i < (f + 1) * hop; ++i) { const float v = 0.5f * (l[i] + r[i]); sum += v * v; }
        en[(size_t) f] = std::log (1.0e-6f + (float) std::sqrt (sum / hop));
    }
    for (int f = 1; f < frames; ++f) on[(size_t) f] = jmax (0.0f, en[(size_t) f] - en[(size_t) f - 1]);
    float mean = 0; for (auto v : on) mean += v; mean /= (float) frames;
    for (auto& v : on) v = jmax (0.0f, v - mean);

    const int lagMin = (int) std::floor (fr * 60.0 / 180.0), lagMax = (int) std::ceil (fr * 60.0 / 70.0);
    if (lagMax >= frames / 2) return 0.0;
    double best = 0.0, bestLag = 0.0;
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        double acc = 0;
        for (int f = 0; f + lag < frames; ++f) acc += on[(size_t) f] * on[(size_t) f + lag];
        acc /= (frames - lag);
        if (acc > best) { best = acc; bestLag = lag; }
    }
    if (best <= 0.0) return 0.0;
    // refine around the best lag (parabolic) for a fractional value
    auto ac = [&] (int lag) { double a = 0; for (int f = 0; f + lag < frames; ++f) a += on[(size_t) f] * on[(size_t) f + lag]; return a / (frames - lag); };
    const int L = (int) bestLag;
    const double y0 = ac (L - 1), y1 = ac (L), y2 = ac (L + 1);
    const double den = y0 - 2 * y1 + y2;
    const double lag = std::abs (den) > 1e-12 ? L + 0.5 * (y0 - y2) / den : L;
    double bpm = 60.0 * fr / lag;

    // loops: if the length is a whole number of bars at a tempo close by, that's the tempo
    const double secs = len / s.sampleRate;
    for (int beats : { 4, 8, 16, 32, 64 })
    {
        const double b = 60.0 * beats / secs;
        if (std::abs (b - bpm) / bpm < 0.04) return std::round (b * 10.0) / 10.0;
    }
    return std::round (bpm * 10.0) / 10.0;
}

int SamplerPage::nearestZeroCrossing (const SampleData& s, int pos, int range)
{
    const int len = s.audio.getNumSamples();
    const float* l = s.audio.getReadPointer (0);
    const float* r = s.audio.getReadPointer (1);
    auto v = [&] (int i) { return l[i] + r[i]; };
    for (int d = 0; d <= range; ++d)
        for (int i : { pos - d, pos + d })
            if (i > 0 && i < len && ((v (i - 1) <= 0.0f && v (i) > 0.0f) || (v (i - 1) >= 0.0f && v (i) < 0.0f))) return i;
    return pos;
}

std::shared_ptr<SampleData> SamplerPage::normalized (const SampleData& s)
{
    auto out = std::make_shared<SampleData> (s);
    float peak = 0.0f;
    for (int c = 0; c < out->audio.getNumChannels(); ++c) peak = jmax (peak, out->audio.getMagnitude (c, 0, out->audio.getNumSamples()));
    if (peak > 1.0e-6f) out->audio.applyGain (0.98f / peak);      // -0.2 dB
    return out;
}

std::shared_ptr<SampleData> SamplerPage::trimmed (const SampleData& s, int& lead)
{
    const int len = s.audio.getNumSamples();
    float peak = 0.0f;
    for (int c = 0; c < s.audio.getNumChannels(); ++c) peak = jmax (peak, s.audio.getMagnitude (c, 0, len));
    const float thr = peak * 0.003f;                              // -50 dB below the peak
    auto loud = [&] (int i) { return std::abs (s.audio.getSample (0, i)) > thr || std::abs (s.audio.getSample (1, i)) > thr; };
    int a = 0, b = len - 1;
    while (a < len && ! loud (a)) ++a;
    while (b > a && ! loud (b)) --b;
    lead = 0;
    if (a >= b) return nullptr;
    a = jmax (0, a - 64);                                         // keep a hair before the sound
    b = jmin (len - 1, b + (int) (0.02 * s.sampleRate));          // and a little tail
    if (a == 0 && b == len - 1) return nullptr;                   // nothing to trim
    auto out = std::make_shared<SampleData>();
    out->file = s.file;
    out->sampleRate = s.sampleRate;
    out->audio.setSize (2, b - a + 1);
    for (int c = 0; c < 2; ++c) out->audio.copyFrom (c, 0, s.audio, c, a, b - a + 1);
    lead = a;
    return out;
}

std::vector<std::pair<int, int>> SamplerPage::patternSteps (const std::vector<int>& starts, double sampleRate,
                                                            double bpm, double stepsPerBeat, int maxSteps)
{
    std::vector<std::pair<int, int>> out;
    int last = -1;
    for (int i = 0; i < (int) starts.size(); ++i)
    {
        int step = i;
        if (bpm > 0.0)
            step = (int) std::round ((starts[(size_t) i] - starts.front()) / sampleRate * bpm / 60.0 * stepsPerBeat);
        if (step >= maxSteps) break;
        if (step <= last) continue;                               // two slices on one step: keep the first
        out.push_back ({ i, step });
        last = step;
    }
    return out;
}

// ===================================================================================== SamplerPage

static void styleSlider (Slider& s, int boxW = 56)
{
    s.setSliderStyle (Slider::LinearHorizontal);
    s.setTextBoxStyle (Slider::TextBoxRight, false, boxW, 20);
    s.setColour (Slider::thumbColourId, accent);
    s.setColour (Slider::trackColourId, accent.withAlpha (0.5f));
    s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    s.setColour (Slider::textBoxTextColourId, text);
}

SamplerPage::SamplerPage (EditorContext& c) : ctx (c), proc (c.getProc())
{
    // ---- file + undo
    styleLabel (fileLabel, 13.0f, text);
    fileLabel.setText ("No sample loaded", dontSendNotification);
    addAndMakeVisible (fileLabel);
    for (auto* b : { &loadButton, &undoButton, &redoButton }) { styleButton (*b); addAndMakeVisible (b); }
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Load a sample to chop", proc.browserRoot, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) loadFile (fc.getResult()); });
    };
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };

    // ---- record
    sourceLabel.setText ("Record from", dontSendNotification); styleLabel (sourceLabel, 11.0f); addAndMakeVisible (sourceLabel);
    sourceBox.addItem ("FlipKit64 output (resample)", 1);
    sourceBox.addItem ("Audio input", 2);
    sourceBox.setSelectedId (proc.recSource.load() + 1, dontSendNotification);
    sourceBox.onChange = [this] { proc.recSource = sourceBox.getSelectedId() - 1; };
    sourceBox.setTooltip ("Resample records whatever FlipKit64 plays (pads, sequencer). Audio input needs your DAW or audio "
                          "device to give FlipKit64 an input. Recordings go to Documents\\FlipKit64\\Recordings.");
    addAndMakeVisible (sourceBox);
    thresholdLabel.setText ("Start when louder than", dontSendNotification); styleLabel (thresholdLabel, 11.0f); addAndMakeVisible (thresholdLabel);
    styleSlider (thresholdSlider, 52);
    thresholdSlider.setRange (-60.0, 0.0, 1.0);
    thresholdSlider.setValue (-60.0, dontSendNotification);
    thresholdSlider.textFromValueFunction = [] (double v) { return v <= -59.5 ? String ("Off") : String ((int) v) + " dB"; };
    thresholdSlider.updateText();
    thresholdSlider.onValueChange = [this]
    {
        const double v = thresholdSlider.getValue();
        proc.recThreshold = v <= -59.5 ? 0.0f : Decibels::decibelsToGain ((float) v);
    };
    addAndMakeVisible (thresholdSlider);
    maxLenLabel.setText ("Max", dontSendNotification); styleLabel (maxLenLabel, 11.0f); addAndMakeVisible (maxLenLabel);
    styleSlider (maxLenSlider, 44);
    maxLenSlider.setRange (5.0, 120.0, 5.0);
    maxLenSlider.setValue (30.0, dontSendNotification);
    maxLenSlider.setTextValueSuffix (" s");
    addAndMakeVisible (maxLenSlider);
    styleButton (recButton); styleButton (stopButton);
    recButton.setColour (TextButton::buttonColourId, rec.darker (0.3f));
    recButton.onClick = [this]
    {
        if (proc.recSource.load() == 1 && ! proc.inputBusAvailable())
        {
            recStatus.setText ("No audio input here", dontSendNotification);
            return;
        }
        proc.armRecording (maxLenSlider.getValue());
    };
    stopButton.onClick = [this] { proc.stopRecording(); };
    addAndMakeVisible (recButton);
    addAndMakeVisible (stopButton);
    styleLabel (recStatus, 12.0f, text);
    addAndMakeVisible (recStatus);

    // ---- chop row
    chopLabel.setText ("Chop", dontSendNotification); styleLabel (chopLabel, 11.0f); addAndMakeVisible (chopLabel);
    for (int i = 0; i < 4; ++i)
    {
        auto& b = modeButtons[i];
        styleButton (b);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (701);
        b.onClick = [this, i] { if (modeButtons[i].getToggleState()) { chopModeId = i + 1; updateChopControls(); } };
        addAndMakeVisible (b);
    }
    modeButtons[0].setTooltip ("Cut where each hit starts");
    modeButtons[1].setTooltip ("Cut into equal pieces");
    modeButtons[2].setTooltip ("Cut on the beat: bars, beats, 8ths or 16ths at the loop's tempo");
    modeButtons[3].setTooltip ("Place markers yourself: double-click the waveform");
    modeButtons[0].setToggleState (true, dontSendNotification);
    styleLabel (chopAmountLabel, 11.0f); addAndMakeVisible (chopAmountLabel);
    styleSlider (chopAmount, 44);
    addAndMakeVisible (chopAmount);
    bpmLabel.setText ("BPM", dontSendNotification); styleLabel (bpmLabel, 11.0f); addAndMakeVisible (bpmLabel);
    bpmEdit.setEditable (true);
    bpmEdit.setJustificationType (Justification::centred);
    bpmEdit.setColour (Label::textColourId, text);
    bpmEdit.setColour (Label::outlineColourId, line);
    bpmEdit.setColour (Label::backgroundColourId, panel.brighter (0.06f));
    bpmEdit.setTooltip ("The loop's tempo (found from the file name or the audio). Click to type it.");
    bpmEdit.onTextChange = [this]
    {
        const double v = bpmEdit.getText().getDoubleValue();
        if (v >= 40.0 && v <= 250.0) { pushUndo(); setBpm (v); }
        else setBpm (bpm);
    };
    addAndMakeVisible (bpmEdit);
    const double gb[4] = { 4.0, 1.0, 0.5, 0.25 };
    for (int i = 0; i < 4; ++i)
    {
        auto& b = gridButtons[i];
        styleButton (b);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (702);
        const double beats = gb[i];
        b.onClick = [this, i, beats] { if (gridButtons[i].getToggleState()) gridBeats = beats; };
        addAndMakeVisible (b);
    }
    gridButtons[1].setToggleState (true, dontSendNotification);
    for (auto* b : { &chopButton, &clearMarkersButton, &tuneDown, &tuneUp, &normalizeButton, &trimButton }) { styleButton (*b); addAndMakeVisible (b); }
    chopButton.setColour (TextButton::buttonColourId, accent.darker (0.35f));
    chopButton.onClick = [this] { doChop(); };
    clearMarkersButton.onClick = [this] { pushUndo(); wave.markers.clear(); wave.selectedSlice = -1; wave.repaint(); markersChanged(); };
    styleLabel (tuneText, 12.0f, text);
    tuneText.setJustificationType (Justification::centred);
    tuneText.setTooltip ("Tune the slices (applied to the pads they go to)");
    addAndMakeVisible (tuneText);
    tuneDown.onClick = [this] { pushUndo(); tuneSemis = jmax (-24, tuneSemis - 1); for (int i = 0; i < (int) assigned.size(); ++i) pushToPad (i); updateSlicePanel(); };
    tuneUp.onClick   = [this] { pushUndo(); tuneSemis = jmin (24, tuneSemis + 1);  for (int i = 0; i < (int) assigned.size(); ++i) pushToPad (i); updateSlicePanel(); };
    normalizeButton.setTooltip ("Make the whole sample as loud as it can go without clipping (saves a copy)");
    trimButton.setTooltip ("Cut the silence off the start and end (saves a copy)");
    normalizeButton.onClick = [this] { normalizeSample(); };
    trimButton.onClick = [this] { trimSample(); };

    // ---- waveform
    wave.onMarkersChanged = [this] { markersChanged(); };
    wave.onEditStart = [this] { pushUndo(); };
    wave.onSliceClicked = [this] (int i) { selectSlice (i, true); };
    wave.onFileDropped = [this] (const File& f) { loadFile (f); };
    wave.snap = [this] (int pos) { auto s = wave.getSample(); return s != nullptr && snapToggle.getToggleState() ? nearestZeroCrossing (*s, pos) : pos; };
    addAndMakeVisible (wave);
    styleLabel (sliceInfo, 12.0f, text);
    addAndMakeVisible (sliceInfo);
    loopToggle.setColour (ToggleButton::tickColourId, accent);
    loopToggle.setTooltip ("Keep playing the selected slice while you nudge and shape it");
    loopToggle.onClick = [this] { loopNextMs = 0.0; };
    addAndMakeVisible (loopToggle);
    waveScroll.addListener (this);
    waveScroll.setAutoHide (false);
    waveScroll.setColour (ScrollBar::thumbColourId, accent.withAlpha (0.6f));
    addAndMakeVisible (waveScroll);
    for (auto* b : { &zoomOutButton, &zoomInButton, &zoomFitButton }) { styleButton (*b); addAndMakeVisible (b); }
    zoomInButton.onClick  = [this] { wave.zoomAround (wave.viewStart + wave.viewLen * 0.5, 0.5); };
    zoomOutButton.onClick = [this] { wave.zoomAround (wave.viewStart + wave.viewLen * 0.5, 2.0); };
    zoomFitButton.onClick = [this] { wave.setView (0.0, wave.length()); };
    wave.onViewChanged = [this] { syncScrollBar(); };

    // ---- slices -> pads
    for (auto* b : { &assignButton, &patternButton, &keysButton }) { styleButton (*b); addAndMakeVisible (b); }
    patternButton.setColour (TextButton::buttonColourId, accent);
    patternButton.setColour (TextButton::textColourOffId, Colours::black);
    keysButton.setColour (TextButton::buttonColourId, zoneCyan.withAlpha (0.25f));
    assignButton.setTooltip ("Put the slices on the pads, in order from the selected pad");
    patternButton.setTooltip ("Put the slices on the pads and write a pattern that plays them in order (the current pattern)");
    keysButton.setTooltip ("Put the slices on the pads and play them from a Keys track: one slice per key from C3 up");
    assignButton.onClick = [this] { assignToPads(); };
    patternButton.onClick = [this] { chopToPattern(); };
    keysButton.onClick = [this]
    {
        PopupMenu m;
        for (int t = 0; t < Keys::numTracks; ++t) m.addItem (t + 1, proc.keysTracks[t].name);
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (&keysButton),
                         [this] (int r) { if (r > 0) sendToKeys (r - 1); });
    };
    tapLabel.setText ("Tap chop", dontSendNotification); styleLabel (tapLabel, 11.0f); addAndMakeVisible (tapLabel);
    tapModeBox.addItem ("Off (just listen)", 1);
    tapModeBox.addItem ("Pad hits drop markers", 2);
    tapModeBox.addItem ("Pad hits chop onto that pad", 3);
    tapModeBox.setSelectedId (proc.tapMode.load() + 1, dontSendNotification);
    tapModeBox.onChange = [this] { proc.tapMode = tapModeBox.getSelectedId() - 1; };
    addAndMakeVisible (tapModeBox);
    styleButton (tapButton);
    tapButton.setColour (TextButton::buttonColourId, accent.darker (0.35f));
    tapButton.onClick = [this] { startStopTap(); };
    tapButton.setTooltip ("Play the sample and hit TempoPAD pads as it plays");
    addAndMakeVisible (tapButton);
    styleLabel (tapInfo, 11.0f, text);
    addAndMakeVisible (tapInfo);
    hint.setText ("Click the waveform to hear a slice; double-click adds a marker, drag moves it, right-click deletes it. "
                  "Wheel zooms, Shift+wheel scrolls. Click a pad above to start the slices there.", dontSendNotification);
    styleLabel (hint, 11.0f);
    hint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hint);

    // ---- selected slice
    selTitle.setFont (Font (13.0f, Font::bold));
    selTitle.setColour (Label::textColourId, text);
    addAndMakeVisible (selTitle);
    for (auto* b : { &hearButton, &startLeft, &startRight, &endLeft, &endRight }) { styleButton (*b); addAndMakeVisible (b); }
    hearButton.setColour (TextButton::buttonColourId, accent);
    hearButton.setColour (TextButton::textColourOffId, Colours::black);
    hearButton.onClick = [this] { auditionSlice (wave.selectedSlice); };
    startLeft.onClick  = [this] { nudge (true, -1); };
    startRight.onClick = [this] { nudge (true, +1); };
    endLeft.onClick    = [this] { nudge (false, -1); };
    endRight.onClick   = [this] { nudge (false, +1); };
    for (auto* b : { &startLeft, &startRight, &endLeft, &endRight }) b->setTooltip ("Move by 10 ms");
    startLabel.setText ("Start", dontSendNotification); endLabel.setText ("End", dontSendNotification);
    for (auto* l : { &startLabel, &endLabel }) { styleLabel (*l, 11.0f); addAndMakeVisible (l); }
    for (auto* l : { &startText, &endText }) { styleLabel (*l, 12.0f, text); l->setJustificationType (Justification::centred); addAndMakeVisible (l); }
    for (auto* t : { &snapToggle, &reverseToggle }) { t->setColour (ToggleButton::tickColourId, accent); addAndMakeVisible (t); }
    snapToggle.setToggleState (true, dontSendNotification);
    reverseToggle.onClick = [this]
    {
        const int i = wave.selectedSlice;
        if (refreshing || ! isPositiveAndBelow (i, (int) sliceSet.size())) return;
        pushUndo();
        sliceSet[(size_t) i].reverse = reverseToggle.getToggleState();
        pushToPad (i);
        auditionSlice (i);
    };
    gainLabel.setText ("Gain", dontSendNotification); fadeInLabel.setText ("Fade in", dontSendNotification); fadeOutLabel.setText ("Fade out", dontSendNotification);
    for (auto* l : { &gainLabel, &fadeInLabel, &fadeOutLabel }) { styleLabel (*l, 11.0f); addAndMakeVisible (l); }
    styleSlider (gainSlider, 60); styleSlider (fadeInSlider, 60); styleSlider (fadeOutSlider, 60);
    gainSlider.setRange (-24.0, 12.0, 0.1);
    gainSlider.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; };
    gainSlider.setDoubleClickReturnValue (true, 0.0);
    for (auto* s : { &fadeInSlider, &fadeOutSlider })
    {
        s->setRange (0.0, 1000.0, 1.0);
        s->setSkewFactorFromMidPoint (120.0);
        s->textFromValueFunction = [] (double v) { return v < 0.5 ? String ("Off") : String ((int) v) + " ms"; };
        s->setDoubleClickReturnValue (true, 0.0);
    }
    for (auto* sl : { &gainSlider, &fadeInSlider, &fadeOutSlider }) sl->updateText();
    auto bindSlider = [this] (Slider& s, std::function<void (SliceSet&, float)> set)
    {
        s.onDragStart = [this] { pushUndo(); };
        s.onValueChange = [this, &s, set]
        {
            const int i = wave.selectedSlice;
            if (refreshing || ! isPositiveAndBelow (i, (int) sliceSet.size())) return;
            set (sliceSet[(size_t) i], (float) s.getValue());
            pushToPad (i);
        };
        s.onDragEnd = [this] { auditionSlice (wave.selectedSlice); };
        addAndMakeVisible (s);
    };
    bindSlider (gainSlider,    [] (SliceSet& x, float v) { x.gainDb = v; });
    bindSlider (fadeInSlider,  [] (SliceSet& x, float v) { x.fadeInMs = v; });
    bindSlider (fadeOutSlider, [] (SliceSet& x, float v) { x.fadeOutMs = v; });

    updateChopControls();
    markersChanged();
    updateUndoButtons();
}

// ------------------------------------------------------------------ undo

SamplerPage::Snap SamplerPage::snapNow() const
{
    return { wave.getSample(), wave.markers, sliceSet, bpm, tuneSemis, wave.selectedSlice };
}

void SamplerPage::pushUndo()
{
    undoStack.push_back (snapNow());
    if (undoStack.size() > 60) undoStack.erase (undoStack.begin());
    redoStack.clear();
    updateUndoButtons();
}

void SamplerPage::applySnap (const Snap& s)
{
    const bool sameCount = s.markers.size() == wave.markers.size() && s.sample == wave.getSample();
    sliceSet = s.sets;
    tuneSemis = s.tune;
    wave.restore (s.sample, s.markers);         // calls markersChanged()
    sliceSet = s.sets;                          // restore exactly, whatever markersChanged guessed
    sliceStarts.clear();
    for (auto& sl : wave.slices()) sliceStarts.push_back (sl.first);
    if (! sameCount) assigned.clear();
    wave.selectedSlice = s.sel;
    setBpm (s.bpm);
    for (int i = 0; i < (int) assigned.size(); ++i) pushToPad (i);
    updateSliceInfo();
    updateSlicePanel();
    wave.repaint();
}

void SamplerPage::undo()
{
    if (undoStack.empty()) return;
    redoStack.push_back (snapNow());
    auto s = undoStack.back(); undoStack.pop_back();
    applySnap (s);
    updateUndoButtons();
}

void SamplerPage::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back (snapNow());
    auto s = redoStack.back(); redoStack.pop_back();
    applySnap (s);
    updateUndoButtons();
}

void SamplerPage::updateUndoButtons()
{
    undoButton.setEnabled (! undoStack.empty());
    redoButton.setEnabled (! redoStack.empty());
}

// ------------------------------------------------------------------ slices

void SamplerPage::markersChanged()
{
    auto sl = wave.getSample() != nullptr ? wave.slices() : std::vector<std::pair<int, int>>();
    std::vector<SliceSet> next (sl.size());
    if (sl.size() == sliceSet.size())
        next = sliceSet;                                     // a marker moved: settings stay with their slice
    else
        for (size_t j = 0; j < sl.size(); ++j)
            for (size_t k = 0; k < sliceStarts.size() && k < sliceSet.size(); ++k)
                if (sliceStarts[k] == sl[j].first) { next[j] = sliceSet[k]; break; }
    if (sl.size() != sliceSet.size()) assigned.clear();
    sliceSet = std::move (next);
    sliceStarts.clear();
    for (auto& s : sl) sliceStarts.push_back (s.first);
    if (wave.selectedSlice >= (int) sl.size()) wave.selectedSlice = (int) sl.size() - 1;
    for (int i = 0; i < (int) assigned.size(); ++i) pushToPad (i);   // moved markers move the pads' regions too
    updateSliceInfo();
    updateSlicePanel();
    repaint (padsArea);
}

void SamplerPage::selectSlice (int i, bool hear)
{
    wave.selectedSlice = i;
    wave.repaint();
    updateSlicePanel();
    repaint (padsArea);
    if (hear) auditionSlice (i);
}

PadState SamplerPage::sliceShape (int i) const
{
    PadState p;
    if (isPositiveAndBelow (i, (int) sliceSet.size()))
    {
        const auto& x = sliceSet[(size_t) i];
        p.gain = Decibels::decibelsToGain (x.gainDb);
        p.attackMs = x.fadeInMs;
        p.fadeOutMs = x.fadeOutMs;
        p.reverse = x.reverse;
    }
    p.tune = (float) tuneSemis;
    return p;
}

void SamplerPage::auditionSlice (int i)
{
    auto s = wave.getSample();
    auto sl = wave.slices();
    if (s == nullptr || ! isPositiveAndBelow (i, (int) sl.size())) return;
    const float len = (float) s->audio.getNumSamples();
    const auto shape = sliceShape (i);
    proc.previewRegion (s, sl[(size_t) i].first / len, sl[(size_t) i].second / len, &shape);
    const double ms = (sl[(size_t) i].second - sl[(size_t) i].first) / s->sampleRate * 1000.0 / std::pow (2.0, tuneSemis / 12.0);
    loopNextMs = Time::getMillisecondCounterHiRes() + jmax (80.0, ms);
}

void SamplerPage::pushToPad (int i)
{
    auto s = wave.getSample();
    auto sl = wave.slices();
    if (s == nullptr || ! isPositiveAndBelow (i, (int) assigned.size()) || ! isPositiveAndBelow (i, (int) sl.size())) return;
    const int pad = assigned[(size_t) i];
    auto ps = proc.getPad (pad);
    if (ps == nullptr || ps->layers.size() != 1 || ps->layers.front() != s) return;   // the pad has something else now
    const float len = (float) s->audio.getNumSamples();
    const auto shape = sliceShape (i);
    const float st = sl[(size_t) i].first / len, en = sl[(size_t) i].second / len;
    proc.updatePad (pad, [shape, st, en] (PadState& p)
    {
        p.gain = shape.gain; p.attackMs = shape.attackMs; p.fadeOutMs = shape.fadeOutMs;
        p.reverse = shape.reverse; p.tune = shape.tune; p.start = st; p.end = en;
    });
}

void SamplerPage::nudge (bool startEdge, int dir)
{
    auto s = wave.getSample();
    const int i = wave.selectedSlice;
    if (s == nullptr || i < 0) return;
    const int m = startEdge ? i - 1 : i;                      // marker index: slice i starts at markers[i-1], ends at markers[i]
    if (! isPositiveAndBelow (m, (int) wave.markers.size())) return;
    pushUndo();
    const int step = (int) (0.01 * s->sampleRate);
    const int lo = m > 0 ? wave.markers[(size_t) m - 1] + 64 : 64;
    const int hi = m + 1 < (int) wave.markers.size() ? wave.markers[(size_t) m + 1] - 64 : s->audio.getNumSamples() - 64;
    int pos = jlimit (lo, hi, wave.markers[(size_t) m] + dir * step);
    if (snapToggle.getToggleState()) pos = jlimit (lo, hi, nearestZeroCrossing (*s, pos));
    wave.markers[(size_t) m] = pos;
    wave.repaint();
    markersChanged();
    if (! loopToggle.getToggleState()) auditionSlice (i);
}

std::vector<int> SamplerPage::targetPads() const
{
    auto order = padsByNote();
    const int startPos = (int) (std::find (order.begin(), order.end(), ctx.getSelectedPad()) - order.begin());
    return std::vector<int> (order.begin() + jmin (startPos, (int) order.size()), order.end());
}

void SamplerPage::setBpm (double b)
{
    bpm = b > 0.0 ? jlimit (40.0, 250.0, b) : 0.0;
    wave.bpm = bpm;
    bpmEdit.setText (bpm > 0.0 ? String (bpm, 1) : String ("?"), dontSendNotification);
    wave.repaint();
    updateSliceInfo();
}

void SamplerPage::updateChopControls()
{
    const int mode = chopModeId;
    for (int i = 0; i < 4; ++i) modeButtons[i].setToggleState (i + 1 == mode, dontSendNotification);
    chopAmount.setVisible (mode == 1 || mode == 2);
    chopAmountLabel.setVisible (mode == 1 || mode == 2);
    for (auto* c : std::initializer_list<Component*> { &bpmLabel, &bpmEdit }) c->setVisible (mode == 3);
    for (auto& b : gridButtons) b.setVisible (mode == 3);
    chopButton.setVisible (mode != 4);
    if (mode == 1)
    {
        chopAmountLabel.setText ("Sensitivity", dontSendNotification);
        chopAmount.setRange (0.0, 100.0, 1.0);
        chopAmount.setValue (60.0, dontSendNotification);
        chopAmount.setTextValueSuffix ("%");
    }
    else if (mode == 2)
    {
        chopAmountLabel.setText ("Slices", dontSendNotification);
        chopAmount.setRange (2.0, 64.0, 1.0);
        chopAmount.setValue (16.0, dontSendNotification);
        chopAmount.setTextValueSuffix ("");
    }
    resized();
}

void SamplerPage::loadFile (const File& f)
{
    if (proc.tapPlaying.load()) { proc.stopTap(); finishTap(); wasTapping = false; }
    auto s = proc.loadSample (f);
    if (s == nullptr) { fileLabel.setText ("Couldn't load " + f.getFileName(), dontSendNotification); return; }
    if (wave.getSample() != nullptr) pushUndo();
    sliceSet.clear(); sliceStarts.clear(); assigned.clear();
    tuneSemis = 0;
    wave.setSample (s);
    // where the first sound starts (the beat grid lines up there)
    {
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c) peak = jmax (peak, s->audio.getMagnitude (c, 0, s->audio.getNumSamples()));
        int first = 0;
        while (first < s->audio.getNumSamples() && std::abs (s->audio.getSample (0, first)) < peak * 0.05f
               && std::abs (s->audio.getSample (1, first)) < peak * 0.05f) ++first;
        wave.gridOffset = first > 0.005 * s->sampleRate ? first : 0.0;
    }
    double b = bpmFromName (f.getFileNameWithoutExtension());
    bpmFromFile = b > 0.0;
    if (b <= 0.0) b = detectBpm (*s);
    setBpm (b);
    updateSliceInfo();
    updateSlicePanel();
}

void SamplerPage::replaceSample (SamplePtr s, int shift)
{
    if (s == nullptr) return;
    std::vector<int> m;
    for (int x : wave.markers) { const int p = x - shift; if (p > 64 && p < s->audio.getNumSamples() - 64) m.push_back (p); }
    // pads given slices of the old sample keep it; reassign to move them over
    assigned.clear();
    wave.restore (s, m);
    wave.gridOffset = jmax (0.0, wave.gridOffset - shift);
    updateSliceInfo();
}

void SamplerPage::normalizeSample()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    pushUndo();
    auto n = normalized (*s);
    if (auto saved = proc.saveEdited (*n, s->file.getFileNameWithoutExtension() + " normalized"))
    {
        replaceSample (saved, 0);
        sliceInfo.setText ("Normalized - saved as " + saved->file.getFileName(), dontSendNotification);
    }
    else sliceInfo.setText ("Couldn't save the normalized copy", dontSendNotification);
}

void SamplerPage::trimSample()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    int lead = 0;
    auto t = trimmed (*s, lead);
    if (t == nullptr) { sliceInfo.setText ("No silence to trim", dontSendNotification); return; }
    pushUndo();
    if (auto saved = proc.saveEdited (*t, s->file.getFileNameWithoutExtension() + " trimmed"))
    {
        replaceSample (saved, lead);
        sliceInfo.setText ("Trimmed - saved as " + saved->file.getFileName(), dontSendNotification);
    }
    else sliceInfo.setText ("Couldn't save the trimmed copy", dontSendNotification);
}

void SamplerPage::doChop()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    if (chopModeId == 3 && bpm <= 0.0) { sliceInfo.setText ("Type the loop's BPM first", dontSendNotification); return; }
    pushUndo();
    std::vector<int> m;
    if (chopModeId == 1) m = detectTransients (*s, (float) chopAmount.getValue() / 100.0f);
    else if (chopModeId == 2) m = equalSlices (s->audio.getNumSamples(), (int) chopAmount.getValue());
    else if (chopModeId == 3) m = gridSlices (s->audio.getNumSamples(), s->sampleRate, bpm, gridBeats, wave.gridOffset);
    if (snapToggle.getToggleState() && chopModeId != 3)       // beat-grid cuts stay exactly on the grid
        for (auto& x : m) x = nearestZeroCrossing (*s, x);
    sliceSet.clear(); sliceStarts.clear(); assigned.clear();
    wave.markers = m;
    wave.sortMarkers();
    wave.selectedSlice = -1;
    wave.repaint();
    markersChanged();
}

int SamplerPage::assignToPads()
{
    auto s = wave.getSample();
    if (s == nullptr) return 0;
    auto sl = wave.slices();
    auto pads = targetPads();
    const float len = (float) s->audio.getNumSamples();
    const String base = s->file.getFileNameWithoutExtension();
    assigned.clear();
    for (int i = 0; i < (int) sl.size() && i < (int) pads.size(); ++i)
    {
        proc.assignRegion (pads[(size_t) i], s, sl[(size_t) i].first / len, sl[(size_t) i].second / len, base + " #" + String (i + 1));
        assigned.push_back (pads[(size_t) i]);
        pushToPad (i);
    }
    const int n = (int) assigned.size();
    sliceInfo.setText (String (n) + " slices on pads " + String (pads.empty() ? 0 : PadLayout::noteForPad (pads.front())) + "-"
                       + String (n > 0 ? PadLayout::noteForPad (pads[(size_t) n - 1]) : 0)
                       + (n < (int) sl.size() ? " (ran out of pads)" : ""), dontSendNotification);
    repaint (padsArea);
    return n;
}

void SamplerPage::chopToPattern()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    const int n = assignToPads();
    if (n == 0) return;
    auto sl = wave.slices();
    auto& pat = proc.patterns[jlimit (0, Seq::numPatterns - 1, proc.currentPattern.load())];
    const double spb = pat.stepsPer2Beats.load() / 2.0;
    std::vector<int> starts;
    for (int i = 0; i < n; ++i) starts.push_back (sl[(size_t) i].first);
    int needed = n;
    if (bpm > 0.0) needed = (int) std::ceil ((s->audio.getNumSamples() - starts.front()) / s->sampleRate * bpm / 60.0 * spb - 0.01);
    int length = 64;
    for (int l : { 16, 32, 48, 64 }) if (l >= needed) { length = l; break; }
    auto steps = patternSteps (starts, s->sampleRate, bpm, spb, length);
    for (int i = 0; i < n; ++i)
        for (int st = 0; st < Seq::maxSteps; ++st) pat.setStep (assigned[(size_t) i], st, 0);
    for (auto& [slice, step] : steps) pat.setStep (assigned[(size_t) slice], step, 100);
    pat.length = length;
    const double tempo = proc.hostPlaying.load() ? proc.hostBpm.load() : proc.internalBpm.load();
    String msg = "Pattern " + String (proc.currentPattern.load() + 1) + ": " + String ((int) steps.size()) + " slices over "
               + String (length) + " steps";
    if (bpm > 0.0 && std::abs (tempo - bpm) > 0.5) msg << " - set the tempo to " << String (bpm, 1) << " to hear it as recorded";
    if ((int) steps.size() < n) msg << " (" << String (n - (int) steps.size()) << " didn't fit)";
    sliceInfo.setText (msg, dontSendNotification);
}

void SamplerPage::sendToKeys (int track)
{
    if (! isPositiveAndBelow (track, Keys::numTracks)) return;
    const int n = assignToPads();
    if (n == 0) return;
    auto& kt = proc.keysTracks[track];
    kt.sourcePad = assigned.front();
    kt.root = 60;
    kt.mono = false;
    kt.glideMs = 0.0f;
    kt.sliceCount = n;
    kt.name = "Slices";
    if (proc.onPadsChanged) proc.onPadsChanged();
    sliceInfo.setText ("Keys track " + String (track + 1) + " plays the slices: C3 = slice 1, C#3 = slice 2 ... (" + String (n) + " keys)",
                       dontSendNotification);
}

void SamplerPage::updateSliceInfo()
{
    auto s = wave.getSample();
    if (s != nullptr)
    {
        const double secs = s->audio.getNumSamples() / s->sampleRate;
        String info = s->file.getFileName() + "     " + String (secs, 1) + " s";
        if (bpm > 0.0) info << "  ·  " << String (bpm, 1) << " BPM" << (bpmFromFile ? " (from the name)" : " (detected)")
                            << "  ·  " << String (secs * bpm / 60.0, 1) << " beats";
        fileLabel.setText (info, dontSendNotification);
    }
    const int n = s != nullptr ? (int) wave.slices().size() : 0;
    const auto pads = targetPads();
    sliceInfo.setText (n == 0 ? String ("No sample") :
                       String (n) + (n == 1 ? " slice" : " slices") + " - they go to pads from note "
                       + String (pads.empty() ? 0 : PadLayout::noteForPad (pads.front())), dontSendNotification);
}

void SamplerPage::updateSlicePanel()
{
    const ScopedValueSetter<bool> svs (refreshing, true);
    auto s = wave.getSample();
    auto sl = wave.slices();
    const int i = wave.selectedSlice;
    const bool on = s != nullptr && isPositiveAndBelow (i, (int) sl.size()) && isPositiveAndBelow (i, (int) sliceSet.size());
    for (Component* c : std::initializer_list<Component*> { &hearButton, &startLeft, &startRight, &endLeft, &endRight, &reverseToggle,
                                                            &gainSlider, &fadeInSlider, &fadeOutSlider })
        c->setEnabled (on);
    tuneText.setText ((tuneSemis > 0 ? "+" : "") + String (tuneSemis) + " st", dontSendNotification);
    if (! on)
    {
        selTitle.setText (s == nullptr ? String ("No sample") : String ("Click a slice"), dontSendNotification);
        startText.setText ("-", dontSendNotification); endText.setText ("-", dontSendNotification);
        return;
    }
    auto secs = [&] (int p) { return String (p / s->sampleRate, 3) + " s"; };
    const auto pads = targetPads();
    String title = "Slice " + String (i + 1);
    if (i < (int) pads.size()) title << "  ->  pad " << String (PadLayout::noteForPad (pads[(size_t) i]));
    selTitle.setText (title, dontSendNotification);
    startText.setText (secs (sl[(size_t) i].first), dontSendNotification);
    endText.setText (secs (sl[(size_t) i].second), dontSendNotification);
    startLeft.setEnabled (i > 0); startRight.setEnabled (i > 0);
    endLeft.setEnabled (i < (int) sl.size() - 1); endRight.setEnabled (i < (int) sl.size() - 1);
    const auto& x = sliceSet[(size_t) i];
    gainSlider.setValue (x.gainDb, dontSendNotification);
    fadeInSlider.setValue (x.fadeInMs, dontSendNotification);
    fadeOutSlider.setValue (x.fadeOutMs, dontSendNotification);
    reverseToggle.setToggleState (x.reverse, dontSendNotification);
}

void SamplerPage::selectedPadChanged() { updateSliceInfo(); updateSlicePanel(); repaint (padsArea); }

void SamplerPage::mouseDown (const MouseEvent& e)
{
    if (! padCellsArea.contains (e.getPosition())) return;
    const auto pads = targetPads();
    const int cw = padCellsArea.getWidth() / 8, ch = padCellsArea.getHeight() / 2;
    const int idx = (e.y - padCellsArea.getY()) / jmax (1, ch) * 8 + (e.x - padCellsArea.getX()) / jmax (1, cw);
    if (! isPositiveAndBelow (idx, (int) pads.size())) return;
    if (isPositiveAndBelow (idx, (int) wave.slices().size()) && wave.getSample() != nullptr && ! e.mods.isShiftDown())
        selectSlice (idx, true);                             // a slice's pad: select that slice
    else
        ctx.selectPad (pads[(size_t) idx]);                  // an empty cell: start the slices there
}

// ------------------------------------------------------------------ zoom, tap chop, tick

void SamplerPage::scrollBarMoved (ScrollBar*, double newRangeStart)
{
    if (std::abs (newRangeStart - wave.viewStart) > 0.5) wave.setView (newRangeStart, wave.viewLen);
}

void SamplerPage::syncScrollBar()
{
    waveScroll.setRangeLimits (0.0, jmax (1.0, (double) wave.length()), dontSendNotification);
    waveScroll.setCurrentRange (wave.viewStart, wave.viewLen, dontSendNotification);
}

void SamplerPage::startStopTap()
{
    if (proc.tapPlaying.load()) { proc.stopTap(); return; }      // tick() finishes up
    auto s = wave.getSample();
    if (s == nullptr) { tapInfo.setText ("Load a sample first", dontSendNotification); return; }
    double start = wave.viewStart;
    auto sl = wave.slices();
    if (isPositiveAndBelow (wave.selectedSlice, (int) sl.size())) start = sl[(size_t) wave.selectedSlice].first;
    pendingPad = -1;
    tapCount = 0;
    tappedPads.clear();
    proc.tapMode = tapModeBox.getSelectedId() - 1;
    if (proc.tapMode.load() == PadRomplerProcessor::tapMarkers) pushUndo();
    proc.startTap (s, start);
    wasTapping = true;
    const int mode = proc.tapMode.load();
    tapInfo.setText (mode == PadRomplerProcessor::tapOff ? "Playing..." :
                     mode == PadRomplerProcessor::tapMarkers ? "Hit any pad to drop a marker" :
                                                                "Hit a pad to give it the sound from here", dontSendNotification);
}

void SamplerPage::handleTap (int pad, double pos)
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    const int mode = proc.tapMode.load();
    const float len = (float) wave.length();
    wave.addMarker ((int) pos);
    if (mode == PadRomplerProcessor::tapPads)
    {
        const String base = s->file.getFileNameWithoutExtension();
        if (pendingPad >= 0)
            proc.assignRegion (pendingPad, s, (float) (pendingStart / len), (float) (pos / len), proc.getPad (pendingPad)->name);
        pendingPad = pad;
        pendingStart = pos;
        tappedPads.push_back (pad);
        proc.assignRegion (pad, s, (float) (pos / len), 1.0f, base + " tap " + String (++tapCount));
        ctx.selectPad (pad);
        tapInfo.setText (String (tapCount) + " pads chopped - keep going or press Stop", dontSendNotification);
    }
    else
        tapInfo.setText (String ((int) wave.markers.size()) + " markers", dontSendNotification);
}

void SamplerPage::finishTap()
{
    wave.setPlayhead (-1.0);
    auto s = wave.getSample();
    if (s != nullptr && pendingPad >= 0)
    {
        const float len = (float) wave.length();
        const double end = proc.tapReachedEnd.load() ? (double) len : proc.tapPos.load();
        if (end > pendingStart + 16)
            proc.assignRegion (pendingPad, s, (float) (pendingStart / len), (float) (end / len), proc.getPad (pendingPad)->name);
    }
    if (proc.tapMode.load() == PadRomplerProcessor::tapPads && tapCount > 0)
        tapInfo.setText (String (tapCount) + " pads chopped - fine-tune them in PADS", dontSendNotification);
    else if (proc.tapMode.load() == PadRomplerProcessor::tapMarkers)
        tapInfo.setText (String ((int) wave.markers.size()) + " markers", dontSendNotification);
    else tapInfo.setText ("Stopped", dontSendNotification);
    pendingPad = -1;
}

Lights::SamplerInfo SamplerPage::lightInfo() const
{
    Lights::SamplerInfo si;
    si.active = isVisible();
    if (! si.active) return si;
    si.tapping = proc.tapPlaying.load();
    if (si.tapping)
    {
        si.tapProgress = wave.length() > 0 ? (float) jlimit (0.0, 1.0, proc.tapPos.load() / wave.length()) : 0.0f;
        for (int p : tappedPads) if (isPositiveAndBelow (p, PadLayout::numPads)) si.tapped[p] = true;
        return si;
    }
    if (wave.getSample() == nullptr) return si;
    const auto pads = targetPads();
    const int n = (int) wave.slices().size();
    for (int i = 0; i < n && i < (int) pads.size(); ++i)
    {
        si.slice[pads[(size_t) i]] = i;
        si.assigned[pads[(size_t) i]] = i < (int) assigned.size() && assigned[(size_t) i] == pads[(size_t) i];
    }
    si.selectedSlice = wave.selectedSlice;
    return si;
}

void SamplerPage::stopAuditions()
{
    loopToggle.setToggleState (false, dontSendNotification);
    if (proc.tapPlaying.load()) proc.stopTap();
}

void SamplerPage::padPressed (int pad)
{
    const auto pads = targetPads();
    const int n = wave.getSample() != nullptr ? (int) wave.slices().size() : 0;
    for (int i = 0; i < n && i < (int) pads.size(); ++i)
        if (pads[(size_t) i] == pad) { selectSlice (i, true); return; }
    proc.triggerPad (pad, 0.8f);                          // not a slice: just play the pad
}

void SamplerPage::tick()
{
    {   // TempoPAD pads pressed in Easy mode while this tab is open
        const auto presses = proc.samplerPresses.load();
        if (presses != lastPresses) { lastPresses = presses; if (isVisible()) padPressed (proc.samplerPressPad.load()); }
    }
    const int st = proc.recState.load();
    const double rate = proc.getHostRate();
    switch (st)
    {
        case PadRomplerProcessor::recArmed:     recStatus.setText ("Waiting for sound...", dontSendNotification); break;
        case PadRomplerProcessor::recRecording: recStatus.setText ("Recording  " + String (proc.recPos.load() / rate, 1) + " s", dontSendNotification); break;
        case PadRomplerProcessor::recFinished:
        {
            auto f = proc.saveRecording();
            if (f.existsAsFile()) { recStatus.setText ("Saved " + f.getFileName(), dontSendNotification); loadFile (f); }
            else recStatus.setText ("Nothing recorded", dontSendNotification);
            break;
        }
        default: if (recStatus.getText().startsWith ("Recording") || recStatus.getText().startsWith ("Waiting")) recStatus.setText ("Stopped", dontSendNotification); break;
    }
    recButton.setColour (TextButton::buttonColourId, st == PadRomplerProcessor::recRecording ? rec : rec.darker (0.3f));

    {   // tap chop
        PadRomplerProcessor::TapEvent ev[64];
        const int n = proc.popTapEvents (ev, 64);
        for (int i = 0; i < n; ++i) handleTap (ev[i].pad, ev[i].pos);
        const bool playing = proc.tapPlaying.load();
        if (playing) wave.setPlayhead (proc.tapPos.load());
        else if (wasTapping) { finishTap(); wasTapping = false; }
        tapButton.setButtonText (playing ? "Stop" : "Play");
    }

    // loop the selected slice
    if (loopToggle.getToggleState() && isShowing() && wave.selectedSlice >= 0 && wave.getSample() != nullptr
        && Time::getMillisecondCounterHiRes() >= loopNextMs)
        auditionSlice (wave.selectedSlice);

    const float lvl = proc.recInputLevel.load();
    if (std::abs (lvl - meter) > 0.001f) { meter = lvl; if (isVisible()) repaint (meterArea); }
}

// ------------------------------------------------------------------ paint / layout

void SamplerPage::paint (Graphics& g)
{
    for (auto r : { topArea, waveArea, padsArea, sliceArea })
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
    }
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText (CharPointer_UTF8 ("SLICES \xe2\x86\x92 PADS"), padsArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);

    // level meter
    g.setColour (panel.brighter (0.08f));
    g.fillRoundedRectangle (meterArea.toFloat(), 3.0f);
    const float db = Decibels::gainToDecibels (meter, -60.0f);
    g.setColour (db > -3.0f ? rec : accent);
    g.fillRoundedRectangle (meterArea.toFloat().withWidth (jmap (db, -60.0f, 0.0f, 0.0f, (float) meterArea.getWidth())), 3.0f);

    // pads the slices go to
    const auto pads = targetPads();
    const int n = wave.getSample() != nullptr ? (int) wave.slices().size() : 0;
    const float cw = padCellsArea.getWidth() / 8.0f, ch = padCellsArea.getHeight() / 2.0f;
    for (int k = 0; k < 16; ++k)
    {
        auto cell = Rectangle<float> (padCellsArea.getX() + (k % 8) * cw, padCellsArea.getY() + (k / 8) * ch, cw, ch).reduced (2.5f);
        if (k >= (int) pads.size()) { g.setColour (bg.withAlpha (0.4f)); g.fillRoundedRectangle (cell, 5.0f); continue; }
        const bool used = k < n;
        const auto hue = padHue (pads[(size_t) k]);
        const bool live = k < (int) assigned.size();
        g.setColour (used ? hue.withAlpha (live ? 0.9f : 0.45f) : panel.brighter (0.06f));
        g.fillRoundedRectangle (cell, 5.0f);
        if (k == wave.selectedSlice) { g.setColour (text); g.drawRoundedRectangle (cell, 5.0f, 2.0f); }
        g.setColour (used ? Colours::black.withAlpha (0.65f) : muted);
        g.setFont (Font (10.0f));
        g.drawText (String (PadLayout::noteForPad (pads[(size_t) k])), cell.reduced (4.0f, 2.0f), Justification::topLeft);
        if (used)
        {
            g.setColour (Colours::black);
            g.setFont (Font (13.0f, Font::bold));
            g.drawText (String (k + 1), cell, Justification::centred);
        }
    }
}

void SamplerPage::resized()
{
    auto area = getLocalBounds();
    topArea = area.removeFromTop (128);
    area.removeFromTop (10);
    auto bottom = area.removeFromBottom (276);
    area.removeFromBottom (10);
    waveArea = area;
    sliceArea = bottom.removeFromRight (330);
    bottom.removeFromRight (12);
    padsArea = bottom;

    // ---- top: file, record, chop
    auto t = topArea.reduced (12, 10);
    {
        auto r = t.removeFromTop (28);
        loadButton.setBounds (r.removeFromLeft (120)); r.removeFromLeft (10);
        redoButton.setBounds (r.removeFromRight (56)); r.removeFromRight (4);
        undoButton.setBounds (r.removeFromRight (56)); r.removeFromRight (10);
        fileLabel.setBounds (r);
    }
    t.removeFromTop (8);
    {
        auto r = t.removeFromTop (28);
        sourceLabel.setBounds (r.removeFromLeft (72));
        sourceBox.setBounds (r.removeFromLeft (190).reduced (0, 1)); r.removeFromLeft (12);
        thresholdLabel.setBounds (r.removeFromLeft (128));
        thresholdSlider.setBounds (r.removeFromLeft (150)); r.removeFromLeft (8);
        maxLenLabel.setBounds (r.removeFromLeft (28));
        maxLenSlider.setBounds (r.removeFromLeft (120)); r.removeFromLeft (8);
        recButton.setBounds (r.removeFromLeft (70)); r.removeFromLeft (4);
        stopButton.setBounds (r.removeFromLeft (56)); r.removeFromLeft (10);
        meterArea = r.removeFromRight (80).reduced (0, 10);
        r.removeFromRight (6);
        recStatus.setBounds (r);
    }
    t.removeFromTop (8);
    {
        auto r = t.removeFromTop (28);
        chopLabel.setBounds (r.removeFromLeft (36));
        for (auto& b : modeButtons) { b.setBounds (r.removeFromLeft (b.getButtonText() == "Equal" ? 56 : 80).reduced (0, 1)); r.removeFromLeft (3); }
        r.removeFromLeft (10);
        trimButton.setBounds (r.removeFromRight (90).reduced (0, 1)); r.removeFromRight (4);
        normalizeButton.setBounds (r.removeFromRight (80).reduced (0, 1)); r.removeFromRight (8);
        tuneUp.setBounds (r.removeFromRight (24).reduced (0, 1));
        tuneText.setBounds (r.removeFromRight (46));
        tuneDown.setBounds (r.removeFromRight (24).reduced (0, 1)); r.removeFromRight (10);
        if (chopModeId == 1 || chopModeId == 2)
        {
            chopAmountLabel.setBounds (r.removeFromLeft (chopModeId == 1 ? 68 : 40));
            chopAmount.setBounds (r.removeFromLeft (150)); r.removeFromLeft (6);
        }
        else if (chopModeId == 3)
        {
            bpmLabel.setBounds (r.removeFromLeft (30));
            bpmEdit.setBounds (r.removeFromLeft (50).reduced (0, 2)); r.removeFromLeft (6);
            for (auto& b : gridButtons) { b.setBounds (r.removeFromLeft (b.getButtonText().length() > 3 ? 46 : 38).reduced (0, 1)); r.removeFromLeft (2); }
            r.removeFromLeft (6);
        }
        chopButton.setBounds (r.removeFromLeft (56).reduced (0, 1)); r.removeFromLeft (4);
        clearMarkersButton.setBounds (r.removeFromLeft (52).reduced (0, 1));
    }

    // ---- waveform
    auto w = waveArea.reduced (12, 8);
    {
        auto r = w.removeFromTop (22);
        loopToggle.setBounds (r.removeFromRight (190));
        sliceInfo.setBounds (r);
    }
    w.removeFromTop (4);
    auto wb = w.removeFromBottom (22);
    zoomFitButton.setBounds (wb.removeFromRight (40)); wb.removeFromRight (4);
    zoomInButton.setBounds (wb.removeFromRight (26));  wb.removeFromRight (4);
    zoomOutButton.setBounds (wb.removeFromRight (26)); wb.removeFromRight (8);
    waveScroll.setBounds (wb.reduced (0, 5));
    w.removeFromBottom (4);
    wave.setBounds (w);

    // ---- slices -> pads
    auto p = padsArea.reduced (12, 8);
    p.removeFromTop (22);
    padCellsArea = p.removeFromTop (96);
    p.removeFromTop (10);
    {
        auto r = p.removeFromTop (30);
        assignButton.setBounds (r.removeFromLeft (150)); r.removeFromLeft (8);
        patternButton.setBounds (r.removeFromLeft (130)); r.removeFromLeft (8);
        keysButton.setBounds (r.removeFromLeft (140));
    }
    p.removeFromTop (8);
    {
        auto r = p.removeFromTop (26);
        tapLabel.setBounds (r.removeFromLeft (58));
        tapModeBox.setBounds (r.removeFromLeft (200)); r.removeFromLeft (6);
        tapButton.setBounds (r.removeFromLeft (60)); r.removeFromLeft (8);
        tapInfo.setBounds (r);
    }
    p.removeFromTop (6);
    hint.setBounds (p);

    // ---- selected slice
    auto s = sliceArea.reduced (12, 8);
    {
        auto r = s.removeFromTop (24);
        hearButton.setBounds (r.removeFromRight (70));
        selTitle.setBounds (r);
    }
    s.removeFromTop (8);
    auto edgeRow = [&] (Label& l, TextButton& a, Label& txt, TextButton& b)
    {
        auto r = s.removeFromTop (26);
        l.setBounds (r.removeFromLeft (44));
        a.setBounds (r.removeFromLeft (30)); r.removeFromLeft (4);
        txt.setBounds (r.removeFromLeft (80)); r.removeFromLeft (4);
        b.setBounds (r.removeFromLeft (30));
        s.removeFromTop (5);
    };
    edgeRow (startLabel, startLeft, startText, startRight);
    edgeRow (endLabel, endLeft, endText, endRight);
    snapToggle.setBounds (s.removeFromTop (22));
    s.removeFromTop (6);
    auto sliderRow = [&] (Label& l, Slider& sl)
    {
        auto r = s.removeFromTop (26);
        l.setBounds (r.removeFromLeft (60));
        sl.setBounds (r);
        s.removeFromTop (4);
    };
    sliderRow (gainLabel, gainSlider);
    sliderRow (fadeInLabel, fadeInSlider);
    sliderRow (fadeOutLabel, fadeOutSlider);
    s.removeFromTop (2);
    reverseToggle.setBounds (s.removeFromTop (22));
}
