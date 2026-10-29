#pragma once
#include "Common.h"

class SequencerPage;

// The step grid: one row per pad, 16 steps per page.
// Click = toggle a step (Shift = accent), drag up/down on a step = velocity,
// right-click = velocity / ratchet menu. Click a pad name to hear it.
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
    static constexpr int rowH = 26, nameW = 170;

private:
    EditorContext& ctx;
    SequencerPage& owner;
    int dragPad = -1, dragStep = -1, dragStartY = 0, dragStartVel = 0;
    bool turnedOn = false, moved = false;
    bool cellAt (juce::Point<int> p, int& pad, int& step) const;
    float cellW() const { return (float) (getWidth() - nameW) / 16.0f; }
    void showStepMenu (int pad, int step);
};

class SequencerPage : public juce::Component
{
public:
    explicit SequencerPage (EditorContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();
    void padsChanged();
    PatternData& pattern();
    int visiblePage() const { return grid.page; }
    void ctrlModeChanged();                // the TempoPAD switched modes (hint text)

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
    juce::Viewport viewport;
    juce::Label hint;
    int lastStep = -2, lastPattern = -1, blink = 0;
    juce::Rectangle<int> barArea;

    void syncPatternControls();
    void setPage (int p);
};
