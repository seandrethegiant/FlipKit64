#pragma once
#include "Common.h"
#include "KeysRoll.h"

class SequencerPage;

// Undo / redo for the sequencer. It watches all 8 patterns and records a step whenever one settles
// after a change, whatever made it: clicks, live record, the TempoPAD's step mode, Capture, Clear.
class PatternHistory
{
public:
    explicit PatternHistory (PadRomplerProcessor&);
    void tick();                                  // ~30x a second, from the editor's timer
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const { return ! redoStack.empty(); }

private:
    struct Entry { int pattern; std::unique_ptr<PatternData> data; };
    PadRomplerProcessor& proc;
    std::unique_ptr<PatternData> stable[Seq::numPatterns], seen[Seq::numPatterns];
    juce::uint32 changedAt[Seq::numPatterns] {};
    bool dirty[Seq::numPatterns] {};
    std::vector<Entry> undoStack, redoStack;
    juce::uint32 lastKitLoads = 0;
    int ticks = 0;
    void resetAll();
    void flush();                                 // commit changes that haven't settled yet
    void commit (int p);
    void apply (int p, const PatternData& d);
};

// The step grid: one row per pad, 16 steps per page.
// Click an empty step = add it (Shift = accent) and select it; click a lit step = select it,
// click the selected step again = remove it. Drag up/down on a step = velocity.
// Right-click = quick menu. Click a pad name to hear it.
class StepGrid : public juce::Component
{
public:
    StepGrid (EditorContext& c, SequencerPage& o) : ctx (c), owner (o) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::vector<int> rows;
    int page = 0;
    int selPad = -1, selStep = -1;               // the step the panel edits
    static constexpr int rowH = 26, nameW = 170;
    static constexpr int laneHeaderH = 24, laneRowH = 20;    // the Keys tracks strip under the drums
    int laneTop() const { return (int) rows.size() * rowH; }

private:
    EditorContext& ctx;
    SequencerPage& owner;
    int dragPad = -1, dragStep = -1, dragStartY = 0, dragStartVel = 0;
    bool turnedOn = false, moved = false, wasSelected = false;
    bool cellAt (juce::Point<int> p, int& pad, int& step) const;
    float cellW() const { return (float) (getWidth() - nameW) / 16.0f; }
    void showStepMenu (int pad, int step);
};

// Settings for the selected step: velocity, chance, when it plays, timing, ratchets.
class StepPanel : public juce::Component
{
public:
    explicit StepPanel (SequencerPage&);
    void refresh();                               // show the grid's selected step
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    SequencerPage& owner;
    juce::Label title, emptyLabel, velLabel, chanceLabel, condLabel, timeLabel, ratLabel;
    juce::Slider vel, chance, timing;
    juce::ComboBox cond;
    juce::TextButton rat[4], removeButton { "Remove step" };
    int shownPad = -1, shownStep = -1;
    bool hasStep() const;
};

class SequencerPage : public juce::Component
{
public:
    explicit SequencerPage (EditorContext&);
    ~SequencerPage() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    void padsChanged();
    PatternData& pattern();
    int visiblePage() const { return grid.page; }
    void ctrlModeChanged();                // the TempoPAD switched modes (hint text)
    void historyTick()                     // always, even while this page is hidden
    {
        history.tick();
        if (! isShowing()) proc.keysInput = -1;   // pads play drums unless a Keys track is on screen
    }
    void undo();
    void redo();
    void stepChanged();                    // the grid's selection or a step's settings changed
    StepGrid& getGrid() { return grid; }
    PadRomplerProcessor& getProc() { return proc; }
    EditorContext& getCtx() { return ctx; }
    void selectTrack (int t);              // 0 = drums, 1..4 = Keys tracks
    int getSelectedTrack() const { return selectedTrack; }

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;

    juce::TextButton playButton { "Play" }, recButton { "Rec" }, clearButton { "Clear" }, dupButton { "Copy to next" };
    juce::Label bpmLabel, transportInfo, lengthLabel, resLabel, swingLabel, patLabel;
    juce::Slider bpmSlider, swingSlider;
    juce::ComboBox lengthBox, resBox;
    juce::TextButton patButtons[Seq::numPatterns];
    juce::TextButton pageButtons[4];
    juce::ToggleButton followToggle { "Follow playhead" }, allPadsToggle { "Show all pads" }, tempoPadToggle { "TempoPAD modes" };
    juce::uint32 lastStepEdits = 0;
    juce::String defaultHint;
    StepGrid grid { ctx, *this };
    StepPanel stepPanel { *this };
    PatternHistory history { proc };
    juce::TextButton captureButton { "Capture" }, fillButton { "Fill" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::ComboBox rollBox;
    juce::Label statusLabel;
    juce::uint32 statusAt = 0;
    int panelTicks = 0;
    void setStatus (const juce::String&);
    juce::Viewport viewport;
    juce::Label hint;
    int lastStep = -2, lastPattern = -1, blink = 0;
    juce::Rectangle<int> barArea;

    void syncPatternControls();
    void setPage (int p);

    // Keys tracks
    PianoRoll roll { *this };
    juce::Viewport rollViewport;
    VelocityLane velLane { roll };
    NotePanel notePanel { *this, roll };
    juce::TextButton trackButtons[1 + Keys::numTracks];
    juce::Label soundLabel, rootLabel;
    juce::ComboBox soundBox, rootBox, scaleRootBox, scaleTypeBox;
    juce::ToggleButton monoToggle { "Mono" }, snapToggle { "Snap to scale" }, ghostToggle { "Show other tracks" },
                       padsPlayToggle { "Pads play this track" };
    juce::Slider glideSlider;
    int selectedTrack = 0;
    juce::uint32 lastKeysRecorded = 0;
    int lastKeysStep = -2;
    KeysLane* currentLane();               // nullptr while the drums are shown
    int pageCount();
    void syncTrackControls();
    void rebuildSoundBox();
    void updateHint();
};
