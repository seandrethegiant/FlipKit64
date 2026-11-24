#pragma once
#include "Common.h"

// Song mode editor (Sequencer tab > Song): a list of rows, each a pattern played a number of times,
// top to bottom. Click a row's pattern number to change it, - / + for repeats, click a row to pick
// it (Play starts there), double-click to play from it.
class SongList : public juce::Component
{
public:
    explicit SongList (EditorContext& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    int preferredHeight() const;
    std::function<void (int)> onSelect;          // a row was clicked
    std::function<void (int)> onPlayFrom;        // a row was double-clicked
    std::function<void()> onEdited;

    static constexpr int rowH = 34, numW = 44, cellW = 30, repW = 120, lenW = 90;
    int patX() const { return numW; }
    int repX() const { return numW + Seq::numPatterns * cellW + 18; }

private:
    EditorContext& ctx;
    int rowAt (int y) const { return y / rowH; }
};

class SongPanel : public juce::Component
{
public:
    explicit SongPanel (EditorContext&);
    void resized() override;
    void paint (juce::Graphics&) override;
    void tick();                                  // ~30x a second while shown
    void setSongMode (bool on);                   // also used by the Song button above the grid
    std::function<void (const juce::String&)> onStatus;

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;
    juce::TextButton modeButton { "Song mode" }, loopButton { "Loop" }, playFromButton { "Play from row" },
                     dupButton { "Duplicate" }, delButton { "Delete" }, upButton { "Move up" }, downButton { "Move down" },
                     clearButton { "Clear song" };
    juce::TextButton addButtons[Seq::numPatterns];
    juce::Label addLabel, infoLabel, emptyLabel;
    SongList list { ctx };
    juce::Viewport viewport;
    int lastRow = -2, lastRows = -1;
    double lastPpq = -1.0;
    void select (int row);
    void playFrom (int row);
    void edited();
    int selected() const;
};
