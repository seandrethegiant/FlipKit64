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
        if (m >= 0) { markers.erase (markers.begin() + m); repaint(); if (onMarkersChanged) onMarkersChanged(); }
        return;
    }
    if (m >= 0) { dragMarker = m; return; }

    // audition the slice under the mouse
    const int s = sampleForX ((float) e.x);
    auto sl = slices();
    for (int i = 0; i < (int) sl.size(); ++i)
        if (s >= sl[(size_t) i].first && s < sl[(size_t) i].second)
        {
            selectedSlice = i;
            const float len = (float) sample->audio.getNumSamples();
            ctx.getProc().previewRegion (sample, sl[(size_t) i].first / len, sl[(size_t) i].second / len);
            repaint();
            break;
        }
}

void WaveformView::mouseDrag (const MouseEvent& e)
{
    if (dragMarker < 0 || sample == nullptr) return;
    markers[(size_t) dragMarker] = jlimit (1, sample->audio.getNumSamples() - 1, sampleForX ((float) e.x));
    const int v = markers[(size_t) dragMarker];
    sortMarkers();
    dragMarker = (int) (std::find (markers.begin(), markers.end(), v) - markers.begin());
    repaint();
    if (onMarkersChanged) onMarkersChanged();
}

void WaveformView::mouseDoubleClick (const MouseEvent& e)
{
    if (sample == nullptr || markerNear ((float) e.x) >= 0) return;
    markers.push_back (jmax (1, sampleForX ((float) e.x)));
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

// ===================================================================================== SamplerPage

SamplerPage::SamplerPage (EditorContext& c) : ctx (c), proc (c.getProc())
{
    // ---- record section
    sourceLabel.setText ("Record from", dontSendNotification); styleLabel (sourceLabel, 11.0f); addAndMakeVisible (sourceLabel);
    sourceBox.addItem ("FlipKit64 output (resample)", 1);
    sourceBox.addItem ("Audio input", 2);
    sourceBox.setSelectedId (proc.recSource.load() + 1, dontSendNotification);
    sourceBox.onChange = [this] { proc.recSource = sourceBox.getSelectedId() - 1; };
    addAndMakeVisible (sourceBox);

    thresholdLabel.setText ("Start when louder than", dontSendNotification); styleLabel (thresholdLabel, 11.0f); addAndMakeVisible (thresholdLabel);
    thresholdSlider.setSliderStyle (Slider::LinearHorizontal);
    thresholdSlider.setTextBoxStyle (Slider::TextBoxRight, false, 70, 20);
    thresholdSlider.setRange (-60.0, 0.0, 1.0);
    thresholdSlider.setValue (-60.0, dontSendNotification);
    thresholdSlider.textFromValueFunction = [] (double v) { return v <= -59.5 ? String ("Off") : String ((int) v) + " dB"; };
    thresholdSlider.updateText();
    thresholdSlider.setColour (Slider::thumbColourId, accent);
    thresholdSlider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    thresholdSlider.onValueChange = [this]
    {
        const double v = thresholdSlider.getValue();
        proc.recThreshold = v <= -59.5 ? 0.0f : Decibels::decibelsToGain ((float) v);
    };
    addAndMakeVisible (thresholdSlider);

    maxLenLabel.setText ("Max length", dontSendNotification); styleLabel (maxLenLabel, 11.0f); addAndMakeVisible (maxLenLabel);
    maxLenSlider.setSliderStyle (Slider::LinearHorizontal);
    maxLenSlider.setTextBoxStyle (Slider::TextBoxRight, false, 50, 20);
    maxLenSlider.setRange (5.0, 120.0, 5.0);
    maxLenSlider.setValue (30.0, dontSendNotification);
    maxLenSlider.setTextValueSuffix (" s");
    maxLenSlider.setColour (Slider::thumbColourId, accent);
    maxLenSlider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    addAndMakeVisible (maxLenSlider);

    styleButton (recButton); styleButton (stopButton);
    recButton.setColour (TextButton::buttonColourId, rec.darker (0.3f));
    recButton.onClick = [this]
    {
        if (proc.recSource.load() == 1 && ! proc.inputBusAvailable())
        {
            recStatus.setText ("No audio input - see note below", dontSendNotification);
            return;
        }
        proc.armRecording (maxLenSlider.getValue());
    };
    stopButton.onClick = [this] { proc.stopRecording(); };
    addAndMakeVisible (recButton);
    addAndMakeVisible (stopButton);
    styleLabel (recStatus, 13.0f, text);
    addAndMakeVisible (recStatus);
    inputNote.setText ("Resample records whatever FlipKit64 plays (pads, sequencer). "
                       "To record outside audio, give FlipKit64's sampling input an audio source in your DAW, if it supports that. "
                       "Recordings are saved to Documents\\FlipKit64\\Recordings.", dontSendNotification);
    styleLabel (inputNote, 11.0f);
    inputNote.setJustificationType (Justification::topLeft);
    addAndMakeVisible (inputNote);

    // ---- waveform
    wave.onMarkersChanged = [this] { updateSliceInfo(); };
    wave.onFileDropped = [this] (const File& f) { loadFile (f); };
    addAndMakeVisible (wave);
    styleLabel (fileLabel, 13.0f, text);
    fileLabel.setText ("No sample loaded", dontSendNotification);
    addAndMakeVisible (fileLabel);
    styleButton (loadButton);
    loadButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Load a sample to chop", proc.browserRoot, "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { if (fc.getResult().existsAsFile()) loadFile (fc.getResult()); });
    };
    addAndMakeVisible (loadButton);

    // ---- chop
    chopMode.addItem ("Transients", 1);
    chopMode.addItem ("Equal slices", 2);
    chopMode.addItem ("Manual", 3);
    chopMode.setSelectedId (1, dontSendNotification);
    chopMode.onChange = [this] { updateChopControls(); };
    addAndMakeVisible (chopMode);
    styleLabel (chopAmountLabel, 11.0f);
    addAndMakeVisible (chopAmountLabel);
    chopAmount.setSliderStyle (Slider::LinearHorizontal);
    chopAmount.setTextBoxStyle (Slider::TextBoxRight, false, 50, 20);
    chopAmount.setColour (Slider::thumbColourId, accent);
    chopAmount.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    addAndMakeVisible (chopAmount);
    for (auto* b : { &chopButton, &clearMarkersButton, &assignButton }) { styleButton (*b); addAndMakeVisible (b); }
    assignButton.setColour (TextButton::buttonColourId, accent.darker (0.35f));
    chopButton.onClick = [this] { doChop(); };
    clearMarkersButton.onClick = [this] { wave.markers.clear(); wave.repaint(); updateSliceInfo(); };
    assignButton.onClick = [this] { assignToPads(); };
    styleLabel (sliceInfo, 12.0f, text);
    addAndMakeVisible (sliceInfo);
    chopHint.setText ("Click the waveform to hear a slice; double-click adds a marker, drag moves it, right-click deletes it. "
                      "Mouse wheel zooms (Shift+wheel scrolls). Tap chop: pick a mode, press Play, and hit TempoPAD pads as it plays.",
                      dontSendNotification);
    styleLabel (chopHint, 11.0f);
    chopHint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (chopHint);

    // ---- zoom / scroll
    waveScroll.addListener (this);
    waveScroll.setAutoHide (false);
    waveScroll.setColour (ScrollBar::thumbColourId, accent.withAlpha (0.6f));
    addAndMakeVisible (waveScroll);
    for (auto* b : { &zoomOutButton, &zoomInButton, &zoomFitButton }) { styleButton (*b); addAndMakeVisible (b); }
    zoomInButton.onClick  = [this] { wave.zoomAround (wave.viewStart + wave.viewLen * 0.5, 0.5); };
    zoomOutButton.onClick = [this] { wave.zoomAround (wave.viewStart + wave.viewLen * 0.5, 2.0); };
    zoomFitButton.onClick = [this] { wave.setView (0.0, wave.length()); };
    wave.onViewChanged = [this] { syncScrollBar(); };

    // ---- tap chop
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
    addAndMakeVisible (tapButton);
    styleLabel (tapInfo, 12.0f, text);
    addAndMakeVisible (tapInfo);

    updateChopControls();
    updateSliceInfo();
}

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

    // start from the selected slice, or from the left edge of the current view
    double start = wave.viewStart;
    auto sl = wave.slices();
    if (isPositiveAndBelow (wave.selectedSlice, (int) sl.size())) start = sl[(size_t) wave.selectedSlice].first;
    pendingPad = -1;
    tapCount = 0;
    proc.tapMode = tapModeBox.getSelectedId() - 1;
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
        if (pendingPad >= 0)       // the previous tapped pad ends where this one starts
            proc.assignRegion (pendingPad, s, (float) (pendingStart / len), (float) (pos / len), proc.getPad (pendingPad)->name);
        pendingPad = pad;
        pendingStart = pos;
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
        tapInfo.setText (String (tapCount) + " pads chopped - fine-tune them in the PADS tab", dontSendNotification);
    else if (proc.tapMode.load() == PadRomplerProcessor::tapMarkers)
        tapInfo.setText (String ((int) wave.markers.size()) + " markers - adjust, then Assign slices to pads", dontSendNotification);
    else tapInfo.setText ("Stopped", dontSendNotification);
    pendingPad = -1;
}

void SamplerPage::updateChopControls()
{
    const int mode = chopMode.getSelectedId();
    chopAmount.setVisible (mode != 3);
    chopAmountLabel.setVisible (mode != 3);
    chopButton.setVisible (mode != 3);
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
}

void SamplerPage::loadFile (const File& f)
{
    if (proc.tapPlaying.load()) { proc.stopTap(); finishTap(); wasTapping = false; }
    auto s = proc.loadSample (f);
    if (s == nullptr) { fileLabel.setText ("Couldn't load " + f.getFileName(), dontSendNotification); return; }
    wave.setSample (s);
    fileLabel.setText (f.getFileName() + "   (" + String (s->audio.getNumSamples() / s->sampleRate, 2) + " s)", dontSendNotification);
    updateSliceInfo();
}

void SamplerPage::doChop()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    if (chopMode.getSelectedId() == 1) wave.markers = detectTransients (*s, (float) chopAmount.getValue() / 100.0f);
    else wave.markers = equalSlices (s->audio.getNumSamples(), (int) chopAmount.getValue());
    wave.selectedSlice = -1;
    wave.repaint();
    updateSliceInfo();
}

void SamplerPage::assignToPads()
{
    auto s = wave.getSample();
    if (s == nullptr) return;
    auto sl = wave.slices();
    auto order = padsByNote();
    const int startPos = (int) (std::find (order.begin(), order.end(), ctx.getSelectedPad()) - order.begin());
    const float len = (float) s->audio.getNumSamples();
    const String base = s->file.getFileNameWithoutExtension();
    int assigned = 0;
    for (int i = 0; i < (int) sl.size() && startPos + i < (int) order.size(); ++i)
    {
        proc.assignRegion (order[(size_t) (startPos + i)], s, sl[(size_t) i].first / len, sl[(size_t) i].second / len,
                           base + " #" + String (i + 1));
        ++assigned;
    }
    sliceInfo.setText (String (assigned) + " slices assigned" + (assigned < (int) sl.size() ? " (ran out of pads)" : "")
                       + " - see the PADS tab", dontSendNotification);
}

void SamplerPage::updateSliceInfo()
{
    const int n = wave.getSample() != nullptr ? (int) wave.slices().size() : 0;
    const int pad = ctx.getSelectedPad();
    sliceInfo.setText (n == 0 ? String ("No sample") :
                       String (n) + (n == 1 ? " slice" : " slices") + " - will start at pad note " + String (PadLayout::noteForPad (pad)),
                       dontSendNotification);
}

void SamplerPage::selectedPadChanged() { updateSliceInfo(); }

void SamplerPage::tick()
{
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

    // tap chop
    {
        PadRomplerProcessor::TapEvent ev[64];
        const int n = proc.popTapEvents (ev, 64);
        for (int i = 0; i < n; ++i) handleTap (ev[i].pad, ev[i].pos);
        const bool playing = proc.tapPlaying.load();
        if (playing) wave.setPlayhead (proc.tapPos.load());
        else if (wasTapping) { finishTap(); wasTapping = false; }
        tapButton.setButtonText (playing ? "Stop" : "Play");
    }

    const float lvl = proc.recInputLevel.load();
    if (std::abs (lvl - meter) > 0.001f) { meter = lvl; if (isVisible()) repaint (meterArea); }
}

void SamplerPage::paint (Graphics& g)
{
    for (auto r : { recArea, chopArea })
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
    }
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText ("RECORD / RESAMPLE", recArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);
    g.drawText ("CHOP", chopArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);

    // level meter
    g.setColour (panel.brighter (0.08f));
    g.fillRoundedRectangle (meterArea.toFloat(), 3.0f);
    const float db = Decibels::gainToDecibels (meter, -60.0f);
    const float w = jmap (db, -60.0f, 0.0f, 0.0f, (float) meterArea.getWidth());
    g.setColour (db > -3.0f ? rec : accent);
    g.fillRoundedRectangle (meterArea.toFloat().withWidth (w), 3.0f);
}

void SamplerPage::resized()
{
    auto area = getLocalBounds();
    recArea = area.removeFromTop (150);
    area.removeFromTop (10);
    chopArea = area.removeFromBottom (150);
    area.removeFromBottom (10);
    waveArea = area;

    // record
    auto r = recArea.reduced (12);
    r.removeFromTop (22);
    auto row1 = r.removeFromTop (14);
    auto row2 = r.removeFromTop (26);
    auto colW = row2.getWidth() / 4;
    sourceLabel.setBounds (row1.removeFromLeft (colW));      sourceBox.setBounds (row2.removeFromLeft (colW - 10)); row2.removeFromLeft (10);
    thresholdLabel.setBounds (row1.removeFromLeft (colW));   thresholdSlider.setBounds (row2.removeFromLeft (colW - 10)); row2.removeFromLeft (10);
    maxLenLabel.setBounds (row1.removeFromLeft (colW));      maxLenSlider.setBounds (row2.removeFromLeft (colW - 10)); row2.removeFromLeft (10);
    recButton.setBounds (row2.removeFromLeft (row2.getWidth() / 2 - 4));
    row2.removeFromLeft (8);
    stopButton.setBounds (row2);
    r.removeFromTop (10);
    auto row3 = r.removeFromTop (22);
    recStatus.setBounds (row3.removeFromLeft (260));
    meterArea = row3.reduced (0, 6);
    r.removeFromTop (6);
    inputNote.setBounds (r);

    // waveform
    auto w = waveArea;
    auto wtop = w.removeFromTop (28);
    loadButton.setBounds (wtop.removeFromRight (130).reduced (0, 2));
    fileLabel.setBounds (wtop);
    w.removeFromTop (4);
    auto wbottom = w.removeFromBottom (24);
    zoomFitButton.setBounds (wbottom.removeFromRight (44)); wbottom.removeFromRight (4);
    zoomInButton.setBounds (wbottom.removeFromRight (28));  wbottom.removeFromRight (4);
    zoomOutButton.setBounds (wbottom.removeFromRight (28)); wbottom.removeFromRight (8);
    waveScroll.setBounds (wbottom.reduced (0, 5));
    w.removeFromBottom (4);
    wave.setBounds (w);

    // chop
    auto c = chopArea.reduced (12);
    c.removeFromTop (22);
    auto crow = c.removeFromTop (26);
    chopMode.setBounds (crow.removeFromLeft (150)); crow.removeFromLeft (12);
    chopAmountLabel.setBounds (crow.removeFromLeft (76));
    chopAmount.setBounds (crow.removeFromLeft (200)); crow.removeFromLeft (12);
    chopButton.setBounds (crow.removeFromLeft (80)); crow.removeFromLeft (8);
    clearMarkersButton.setBounds (crow.removeFromLeft (110)); crow.removeFromLeft (16);
    assignButton.setBounds (crow.removeFromRight (190));
    sliceInfo.setBounds (crow);
    c.removeFromTop (8);
    auto trow = c.removeFromTop (26);
    tapLabel.setBounds (trow.removeFromLeft (60));
    tapModeBox.setBounds (trow.removeFromLeft (220)); trow.removeFromLeft (8);
    tapButton.setBounds (trow.removeFromLeft (80)); trow.removeFromLeft (12);
    tapInfo.setBounds (trow);
    c.removeFromTop (6);
    chopHint.setBounds (c);
}
