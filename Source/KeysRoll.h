#pragma once
#include "Common.h"

class SequencerPage;

namespace KeysUI
{
    juce::Colour trackColour (int track);          // 0..3; drums use UI::accent
    juce::String noteName (int pitch);             // middle C (60) = C3
    inline bool isBlack (int pitch) { const int pc = pitch % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
}

// Piano roll for one Keys track: a keyboard down the side, 16 steps per page across.
// Click empty space = add a note (drag right to set its length); click a note = select it,
// drag it to move, drag its right edge to resize, click it again (or right-click) to remove it.
class PianoRoll : public juce::Component
{
public:
    explicit PianoRoll (SequencerPage&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    static constexpr int rowH = 14, keyW = 56;
    int track = 0, page = 0;
    int selPitch = -1, selStep = -1;               // the selected note (pitch + start step)
    bool snap = true, ghosts = true;
    int lastLen = 2;
    std::function<void()> onChange;                // notes or selection changed

    int preferredHeight() const { return (Keys::highNote - Keys::lowNote + 1) * rowH; }
    int yForPitch (int p) const { return (Keys::highNote - p) * rowH; }
    int stepAt (int x) const;
    float xForStep (double step) const { return (float) keyW + (float) (step - page * 16) * cellW(); }
    float cellW() const { return (float) (getWidth() - keyW) / 16.0f; }
    KeysLane& lane() const;
    bool selectedNote (Keys::Note& out) const;
    void replaceSelected (std::function<void (Keys::Note&)> change);   // edit the selected note
    void removeSelected();
    int snapPitch (int p) const;

private:
    SequencerPage& owner;
    enum DragMode { none, moving, resizing };
    DragMode drag = none;
    int downStep = 0, downPitch = 0, origStep = 0, origPitch = 0;
    bool moved = false, wasSelected = false;
    int pitchAt (int y) const { return juce::jlimit (Keys::lowNote, Keys::highNote, Keys::highNote - y / rowH); }
    void setNotes (Keys::NoteList list);
    int findNote (const Keys::NoteList& list, int pitch, int step) const;   // index covering (pitch, step) or -1
};

// Velocity bars for the notes on the piano roll's page; drag a bar up or down.
class VelocityLane : public juce::Component
{
public:
    explicit VelocityLane (PianoRoll& r) : roll (r) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    PianoRoll& roll;
    void setFrom (const juce::MouseEvent&, bool pick);
};

// Settings for the selected note.
class NotePanel : public juce::Component
{
public:
    NotePanel (SequencerPage&, PianoRoll&);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    SequencerPage& owner;
    PianoRoll& roll;
    juce::Label title, emptyLabel, velLabel, lenLabel;
    juce::Slider vel, len;
    juce::TextButton removeButton { "Remove note" };
};
