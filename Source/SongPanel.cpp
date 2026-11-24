#include "SongPanel.h"

using namespace juce;
using namespace UI;

static String barsText (double beats)
{
    const double bars = beats / 4.0;
    const bool whole = std::abs (bars - std::round (bars)) < 1.0e-6;
    const String n = whole ? String ((int) std::round (bars)) : String (bars, 2).trimCharactersAtEnd ("0");
    return n + (whole && (int) std::round (bars) == 1 ? " bar" : " bars");
}

static String barNumber (double startBeats)     // "5", or "5.5" for a row that starts mid-bar
{
    const double bar = startBeats / 4.0 + 1.0;
    if (std::abs (bar - std::round (bar)) < 1.0e-6) return String ((int) std::round (bar));
    return String (bar, 2).trimCharactersAtEnd ("0");
}

// ===================================================================================== SongList

int SongList::preferredHeight() const { return jmax (1, ctx.getProc().song.rows()) * rowH; }

void SongList::paint (Graphics& g)
{
    auto& proc = ctx.getProc();
    const auto s = proc.songSnapshot();
    const int playing = proc.songRow.load();
    const int sel = jlimit (0, jmax (0, s.n - 1), proc.songCursor.load());
    const double ppq = proc.transportPpq();
    const int w = getWidth();

    for (int r = 0; r < s.n; ++r)
    {
        const int y = r * rowH;
        auto rowArea = Rectangle<int> (0, y, w, rowH - 3);
        g.setColour (r % 2 ? panel : panel.brighter (0.03f));
        g.fillRoundedRectangle (rowArea.toFloat(), 5.0f);

        if (r == playing && ppq >= 0.0 && s.rowLength (r) > 0.0)        // how far through the row
        {
            const double local = (s.loop ? std::fmod (ppq, s.total()) : ppq) - s.start[r];
            const float frac = (float) jlimit (0.0, 1.0, local / s.rowLength (r));
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (rowArea.withWidth ((int) (w * frac)).toFloat(), 5.0f);
        }
        if (r == sel)
        {
            g.setColour (accent);
            g.drawRoundedRectangle (rowArea.toFloat().reduced (0.5f), 5.0f, 1.5f);
        }

        g.setColour (r == playing ? accent : muted);
        g.setFont (Font (13.0f, r == playing ? Font::bold : Font::plain));
        g.drawText (String (r + 1), 0, y, numW - 8, rowH - 3, Justification::centredRight);

        for (int p = 0; p < Seq::numPatterns; ++p)                      // pattern picker
        {
            auto c = Rectangle<int> (patX() + p * cellW, y + 5, cellW - 3, rowH - 13).toFloat();
            const bool on = s.pattern[r] == p;
            g.setColour (on ? accent : line.withAlpha (0.6f));
            g.fillRoundedRectangle (c, 3.0f);
            g.setColour (on ? Colours::black : muted);
            g.setFont (Font (12.0f, on ? Font::bold : Font::plain));
            g.drawText (String (p + 1), c, Justification::centred);
        }

        const int rx = repX();                                           // repeats: - x4 +
        auto minus = Rectangle<int> (rx, y + 5, 26, rowH - 13).toFloat();
        auto plus  = Rectangle<int> (rx + repW - 26, y + 5, 26, rowH - 13).toFloat();
        g.setColour (line);
        g.fillRoundedRectangle (minus, 3.0f);
        g.fillRoundedRectangle (plus, 3.0f);
        g.setColour (text);
        g.setFont (Font (14.0f, Font::bold));
        g.drawText ("-", minus, Justification::centred);
        g.drawText ("+", plus, Justification::centred);
        g.setFont (Font (13.0f));
        g.drawText ("x" + String (s.repeats[r]), rx + 26, y, repW - 52, rowH - 3, Justification::centred);

        int x = rx + repW + 16;
        g.setColour (text);
        g.setFont (Font (12.0f));
        g.drawText (barsText (s.rowLength (r)), x, y, lenW, rowH - 3, Justification::centredLeft);
        x += lenW;
        g.setColour (muted);
        g.drawText ("starts at bar " + barNumber (s.start[r]), x, y, 140, rowH - 3, Justification::centredLeft);
    }
}

void SongList::mouseDown (const MouseEvent& e)
{
    auto& song = ctx.getProc().song;
    const int r = rowAt (e.y);
    if (r < 0 || r >= song.rows()) return;
    const int x = e.x;
    if (x >= patX() && x < patX() + Seq::numPatterns * cellW)
    {
        song.pattern[r].store ((uint8_t) jlimit (0, Seq::numPatterns - 1, (x - patX()) / cellW));
        if (onEdited) onEdited();
    }
    else if (x >= repX() && x < repX() + 26)
    {
        song.repeats[r].store ((uint8_t) jmax (1, song.repeats[r].load() - 1));
        if (onEdited) onEdited();
    }
    else if (x >= repX() + repW - 26 && x < repX() + repW)
    {
        song.repeats[r].store ((uint8_t) jmin (Song::maxRepeats, song.repeats[r].load() + 1));
        if (onEdited) onEdited();
    }
    if (onSelect) onSelect (r);
}

void SongList::mouseDoubleClick (const MouseEvent& e)
{
    const int r = rowAt (e.y);     // on the number or the length, not the pattern / repeat buttons
    const bool onControls = e.x >= patX() && e.x < repX() + repW;
    if (r >= 0 && r < ctx.getProc().song.rows() && ! onControls && onPlayFrom) onPlayFrom (r);
}

void SongList::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    auto& song = ctx.getProc().song;
    const int r = rowAt (e.y);
    if (r >= 0 && r < song.rows() && e.x >= repX() && e.x < repX() + repW && w.deltaY != 0.0f)
    {
        const int d = w.deltaY > 0 ? 1 : -1;
        song.repeats[r].store ((uint8_t) jlimit (1, Song::maxRepeats, song.repeats[r].load() + d));
        if (onEdited) onEdited();
        return;
    }
    Component::mouseWheelMove (e, w);    // let the viewport scroll
}

// ===================================================================================== SongPanel

SongPanel::SongPanel (EditorContext& c) : ctx (c), proc (c.getProc())
{
    for (auto* b : { &modeButton, &loopButton, &playFromButton, &dupButton, &delButton, &upButton, &downButton, &clearButton })
    {
        styleButton (*b);
        addAndMakeVisible (b);
    }
    modeButton.setClickingTogglesState (true);
    modeButton.setTooltip ("Song mode: the rows below play in order instead of the selected pattern. Off = patterns as usual.");
    modeButton.onClick = [this] { setSongMode (modeButton.getToggleState()); };
    loopButton.setClickingTogglesState (true);
    loopButton.setTooltip ("On: after the last row the song starts again. Off: the sequencer stops at the end.");
    loopButton.onClick = [this] { proc.songLoop = loopButton.getToggleState(); list.repaint(); };
    playFromButton.setTooltip ("Turn on Song mode and play from the selected row (FlipKit64's own transport). "
                               "In LUNA, the song follows the timeline: bar 1 is row 1.");
    playFromButton.onClick = [this] { playFrom (selected()); };
    dupButton.setTooltip ("Copy the selected row underneath it");
    dupButton.onClick = [this]
    {
        const int r = selected();
        if (proc.song.rows() == 0 || proc.song.rows() >= Song::maxRows) return;
        proc.song.insert (r + 1, proc.song.pattern[r].load(), proc.song.repeats[r].load());
        select (r + 1); edited();
    };
    delButton.onClick = [this]
    {
        const int r = selected();
        proc.song.remove (r);
        select (jmin (r, proc.song.rows() - 1)); edited();
    };
    upButton.onClick   = [this] { const int r = selected(); if (r > 0) { proc.song.swap (r, r - 1); select (r - 1); edited(); } };
    downButton.onClick = [this] { const int r = selected(); if (r + 1 < proc.song.rows()) { proc.song.swap (r, r + 1); select (r + 1); edited(); } };
    clearButton.onClick = [this]
    {
        if (proc.song.rows() == 0) return;
        AlertWindow::showOkCancelBox (MessageBoxIconType::QuestionIcon, "Clear song", "Remove every row from the song? (The patterns stay.)",
                                      "Clear", "Cancel", this, ModalCallbackFunction::create ([this] (int ok)
        {
            if (ok == 0) return;
            proc.song.clear(); proc.songCursor = 0; edited();
        }));
    };

    addLabel.setText ("Add pattern", dontSendNotification);
    styleLabel (addLabel, 11.0f);
    addAndMakeVisible (addLabel);
    for (int p = 0; p < Seq::numPatterns; ++p)
    {
        auto& b = addButtons[p];
        b.setButtonText (String (p + 1));
        styleButton (b);
        b.setTooltip ("Add pattern " + String (p + 1) + " after the selected row");
        b.onClick = [this, p]
        {
            if (proc.song.rows() >= Song::maxRows) { if (onStatus) onStatus ("A song can have up to 64 rows."); return; }
            const int at = proc.song.rows() == 0 ? 0 : selected() + 1;
            proc.song.insert (at, p, 1);
            select (at); edited();
        };
        addAndMakeVisible (b);
    }

    styleLabel (infoLabel, 12.0f, text);
    infoLabel.setJustificationType (Justification::centredRight);
    addAndMakeVisible (infoLabel);
    emptyLabel.setText ("No rows yet. Use Add pattern 1-8 to build the song: each row plays a pattern a number of times, top to bottom.\n"
                        "Then turn on Song mode, or press Play from row.", dontSendNotification);
    styleLabel (emptyLabel, 12.0f);
    emptyLabel.setJustificationType (Justification::centredTop);
    addAndMakeVisible (emptyLabel);

    list.onSelect = [this] (int r) { select (r); };
    list.onPlayFrom = [this] (int r) { playFrom (r); };
    list.onEdited = [this] { edited(); };
    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
    edited();
}

int SongPanel::selected() const { return jlimit (0, jmax (0, proc.song.rows() - 1), proc.songCursor.load()); }

void SongPanel::select (int row)
{
    row = jlimit (0, jmax (0, proc.song.rows() - 1), row);
    proc.songCursor = row;
    // stopped: show that row's pattern on the Drums / Keys grids so you can edit it
    if (proc.song.rows() > 0 && ! proc.hostPlaying.load() && ! proc.internalPlaying.load())
        proc.selectPattern (proc.song.pattern[row].load());
    list.repaint();
    if (viewport.getHeight() > 0)
    {
        const int y = row * SongList::rowH, top = viewport.getViewPositionY();
        if (y < top) viewport.setViewPosition (0, y);
        else if (y + SongList::rowH > top + viewport.getHeight()) viewport.setViewPosition (0, y + SongList::rowH - viewport.getHeight());
    }
}

void SongPanel::setSongMode (bool on)
{
    if (on && proc.song.rows() == 0)            // nothing to play yet: start the song with the current pattern
    {
        proc.song.insert (0, proc.currentPattern.load(), 1);
        proc.songCursor = 0;
        if (onStatus) onStatus ("Song mode on: started the song with pattern " + String (proc.currentPattern.load() + 1) + ". Add more rows below.");
        edited();
    }
    proc.songMode = on;
    modeButton.setToggleState (on, dontSendNotification);
}

void SongPanel::playFrom (int row)
{
    if (proc.song.rows() == 0) return;
    select (row);
    setSongMode (true);
    if (proc.hostPlaying.load())
    {
        if (onStatus) onStatus ("LUNA is playing: the song follows its timeline. Row " + String (row + 1) + " starts at bar "
                                + String (Song::rowStart (proc.songSnapshot(), row) / 4.0 + 1.0, 0) + ".");
        return;
    }
    if (! proc.internalPlaying.load()) { proc.internalPlaying = true; return; }
    proc.internalPlaying = false;               // already running: stop for a block, then restart from the row
    Component::SafePointer<SongPanel> safe (this);
    Timer::callAfterDelay (60, [safe] { if (safe != nullptr) safe->proc.internalPlaying = true; });
}

void SongPanel::edited()
{
    list.setSize (jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()), list.preferredHeight());
    list.repaint();
    emptyLabel.setVisible (proc.song.rows() == 0);
    lastRows = -1;
    tick();
}

void SongPanel::tick()
{
    modeButton.setToggleState (proc.songMode.load(), dontSendNotification);
    modeButton.setButtonText (proc.songMode.load() ? "Song mode: on" : "Song mode: off");
    loopButton.setToggleState (proc.songLoop.load(), dontSendNotification);
    const int rows = proc.song.rows();
    for (auto* b : { &dupButton, &delButton, &upButton, &downButton, &playFromButton, &clearButton }) b->setEnabled (rows > 0);
    if (rows != lastRows)
    {
        lastRows = rows;
        if (list.getHeight() != list.preferredHeight()) list.setSize (list.getWidth(), list.preferredHeight());
        emptyLabel.setVisible (rows == 0);
        const auto s = proc.songSnapshot();
        const double bpm = proc.hostPlaying.load() ? proc.hostBpm.load() : proc.internalBpm.load();
        const int secs = (int) std::round (s.total() * 60.0 / jmax (1.0, bpm));
        infoLabel.setText (rows == 0 ? String() : String (rows) + (rows == 1 ? " row, " : " rows, ") + barsText (s.total())
                                                  + String::formatted (",  %d:%02d at ", secs / 60, secs % 60) + String (bpm, 0) + " BPM",
                           dontSendNotification);
    }
    const int row = proc.songRow.load();
    const double ppq = proc.transportPpq();
    if (row != lastRow || (row >= 0 && std::abs (ppq - lastPpq) > 0.05))
    {
        if (row != lastRow && row >= 0)          // keep the playing row in view
        {
            const int y = row * SongList::rowH, top = viewport.getViewPositionY();
            if (y < top || y + SongList::rowH > top + viewport.getHeight()) viewport.setViewPosition (0, jmax (0, y - SongList::rowH));
        }
        lastRow = row; lastPpq = ppq;
        list.repaint();
    }
}

void SongPanel::paint (Graphics&) {}

void SongPanel::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (28);
    modeButton.setBounds (top.removeFromLeft (130)); top.removeFromLeft (6);
    loopButton.setBounds (top.removeFromLeft (60)); top.removeFromLeft (6);
    playFromButton.setBounds (top.removeFromLeft (110)); top.removeFromLeft (18);
    addLabel.setBounds (top.removeFromLeft (76));
    for (auto& b : addButtons) { b.setBounds (top.removeFromLeft (30)); top.removeFromLeft (3); }
    top.removeFromLeft (12);
    infoLabel.setBounds (top);

    r.removeFromTop (8);
    auto bottom = r.removeFromBottom (28);
    dupButton.setBounds (bottom.removeFromLeft (86)); bottom.removeFromLeft (6);
    upButton.setBounds (bottom.removeFromLeft (80)); bottom.removeFromLeft (6);
    downButton.setBounds (bottom.removeFromLeft (90)); bottom.removeFromLeft (6);
    delButton.setBounds (bottom.removeFromLeft (70));
    clearButton.setBounds (bottom.removeFromRight (90));
    r.removeFromBottom (8);
    viewport.setBounds (r);
    emptyLabel.setBounds (r.reduced (20, 30));
    list.setSize (jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()), list.preferredHeight());
}
