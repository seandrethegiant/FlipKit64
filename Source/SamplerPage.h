#pragma once
#include "Common.h"

// Waveform with slice markers. Click = audition the slice under the mouse,
// double-click = add a marker, drag a marker = move it, right-click a marker = delete it,
// mouse wheel = zoom (Shift + wheel = scroll).
class WaveformView : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     public juce::DragAndDropTarget
{
public:
    explicit WaveformView (EditorContext& c) : ctx (c) {}
    void setSample (SamplePtr s);
    SamplePtr getSample() const { return sample; }
    std::vector<int> markers;               // slice starts in samples (0 is implicit)
    int selectedSlice = -1;
    std::function<void()> onMarkersChanged;
    std::function<void (const juce::File&)> onFileDropped;

    std::vector<std::pair<int, int>> slices() const;   // [start, end) in samples

    // zoom / scroll (in samples)
    double viewStart = 0.0, viewLen = 1.0;
    void setView (double start, double len);
    void zoomAround (double centreSample, double factor);
    int length() const { return sample != nullptr ? sample->audio.getNumSamples() : 0; }
    std::function<void()> onViewChanged;
    double playhead = -1.0;                  // tap-chop playhead in samples (-1 = hidden)
    void setPlayhead (double s);
    bool addMarker (int samplePos);          // returns false if too close to an existing one

    void paint (juce::Graphics&) override;
    void resized() override { peaksDirty = true; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragMarker = -1; }
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString() == EditorContext::dragId; }
    void itemDropped (const SourceDetails&) override;

private:
    EditorContext& ctx;
    SamplePtr sample;
    std::vector<float> peakMin, peakMax;
    bool peaksDirty = true;
    int dragMarker = -1;
    void buildPeaks();
    float xForSample (int s) const;
    int sampleForX (float x) const;
    int markerNear (float x) const;
    void sortMarkers();
};

class SamplerPage : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit SamplerPage (EditorContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    void loadFile (const juce::File& f);
    void selectedPadChanged();

    // chop helpers (public for tests)
    static std::vector<int> detectTransients (const SampleData& s, float sensitivity /*0..1*/);
    static std::vector<int> equalSlices (int length, int count);

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;

    // record section
    juce::ComboBox sourceBox;
    juce::Slider thresholdSlider, maxLenSlider;
    juce::Label thresholdLabel, sourceLabel, recStatus, inputNote, maxLenLabel;
    juce::TextButton recButton { "Record" }, stopButton { "Stop" };
    float meter = 0.0f;

    // waveform + chop
    WaveformView wave { ctx };
    juce::Label fileLabel, sliceInfo, chopHint;
    juce::TextButton loadButton { "Load sample..." };
    juce::ComboBox chopMode;
    juce::Slider chopAmount;
    juce::Label chopAmountLabel;
    juce::TextButton chopButton { "Chop" }, clearMarkersButton { "Clear markers" }, assignButton { "Assign slices to pads" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> recArea, waveArea, chopArea, meterArea;

    // zoom / scroll
    juce::ScrollBar waveScroll { false };
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" }, zoomFitButton { "Fit" };
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void syncScrollBar();

    // tap chop
    juce::Label tapLabel, tapInfo;
    juce::ComboBox tapModeBox;
    juce::TextButton tapButton { "Play" };
    int pendingPad = -1, tapCount = 0;
    double pendingStart = 0.0;
    bool wasTapping = false;
    void startStopTap();
    void handleTap (int pad, double pos);
    void finishTap();

    void doChop();
    void assignToPads();
    void updateChopControls();
    void updateSliceInfo();
};
