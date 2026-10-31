#pragma once
#include "Common.h"
#include "Lights.h"

// Waveform with slice markers. Click = select and hear the slice under the mouse,
// double-click = add a marker, drag a marker = move it, right-click a marker = delete it,
// mouse wheel = zoom (Shift + wheel = scroll).
class WaveformView : public juce::Component,
                     public juce::FileDragAndDropTarget,
                     public juce::DragAndDropTarget
{
public:
    explicit WaveformView (EditorContext& c) : ctx (c) {}
    void setSample (SamplePtr s);
    void restore (SamplePtr s, std::vector<int> m);    // undo: keeps the view if the sample is the same
    SamplePtr getSample() const { return sample; }
    std::vector<int> markers;               // slice starts in samples (0 is implicit)
    int selectedSlice = -1;
    std::function<void()> onMarkersChanged;
    std::function<void()> onEditStart;      // just before the mouse changes the markers (for undo)
    std::function<void (int)> onSliceClicked;
    std::function<int (int)> snap;          // optional: move a marker position (zero crossings)
    std::function<void (const juce::File&)> onFileDropped;

    std::vector<std::pair<int, int>> slices() const;   // [start, end) in samples

    // beat grid overlay (bpm <= 0 = none)
    double bpm = 0.0, gridOffset = 0.0;

    // zoom / scroll (in samples)
    double viewStart = 0.0, viewLen = 1.0;
    void setView (double start, double len);
    void zoomAround (double centreSample, double factor);
    int length() const { return sample != nullptr ? sample->audio.getNumSamples() : 0; }
    std::function<void()> onViewChanged;
    double playhead = -1.0;                  // tap-chop playhead in samples (-1 = hidden)
    void setPlayhead (double s);
    bool addMarker (int samplePos);          // returns false if too close to an existing one
    void sortMarkers();

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
};

class SamplerPage : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit SamplerPage (EditorContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void tick();
    void loadFile (const juce::File& f);
    void selectedPadChanged();

    // per-slice settings, applied to the pads they're assigned to
    struct SliceSet { float gainDb = 0.0f, fadeInMs = 0.0f, fadeOutMs = 0.0f; bool reverse = false; };

    // chop helpers (public for tests)
    static std::vector<int> detectTransients (const SampleData& s, float sensitivity /*0..1*/);
    static std::vector<int> equalSlices (int length, int count);
    static std::vector<int> gridSlices (int length, double sampleRate, double bpm, double beatsPerSlice, double offset = 0.0);
    static double detectBpm (const SampleData& s);                 // 0 = couldn't tell
    static double bpmFromName (const juce::String& fileName);      // "Rainy Keys 88bpm.wav" -> 88, else 0
    static int nearestZeroCrossing (const SampleData& s, int pos, int range = 256);
    static std::shared_ptr<SampleData> normalized (const SampleData& s);
    static std::shared_ptr<SampleData> trimmed (const SampleData& s, int& lead);   // lead: samples cut from the front
    // slice starts -> pattern steps (pairs of slice index, step); bpm <= 0 = one slice per step
    static std::vector<std::pair<int, int>> patternSteps (const std::vector<int>& starts, double sampleRate,
                                                          double bpm, double stepsPerBeat, int maxSteps);

    // actions (public for tests)
    void doChop();
    int assignToPads();                     // returns how many slices landed on pads
    void chopToPattern();
    void sendToKeys (int track);
    WaveformView& getWave() { return wave; }
    std::vector<SliceSet> sliceSet;
    std::vector<int> sliceStarts;           // where each SliceSet's slice started (to follow marker edits)
    std::vector<int> assigned;              // slice i -> the pad it was last assigned to (edits follow live)
    double bpm = 0.0;
    bool bpmFromFile = false;
    int tuneSemis = 0;
    int chopModeId = 1;                     // 1 transients, 2 equal, 3 beat grid, 4 manual
    double gridBeats = 1.0;                 // beat grid: beats per slice
    void setChopMode (int id) { chopModeId = juce::jlimit (1, 4, id); updateChopControls(); }
    void undo();
    void redo();
    juce::String status() const { return sliceInfo.getText(); }
    Lights::SamplerInfo lightInfo() const;   // what the TempoPAD shows while this tab is open
    void padPressed (int pad);
    void stopAuditions();                    // Esc / Stop sounds: also stops "Loop the selected slice"               // a TempoPAD pad in Easy mode: hear and select its slice

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;

    // file + undo
    juce::Label fileLabel;
    juce::TextButton loadButton { "Load sample..." }, undoButton { "Undo" }, redoButton { "Redo" };
    std::unique_ptr<juce::FileChooser> chooser;
    struct Snap { SamplePtr sample; std::vector<int> markers; std::vector<SliceSet> sets; double bpm; int tune; int sel; };
    std::vector<Snap> undoStack, redoStack;
    Snap snapNow() const;
    void pushUndo();
    void applySnap (const Snap&);

    // record
    juce::ComboBox sourceBox;
    juce::Slider thresholdSlider, maxLenSlider;
    juce::Label thresholdLabel, sourceLabel, recStatus, maxLenLabel;
    juce::TextButton recButton { "Record" }, stopButton { "Stop" };
    float meter = 0.0f;

    // chop row
    juce::Label chopLabel, chopAmountLabel, bpmLabel, bpmEdit, tuneText;
    juce::TextButton modeButtons[4] { juce::TextButton ("Transients"), juce::TextButton ("Equal"), juce::TextButton ("Beat grid"), juce::TextButton ("Manual") };
    juce::TextButton gridButtons[4] { juce::TextButton ("1 bar"), juce::TextButton ("1/4"), juce::TextButton ("1/8"), juce::TextButton ("1/16") };
    juce::Slider chopAmount;
    juce::TextButton chopButton { "Chop" }, clearMarkersButton { "Clear" };
    juce::TextButton tuneDown { "-" }, tuneUp { "+" }, normalizeButton { "Normalize" }, trimButton { "Trim silence" };

    // waveform
    WaveformView wave { ctx };
    juce::Label sliceInfo;
    juce::ToggleButton loopToggle { "Loop the selected slice" };
    juce::ScrollBar waveScroll { false };
    juce::TextButton zoomInButton { "+" }, zoomOutButton { "-" }, zoomFitButton { "Fit" };
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void syncScrollBar();
    double loopNextMs = 0.0;

    // slices -> pads
    juce::TextButton assignButton { "Assign slices to pads" }, patternButton { "Chop to pattern" }, keysButton { "Send to Keys track" };
    juce::Label tapLabel, tapInfo, hint;
    juce::ComboBox tapModeBox;
    juce::TextButton tapButton { "Play" };
    juce::Rectangle<int> padCellsArea;
    std::vector<int> targetPads() const;    // pads the slices go to, in note order from the selected pad

    // selected slice
    juce::Label selTitle, startLabel, endLabel, startText, endText;
    juce::TextButton hearButton { "Hear it" }, startLeft { "<" }, startRight { ">" }, endLeft { "<" }, endRight { ">" };
    juce::ToggleButton snapToggle { "Snap to zero crossings (no clicks)" }, reverseToggle { "Reverse this slice" };
    juce::Slider gainSlider, fadeInSlider, fadeOutSlider;
    juce::Label gainLabel, fadeInLabel, fadeOutLabel;
    bool refreshing = false;

    juce::Rectangle<int> topArea, waveArea, padsArea, sliceArea, meterArea;

    // tap chop
    int pendingPad = -1, tapCount = 0;
    std::vector<int> tappedPads;
    uint32_t lastPresses = 0;
    double pendingStart = 0.0;
    bool wasTapping = false;
    void startStopTap();
    void handleTap (int pad, double pos);
    void finishTap();

    void markersChanged();                  // keeps sliceSet in step with the markers
    void selectSlice (int i, bool hear);
    void auditionSlice (int i);
    PadState sliceShape (int i) const;
    void pushToPad (int i);                 // an assigned slice's pad follows its settings
    void nudge (bool startEdge, int dir);
    void updateChopControls();
    void updateSliceInfo();
    void updateSlicePanel();
    void updateUndoButtons();
    void setBpm (double b);
    void normalizeSample();
    void trimSample();
    void replaceSample (SamplePtr s, int shift);
};
