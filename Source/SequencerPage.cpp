#include "SequencerPage.h"

using namespace juce;
using namespace UI;

// ===================================================================================== PatternHistory

PatternHistory::PatternHistory (PadRomplerProcessor& p) : proc (p)
{
    for (int i = 0; i < Seq::numPatterns; ++i)
    {
        stable[i] = std::make_unique<PatternData>();
        seen[i] = std::make_unique<PatternData>();
    }
    resetAll();
}

void PatternHistory::resetAll()
{
    for (int i = 0; i < Seq::numPatterns; ++i)
    {
        stable[i]->copyFrom (proc.patterns[i]);
        seen[i]->copyFrom (proc.patterns[i]);
        dirty[i] = false;
    }
    undoStack.clear(); redoStack.clear();
    lastKitLoads = proc.kitLoads.load();
}

void PatternHistory::commit (int p)
{
    dirty[p] = false;
    if (proc.patterns[p].sameAs (*stable[p])) return;
    auto before = std::make_unique<PatternData>();
    before->copyFrom (*stable[p]);
    undoStack.push_back ({ p, std::move (before) });
    if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
    redoStack.clear();
    stable[p]->copyFrom (proc.patterns[p]);
    seen[p]->copyFrom (proc.patterns[p]);
}

void PatternHistory::flush()
{
    for (int p = 0; p < Seq::numPatterns; ++p)
        if (dirty[p] || ! proc.patterns[p].sameAs (*stable[p])) commit (p);
}

void PatternHistory::tick()
{
    if (proc.kitLoads.load() != lastKitLoads) { resetAll(); return; }   // a kit or project replaced everything
    if (++ticks % 3 != 0) return;
    const auto now = Time::getMillisecondCounter();
    for (int p = 0; p < Seq::numPatterns; ++p)
    {
        if (! proc.patterns[p].sameAs (*seen[p]))
        {
            seen[p]->copyFrom (proc.patterns[p]);
            changedAt[p] = now;
            dirty[p] = true;
        }
        else if (dirty[p] && now - changedAt[p] > 400)                  // settled: one undo step
            commit (p);
    }
}

bool PatternHistory::canUndo() const
{
    if (! undoStack.empty()) return true;
    for (bool d : dirty) if (d) return true;
    return false;
}

void PatternHistory::apply (int p, const PatternData& d)
{
    proc.patterns[p].copyFrom (d);
    stable[p]->copyFrom (d);
    seen[p]->copyFrom (d);
    dirty[p] = false;
    if (p != proc.currentPattern.load()) proc.selectPattern (p);
}

bool PatternHistory::undo()
{
    flush();
    if (undoStack.empty()) return false;
    auto e = std::move (undoStack.back());
    undoStack.pop_back();
    auto now = std::make_unique<PatternData>();
    now->copyFrom (proc.patterns[e.pattern]);
    redoStack.push_back ({ e.pattern, std::move (now) });
    apply (e.pattern, *e.data);
    return true;
}

bool PatternHistory::redo()
{
    flush();
    if (redoStack.empty()) return false;
    auto e = std::move (redoStack.back());
    redoStack.pop_back();
    auto now = std::make_unique<PatternData>();
    now->copyFrom (proc.patterns[e.pattern]);
    undoStack.push_back ({ e.pattern, std::move (now) });
    apply (e.pattern, *e.data);
    return true;
}

// ===================================================================================== StepGrid

bool StepGrid::cellAt (Point<int> p, int& pad, int& step) const
{
    const int r = p.y / rowH;
    if (! isPositiveAndBelow (r, (int) rows.size()) || p.x < nameW) return false;
    pad = rows[(size_t) r];
    step = page * 16 + jlimit (0, 15, (int) ((p.x - nameW) / cellW()));
    return true;
}

void StepGrid::paint (Graphics& g)
{
    auto& pat = owner.pattern();
    const int len = pat.length.load();
    // shade by beat; 1/4 triplets don't fit in a beat, so they're grouped in twos-of-beats (3 steps)
    const int s2 = jmax (1, pat.stepsPer2Beats.load());
    const int spb = (s2 % 2 == 0) ? s2 / 2 : s2;
    const int playStep = ctx.getProc().playStep.load();
    const float cw = cellW();

    for (int r = 0; r < (int) rows.size(); ++r)
    {
        const int pad = rows[(size_t) r];
        const int y = r * rowH;
        auto ps = ctx.getProc().getPad (pad);
        const Colour hue = padHue (pad);
        const bool sel = pad == ctx.getSelectedPad();

        // name column
        g.setColour (sel ? accent.withAlpha (0.18f) : (r % 2 ? panel : panel.brighter (0.025f)));
        g.fillRect (0, y, nameW - 4, rowH - 2);
        g.setColour (hue);
        g.fillRect (0, y, 3, rowH - 2);
        g.setColour (muted);
        g.setFont (Font (10.0f));
        g.drawText (String (PadLayout::noteForPad (pad)), 8, y, 24, rowH - 2, Justification::centredLeft);
        g.setColour (ps->layers.empty() ? muted : text);
        g.setFont (Font (12.0f));
        g.drawText (ps->name.isEmpty() ? String ("(empty)") : ps->name, 34, y, nameW - 40, rowH - 2, Justification::centredLeft);

        for (int c = 0; c < 16; ++c)
        {
            const int step = page * 16 + c;
            auto cell = Rectangle<float> (nameW + c * cw, (float) y, cw, (float) rowH - 2).reduced (1.5f, 1.0f);
            if (step >= len) { g.setColour (bg); g.fillRect (cell); continue; }
            const bool beatStart = (step % spb) == 0;
            Colour base = ((step / spb) % 2 == 0) ? panel.brighter (0.07f) : panel.brighter (0.03f);
            if (step == playStep) base = base.brighter (0.25f);
            g.setColour (base);
            g.fillRoundedRectangle (cell, 3.0f);
            if (beatStart) { g.setColour (line.brighter (0.2f)); g.fillRect (cell.getX() - 1.5f, cell.getY(), 1.0f, cell.getHeight()); }

            const int vel = pat.vel[pad][step].load();
            if (vel > 0)
            {
                const int ch = pat.chance[pad][step].load(), cd = pat.cond[pad][step].load(), nd = pat.nudge[pad][step].load() - 128;
                const float a = (0.35f + 0.65f * vel / 127.0f) * (ch < 100 ? 0.55f : 1.0f);   // dimmed = plays sometimes
                g.setColour (hue.withAlpha (a));
                g.fillRoundedRectangle (cell.reduced (2.0f), 3.0f);
                if (nd != 0)                                   // early / late: an orange edge on that side
                {
                    g.setColour (accent);
                    g.fillRect (nd < 0 ? cell.getX() + 2.0f : cell.getRight() - 5.0f, cell.getY() + 2.0f, 3.0f, cell.getHeight() - 4.0f);
                }
                const String tag = cd != Seq::always ? String (Seq::condShort (cd)) : (ch < 100 ? String (ch) + "%" : String());
                if (tag.isNotEmpty())
                {
                    g.setColour (Colour (0xff120c1c));
                    g.setFont (Font (9.5f, Font::bold));
                    g.drawText (tag, cell.reduced (4.0f, 0.0f), Justification::centred);
                }
                // velocity bar
                g.setColour (Colours::white.withAlpha (0.55f));
                const float bh = (cell.getHeight() - 6.0f) * vel / 127.0f;
                g.fillRect (cell.getRight() - 5.0f, cell.getBottom() - 3.0f - bh, 2.0f, bh);
                // ratchet ticks
                const int rat = jlimit (1, 4, (int) pat.ratchet[pad][step].load());
                if (rat > 1)
                {
                    g.setColour (Colours::black.withAlpha (0.7f));
                    for (int k = 0; k < rat; ++k)
                        g.fillRect (cell.getX() + 4.0f + k * 4.0f, cell.getY() + 4.0f, 2.0f, cell.getHeight() - 8.0f);
                }
            }
            if (step == playStep) { g.setColour (accent); g.drawRoundedRectangle (cell, 3.0f, 1.2f); }
            if (pad == selPad && step == selStep && vel > 0) { g.setColour (text); g.drawRoundedRectangle (cell.reduced (0.5f), 3.0f, 2.0f); }
        }
    }

    // Keys tracks strip: each track's notes placed in time against the drum steps; click one to open it
    const int top = laneTop();
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText ("KEYS TRACKS", 4, top + 6, nameW, laneHeaderH - 6, Justification::centredLeft);
    const double drumSL = pat.stepLength();
    const double winStart = page * 16 * drumSL, winEnd = winStart + 16 * drumSL;
    for (int t = 0; t < Keys::numTracks; ++t)
    {
        const int y = top + laneHeaderH + t * laneRowH;
        const auto& kt = ctx.getProc().keysTracks[t];
        g.setColour (t % 2 ? panel : panel.brighter (0.025f));
        g.fillRect (0, y, nameW - 4, laneRowH - 2);
        g.setColour (KeysUI::trackColour (t));
        g.fillEllipse (8.0f, (float) y + laneRowH * 0.5f - 4.0f, 8.0f, 8.0f);
        g.setColour (kt.sourcePad.load() >= 0 ? text : muted);
        g.setFont (Font (12.0f));
        g.drawText (kt.name, 24, y, nameW - 30, laneRowH - 2, Justification::centredLeft);
        g.setColour (Colour (0xff1b1430));
        g.fillRect ((float) nameW, (float) y, cw * 16.0f, (float) laneRowH - 2.0f);
        const auto& ln = pat.lanes[t];
        auto notes = ln.get();
        if (notes == nullptr) continue;
        const double sl = ln.stepLength(), loop = jlimit (1, Seq::maxSteps, ln.length.load()) * sl;
        g.setColour (KeysUI::trackColour (t));
        for (long long m = (long long) std::floor (winStart / loop); m * loop < winEnd; ++m)
            for (const auto& n : *notes)
            {
                const double s = m * loop + n.step * sl, e = s + n.len * sl;
                if (e <= winStart || s >= winEnd) continue;
                const float x0 = nameW + (float) ((jmax (s, winStart) - winStart) / drumSL) * cw;
                const float x1 = nameW + (float) ((jmin (e, winEnd) - winStart) / drumSL) * cw;
                const float ny = (float) y + 3.0f + (float) (Keys::highNote - n.pitch) / (Keys::highNote - Keys::lowNote) * (laneRowH - 9.0f);
                g.fillRoundedRectangle (x0 + 1.0f, ny, jmax (2.0f, x1 - x0 - 2.0f), 3.0f, 1.5f);
            }
    }
}

void StepGrid::mouseDown (const MouseEvent& e)
{
    if (e.y >= laneTop() + laneHeaderH)                // the Keys tracks strip: open that track
    {
        const int t = (e.y - laneTop() - laneHeaderH) / laneRowH;
        if (isPositiveAndBelow (t, Keys::numTracks)) owner.selectTrack (t + 1);
        return;
    }
    const int r = e.y / rowH;
    if (! isPositiveAndBelow (r, (int) rows.size())) return;
    if (e.x < nameW)
    {
        ctx.selectPad (rows[(size_t) r]);
        ctx.getProc().triggerPad (rows[(size_t) r], 0.9f);
        repaint();
        return;
    }
    int pad, step;
    if (! cellAt (e.getPosition(), pad, step) || step >= owner.pattern().length.load()) return;
    if (e.mods.isPopupMenu()) { showStepMenu (pad, step); return; }

    auto& pat = owner.pattern();
    dragPad = pad; dragStep = step; dragStartY = e.y; moved = false;
    wasSelected = (pad == selPad && step == selStep);
    if (pat.vel[pad][step].load() == 0)
    {
        pat.setStep (pad, step, (uint8_t) (e.mods.isShiftDown() ? 127 : 100));
        turnedOn = true;
        ctx.getProc().triggerPad (pad, e.mods.isShiftDown() ? 1.0f : 100.0f / 127.0f);
    }
    else turnedOn = false;
    selPad = pad; selStep = step;
    dragStartVel = pat.vel[pad][step].load();
    owner.stepChanged();
    repaint();
}

void StepGrid::mouseDrag (const MouseEvent& e)
{
    if (dragPad < 0) return;
    const int dy = dragStartY - e.y;
    if (! moved && std::abs (dy) < 4) return;
    moved = true;
    owner.pattern().vel[dragPad][dragStep].store ((uint8_t) jlimit (1, 127, dragStartVel + dy));
    owner.stepChanged();
    repaint();
}

void StepGrid::mouseUp (const MouseEvent&)
{
    if (dragPad >= 0 && ! turnedOn && ! moved && wasSelected)     // second click on the selected step removes it
    {
        owner.pattern().vel[dragPad][dragStep].store (0);
        selPad = selStep = -1;
        owner.stepChanged();
    }
    dragPad = -1;
    repaint();
}

void StepGrid::showStepMenu (int pad, int step)
{
    auto& pat = owner.pattern();
    const int vel = pat.vel[pad][step].load(), rat = pat.ratchet[pad][step].load();
    PopupMenu m;
    m.addSectionHeader ("Velocity");
    for (int v : { 127, 100, 80, 60, 40, 20 }) m.addItem (v, String (v), true, vel == v);
    m.addSectionHeader ("Ratchet (hits per step)");
    for (int r = 1; r <= 4; ++r) m.addItem (1000 + r, String (r) + "x", true, vel > 0 && rat == r);
    PopupMenu chanceMenu, playsMenu, timingMenu;
    const int ch = pat.chance[pad][step].load(), cd = pat.cond[pad][step].load(), nd = pat.nudge[pad][step].load() - 128;
    for (int c : { 100, 75, 50, 33, 25, 10 }) chanceMenu.addItem (3000 + c, String (c) + "%", true, vel > 0 && ch == c);
    for (int c = 0; c < Seq::numConds; ++c) playsMenu.addItem (4000 + c, Seq::condName (c), true, vel > 0 && cd == c);
    for (int t : { -50, -30, -20, -10, 0, 10, 20, 30, 50 })
        timingMenu.addItem (5100 + t, t == 0 ? String ("On the grid") : (t < 0 ? String (-t) + "% early" : String (t) + "% late"), true, vel > 0 && nd == t);
    m.addSubMenu ("Chance", chanceMenu);
    m.addSubMenu ("Plays", playsMenu);
    m.addSubMenu ("Timing", timingMenu);
    m.addSeparator();
    m.addItem (2000, "Clear step", vel > 0);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                     [this, pad, step] (int res)
                     {
                         auto& p = owner.pattern();
                         if (res <= 0) return;
                         if (res <= 127) { if (p.vel[pad][step].load() == 0) p.setStep (pad, step, (uint8_t) res); else p.vel[pad][step].store ((uint8_t) res); }
                         else if (res > 1000 && res <= 1004)
                         {
                             if (p.vel[pad][step].load() == 0) p.setStep (pad, step, 100);
                             p.ratchet[pad][step].store ((uint8_t) (res - 1000));
                         }
                         else if (res == 2000) p.vel[pad][step].store (0);
                         else if (res >= 3000 && res <= 3100) { if (p.vel[pad][step].load() == 0) p.setStep (pad, step, 100); p.chance[pad][step].store ((uint8_t) (res - 3000)); }
                         else if (res >= 4000 && res < 4000 + Seq::numConds) { if (p.vel[pad][step].load() == 0) p.setStep (pad, step, 100); p.cond[pad][step].store ((uint8_t) (res - 4000)); }
                         else if (res >= 5050 && res <= 5150) { if (p.vel[pad][step].load() == 0) p.setStep (pad, step, 100); p.nudge[pad][step].store ((uint8_t) (128 + res - 5100)); }
                         selPad = pad; selStep = step;
                         owner.stepChanged();
                         repaint();
                     });
}

// ===================================================================================== StepPanel

StepPanel::StepPanel (SequencerPage& o) : owner (o)
{
    styleLabel (title, 13.0f, text);
    title.setFont (Font (13.0f, Font::bold));
    addAndMakeVisible (title);
    styleLabel (emptyLabel, 12.0f);
    emptyLabel.setJustificationType (Justification::topLeft);
    emptyLabel.setText ("Click a step to set its chance, when it plays, its timing and ratchets.", dontSendNotification);
    addAndMakeVisible (emptyLabel);

    auto setup = [this] (Label& l, const String& name, Slider& s, double lo, double hi, double stepSize)
    {
        l.setText (name, dontSendNotification); styleLabel (l, 11.0f); addAndMakeVisible (l);
        s.setSliderStyle (Slider::LinearHorizontal);
        s.setRange (lo, hi, stepSize);
        s.setTextBoxStyle (Slider::TextBoxRight, false, 70, 20);
        s.setColour (Slider::thumbColourId, accent);
        s.setColour (Slider::trackColourId, accent.withAlpha (0.5f));
        s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        s.setColour (Slider::textBoxTextColourId, text);
        addAndMakeVisible (s);
    };
    setup (velLabel, "Velocity", vel, 1, 127, 1);
    setup (chanceLabel, "Chance", chance, 5, 100, 5);
    chance.setTextValueSuffix ("%");
    setup (timeLabel, "Timing", timing, -50, 50, 5);
    timing.textFromValueFunction = [] (double v)
    {
        const int t = (int) std::lround (v);
        return t == 0 ? String ("On grid") : (t < 0 ? String (-t) + "% early" : String (t) + "% late");
    };
    timing.valueFromTextFunction = [] (const String& s) { return (double) s.getIntValue() * (s.containsIgnoreCase ("early") ? -1 : 1); };
    timing.setDoubleClickReturnValue (true, 0.0);
    timing.updateText();
    timing.setTooltip ("Nudge the step early (laid back... pushed) or late, up to half a step");

    condLabel.setText ("Plays", dontSendNotification); styleLabel (condLabel, 11.0f); addAndMakeVisible (condLabel);
    for (int c = 0; c < Seq::numConds; ++c) cond.addItem (Seq::condName (c), c + 1);
    cond.setTooltip ("When the step plays, counted in loops of the pattern. Fill steps play while Fill is on.");
    addAndMakeVisible (cond);

    ratLabel.setText ("Ratchet", dontSendNotification); styleLabel (ratLabel, 11.0f); addAndMakeVisible (ratLabel);
    for (int i = 0; i < 4; ++i)
    {
        rat[i].setButtonText (i == 0 ? String ("Off") : "x" + String (i + 1));
        styleButton (rat[i]);
        rat[i].setClickingTogglesState (false);
        addAndMakeVisible (rat[i]);
    }
    styleButton (removeButton);
    addAndMakeVisible (removeButton);

    auto edit = [this] (std::function<void (PatternData&, int, int)> f)
    {
        if (! hasStep()) return;
        f (owner.pattern(), shownPad, shownStep);
        owner.getGrid().repaint();
    };
    vel.onValueChange = [this, edit] { edit ([this] (PatternData& p, int pad, int st) { p.vel[pad][st].store ((uint8_t) vel.getValue()); }); };
    chance.onValueChange = [this, edit] { edit ([this] (PatternData& p, int pad, int st) { p.chance[pad][st].store ((uint8_t) chance.getValue()); }); };
    timing.onValueChange = [this, edit] { edit ([this] (PatternData& p, int pad, int st) { p.nudge[pad][st].store ((uint8_t) (128 + (int) std::lround (timing.getValue()))); }); };
    cond.onChange = [this, edit] { edit ([this] (PatternData& p, int pad, int st) { p.cond[pad][st].store ((uint8_t) jlimit (0, Seq::numConds - 1, cond.getSelectedId() - 1)); }); };
    for (int i = 0; i < 4; ++i)
        rat[i].onClick = [this, edit, i] { edit ([i] (PatternData& p, int pad, int st) { p.ratchet[pad][st].store ((uint8_t) (i + 1)); }); refresh(); };
    removeButton.onClick = [this, edit]
    {
        edit ([] (PatternData& p, int pad, int st) { p.vel[pad][st].store (0); });
        owner.getGrid().selPad = owner.getGrid().selStep = -1;
        refresh();
    };
    refresh();
}

bool StepPanel::hasStep() const
{
    return shownPad >= 0 && shownStep >= 0 && owner.pattern().vel[shownPad][shownStep].load() > 0;
}

void StepPanel::refresh()
{
    auto& g = owner.getGrid();
    shownPad = g.selPad; shownStep = g.selStep;
    const bool on = hasStep();
    for (Component* c : std::initializer_list<Component*> { &velLabel, &vel, &chanceLabel, &chance, &condLabel, &cond, &timeLabel, &timing, &ratLabel, &rat[0], &rat[1], &rat[2], &rat[3], &removeButton })
        c->setVisible (on);
    emptyLabel.setVisible (! on);
    if (! on) { title.setText ("STEP", dontSendNotification); return; }
    auto& p = owner.pattern();
    auto ps = owner.getProc().getPad (shownPad);
    title.setText ((ps->name.isEmpty() ? "Pad " + String (PadLayout::noteForPad (shownPad)) : ps->name) + ", step " + String (shownStep + 1), dontSendNotification);
    vel.setValue (p.vel[shownPad][shownStep].load(), dontSendNotification);
    chance.setValue (p.chance[shownPad][shownStep].load(), dontSendNotification);
    timing.setValue (p.nudge[shownPad][shownStep].load() - 128, dontSendNotification);
    timing.updateText();
    cond.setSelectedId (p.cond[shownPad][shownStep].load() + 1, dontSendNotification);
    const int r = p.ratchet[shownPad][shownStep].load();
    for (int i = 0; i < 4; ++i) rat[i].setToggleState (r == i + 1, dontSendNotification);
}

void StepPanel::paint (Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void StepPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    title.setBounds (r.removeFromTop (20));
    r.removeFromTop (6);
    emptyLabel.setBounds (r.withHeight (80));
    auto row = [&r] (Label& l, Component& c, int h)
    {
        l.setBounds (r.removeFromTop (16));
        c.setBounds (r.removeFromTop (h));
        r.removeFromTop (8);
    };
    row (velLabel, vel, 22);
    row (chanceLabel, chance, 22);
    row (condLabel, cond, 26);
    row (timeLabel, timing, 22);
    ratLabel.setBounds (r.removeFromTop (16));
    auto rr = r.removeFromTop (26);
    const int w = (rr.getWidth() - 9) / 4;
    for (int i = 0; i < 4; ++i) { rat[i].setBounds (rr.removeFromLeft (w)); rr.removeFromLeft (3); }
    r.removeFromTop (12);
    removeButton.setBounds (r.removeFromTop (26));
}

// ===================================================================================== SequencerPage

static const int resSpb2[] = { 2, 3, 4, 6, 8, 12, 16 };   // steps per two beats, in menu order
static constexpr int numRes = 7;

SequencerPage::SequencerPage (EditorContext& c) : ctx (c), proc (c.getProc())
{
    for (auto* b : { &playButton, &recButton, &clearButton, &dupButton }) { styleButton (*b); addAndMakeVisible (b); }
    playButton.onClick = [this] { proc.internalPlaying = ! proc.internalPlaying.load(); };
    recButton.setClickingTogglesState (true);
    recButton.setColour (TextButton::buttonOnColourId, rec);
    recButton.setTooltip ("Live record: hits you play on the TempoPAD go into the pattern, snapped to the nearest step");
    recButton.onClick = [this] { proc.seqRecord = recButton.getToggleState(); };
    clearButton.onClick = [this]
    {
        if (auto* l = currentLane()) { l->set (nullptr); roll.selPitch = roll.selStep = -1; roll.repaint(); notePanel.refresh(); velLane.repaint(); }
        else { pattern().clearDrums(); grid.repaint(); padsChanged(); stepChanged(); }
    };

    for (auto* b : { &captureButton, &fillButton, &undoButton, &redoButton }) { styleButton (*b); addAndMakeVisible (b); }
    captureButton.setTooltip ("Turn what you just played on the pads into the pattern (the last loop's worth), even with Rec off");
    captureButton.setColour (TextButton::textColourOffId, zoneCyan);
    captureButton.onClick = [this]
    {
        const int n = proc.captureToPattern();
        if (n < 0) setStatus ("Capture works while the pattern plays: press Play, jam on the pads, then Capture.");
        else if (n == 0) setStatus ("Nothing to capture yet: play some pads while the pattern loops.");
        else setStatus ("Captured " + String (n) + (n == 1 ? " hit" : " hits") + " into pattern " + String (proc.currentPattern.load() + 1) + ".");
        padsChanged();
    };
    fillButton.setClickingTogglesState (true);
    fillButton.setTooltip ("Fill: steps set to \"Only in fills\" play and \"Not in fills\" steps rest while this is on");
    fillButton.onClick = [this] { proc.fillOn = fillButton.getToggleState(); };
    undoButton.setTooltip ("Undo the last sequencer change (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Shift+Z)");
    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    rollBox.addItem ("Roll off", 1);
    rollBox.addItem ("Roll 1/8", 2);
    rollBox.addItem ("Roll 1/16", 3);
    rollBox.addItem ("Roll 1/32", 4);
    rollBox.setSelectedId (1, dontSendNotification);
    rollBox.setTooltip ("Roll: hold a pad and it repeats in time (note repeat)");
    rollBox.onChange = [this] { static const int divs[] = { 0, 2, 4, 8 }; proc.rollDiv = divs[jlimit (1, 4, rollBox.getSelectedId()) - 1]; };
    addAndMakeVisible (rollBox);
    styleLabel (statusLabel, 11.0f, zoneCyan);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (stepPanel);
    dupButton.setTooltip ("Copy this pattern into the next pattern slot and switch to it");
    dupButton.onClick = [this]
    {
        const int cur = proc.currentPattern.load();
        if (cur + 1 >= Seq::numPatterns) return;
        proc.patterns[cur + 1].copyFrom (proc.patterns[cur]);
        proc.selectPattern (cur + 1);
    };

    bpmLabel.setText ("BPM", dontSendNotification); styleLabel (bpmLabel, 11.0f); addAndMakeVisible (bpmLabel);
    bpmSlider.setSliderStyle (Slider::IncDecButtons);
    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (proc.internalBpm.load(), dontSendNotification);
    bpmSlider.setTextBoxStyle (Slider::TextBoxLeft, false, 44, 22);
    bpmSlider.setColour (Slider::textBoxTextColourId, text);
    bpmSlider.setColour (Slider::textBoxOutlineColourId, line);
    bpmSlider.onValueChange = [this] { proc.internalBpm = bpmSlider.getValue(); };
    addAndMakeVisible (bpmSlider);
    styleLabel (transportInfo, 11.0f);
    addAndMakeVisible (transportInfo);

    patLabel.setText ("Pattern", dontSendNotification); styleLabel (patLabel, 11.0f); addAndMakeVisible (patLabel);
    for (int i = 0; i < Seq::numPatterns; ++i)
    {
        patButtons[i].setButtonText (String (i + 1));
        styleButton (patButtons[i]);
        patButtons[i].onClick = [this, i] { proc.selectPattern (i); syncPatternControls(); };
        addAndMakeVisible (patButtons[i]);
    }

    lengthLabel.setText ("Length", dontSendNotification); styleLabel (lengthLabel, 11.0f); addAndMakeVisible (lengthLabel);
    for (int l : { 16, 32, 48, 64 }) lengthBox.addItem (String (l) + " steps", l);
    lengthBox.onChange = [this]
    {
        if (auto* l = currentLane()) l->length = lengthBox.getSelectedId();
        else pattern().length = lengthBox.getSelectedId();
        setPage (jmin (grid.page, pageCount() - 1));
    };
    addAndMakeVisible (lengthBox);

    resLabel.setText ("Steps", dontSendNotification); styleLabel (resLabel, 11.0f); addAndMakeVisible (resLabel);
    resBox.addItem ("1/4", 1); resBox.addItem ("1/4 triplet", 2); resBox.addItem ("1/8", 3); resBox.addItem ("1/8 triplet", 4);
    resBox.addItem ("1/16", 5); resBox.addItem ("1/16 triplet", 6); resBox.addItem ("1/32", 7);
    resBox.onChange = [this]
    {
        const int v = resSpb2[jlimit (1, numRes, resBox.getSelectedId()) - 1];
        if (auto* l = currentLane()) l->stepsPer2Beats = v; else pattern().stepsPer2Beats = v;
        grid.repaint(); roll.repaint();
    };
    addAndMakeVisible (resBox);

    swingLabel.setText ("Swing", dontSendNotification); styleLabel (swingLabel, 11.0f); addAndMakeVisible (swingLabel);
    swingSlider.setSliderStyle (Slider::LinearHorizontal);
    swingSlider.setRange (50.0, 75.0, 1.0);
    swingSlider.setValue (proc.swing.load(), dontSendNotification);
    swingSlider.setTextBoxStyle (Slider::TextBoxRight, false, 44, 20);
    swingSlider.setTextValueSuffix ("%");
    swingSlider.setColour (Slider::thumbColourId, accent);
    swingSlider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    swingSlider.onValueChange = [this] { proc.swing = (float) swingSlider.getValue(); };
    addAndMakeVisible (swingSlider);

    for (int i = 0; i < 4; ++i)
    {
        pageButtons[i].setButtonText (String (i * 16 + 1) + "-" + String (i * 16 + 16));
        styleButton (pageButtons[i]);
        pageButtons[i].onClick = [this, i] { followToggle.setToggleState (false, dontSendNotification); setPage (i); };
        addAndMakeVisible (pageButtons[i]);
    }
    for (auto* t : { &followToggle, &allPadsToggle, &tempoPadToggle })
    {
        t->setColour (ToggleButton::textColourId, muted);
        t->setColour (ToggleButton::tickColourId, accent);
        addAndMakeVisible (t);
    }
    followToggle.setToggleState (true, dontSendNotification);
    allPadsToggle.onClick = [this] { padsChanged(); };
    tempoPadToggle.setTooltip ("Let the TempoPAD's mode buttons drive FlipKit64: User mode = step entry for the "
                               "selected pad, Easy mode = top row picks patterns. Turn MMC on for Play / Stop / Record.");
    tempoPadToggle.setToggleState (proc.tempoPadModes.load(), dontSendNotification);
    tempoPadToggle.onClick = [this] { ctx.tempoPadModesChanged (tempoPadToggle.getToggleState()); ctrlModeChanged(); };

    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    // ---- Keys tracks
    for (int i = 0; i <= Keys::numTracks; ++i)
    {
        auto& b = trackButtons[i];
        styleButton (b);
        b.setColour (TextButton::buttonOnColourId, (i == 0 ? accent : KeysUI::trackColour (i - 1)).withAlpha (0.85f));
        b.setColour (TextButton::textColourOnId, Colour (0xff120c1c));
        b.setTooltip (i == 0 ? String ("The drum pads' step grid") : "A melodic track: one pad's sound played across the keyboard");
        b.onClick = [this, i] { selectTrack (i); };
        addAndMakeVisible (b);
    }
    soundLabel.setText ("Sound", dontSendNotification); styleLabel (soundLabel, 11.0f); addChildComponent (soundLabel);
    soundBox.setTooltip ("The pad this track plays, pitched across the keyboard (it keeps that pad's tuning, envelope and filter)");
    soundBox.onChange = [this]
    {
        if (selectedTrack <= 0 || soundBox.getSelectedId() == sliceItemId) return;
        proc.keysTracks[selectedTrack - 1].sliceCount = 0;
        proc.keysTracks[selectedTrack - 1].sourcePad = soundBox.getSelectedId() - 2;
        grid.repaint();
    };
    addChildComponent (soundBox);
    rootLabel.setText ("Root", dontSendNotification); styleLabel (rootLabel, 11.0f); addChildComponent (rootLabel);
    for (int n = 24; n <= 96; ++n) rootBox.addItem (KeysUI::noteName (n), n);
    rootBox.setTooltip ("The note the sample plays at its own pitch (e.g. C3 for \"Rhodes C3.wav\")");
    rootBox.onChange = [this] { if (selectedTrack > 0) proc.keysTracks[selectedTrack - 1].root = rootBox.getSelectedId(); };
    addChildComponent (rootBox);
    monoToggle.setTooltip ("One note at a time (bass lines, leads)");
    monoToggle.onClick = [this]
    {
        if (selectedTrack > 0) proc.keysTracks[selectedTrack - 1].mono = monoToggle.getToggleState();
        syncTrackControls();
    };
    glideSlider.setSliderStyle (Slider::LinearHorizontal);
    glideSlider.setRange (0.0, 300.0, 5.0);
    glideSlider.setTextBoxStyle (Slider::TextBoxRight, false, 64, 20);
    glideSlider.textFromValueFunction = [] (double v) { return v < 1.0 ? String ("No glide") : "Glide " + String ((int) v) + " ms"; };
    glideSlider.setColour (Slider::thumbColourId, accent);
    glideSlider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    glideSlider.setColour (Slider::textBoxTextColourId, text);
    glideSlider.setTooltip ("Mono only: slide from one note to the next");
    glideSlider.onValueChange = [this] { if (selectedTrack > 0) proc.keysTracks[selectedTrack - 1].glideMs = (float) glideSlider.getValue(); };
    addChildComponent (glideSlider);
    for (int r = 0; r < 12; ++r) scaleRootBox.addItem (MidiMessage::getMidiNoteName (r, true, false, 3), r + 1);
    for (int s = 0; s < Keys::numScales; ++s) scaleTypeBox.addItem (Keys::scaleName (s), s + 1);
    scaleRootBox.setTooltip ("Key: highlights its notes in the piano roll");
    scaleTypeBox.setTooltip ("Scale: highlights its notes; Snap keeps new notes in it");
    scaleRootBox.onChange = [this] { proc.scaleRoot = scaleRootBox.getSelectedId() - 1; roll.repaint(); };
    scaleTypeBox.onChange = [this] { proc.scaleType = scaleTypeBox.getSelectedId() - 1; roll.repaint(); };
    addChildComponent (scaleRootBox);
    addChildComponent (scaleTypeBox);
    for (auto* t : { &monoToggle, &snapToggle, &ghostToggle, &padsPlayToggle })
    {
        t->setColour (ToggleButton::textColourId, muted);
        t->setColour (ToggleButton::tickColourId, accent);
        addChildComponent (t);
    }
    snapToggle.setToggleState (true, dontSendNotification);
    ghostToggle.setToggleState (true, dontSendNotification);
    padsPlayToggle.setToggleState (true, dontSendNotification);
    padsPlayToggle.setTooltip ("While this track is shown, the TempoPAD's pads play it (Key mode gives you a keyboard layout). Arm Rec to record.");
    snapToggle.onClick = [this] { roll.snap = snapToggle.getToggleState(); };
    ghostToggle.onClick = [this] { roll.ghosts = ghostToggle.getToggleState(); roll.repaint(); };
    rollViewport.setViewedComponent (&roll, false);
    rollViewport.setScrollBarsShown (true, false);
    addChildComponent (rollViewport);
    addChildComponent (velLane);
    addChildComponent (notePanel);
    roll.onChange = [this] { notePanel.refresh(); velLane.repaint(); };

    defaultHint = String ("Click an empty step to add it (Shift = accent), click a lit step to select it, click it again to remove it. Drag up/down for velocity. "
                  "Right-click for velocity and ratchets (rolls). Arm Rec and play the TempoPAD while it loops to record. "
                  "Plays with LUNA's transport; Play runs it on its own.");
    hint.setText (defaultHint, dontSendNotification);
    styleLabel (hint, 11.0f);
    hint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hint);

    syncPatternControls();
    padsChanged();
    selectTrack (0);
}

SequencerPage::~SequencerPage() { proc.keysInput = -1; }

KeysLane* SequencerPage::currentLane()
{
    return selectedTrack > 0 ? &pattern().lanes[jlimit (0, Keys::numTracks - 1, selectedTrack - 1)] : nullptr;
}

int SequencerPage::pageCount()
{
    const int len = currentLane() != nullptr ? currentLane()->length.load() : pattern().length.load();
    return jlimit (1, 4, len / 16);
}

void SequencerPage::rebuildSoundBox()
{
    const int keep = soundBox.getSelectedId();
    soundBox.clear (dontSendNotification);
    soundBox.addItem ("Pick a sound...", 1);
    for (int pad : padsByNote())
    {
        auto ps = proc.getPad (pad);
        if (ps->layers.empty()) continue;
        soundBox.addItem (String (PadLayout::noteForPad (pad)) + "  " + (ps->name.isEmpty() ? String ("Pad") : ps->name), pad + 2);
    }
    if (selectedTrack > 0)
    {
        const auto& kt = proc.keysTracks[selectedTrack - 1];
        const int src = kt.sourcePad.load();
        if (const int n = kt.sliceCount.load(); n > 0 && src >= 0)  // slices sent from the Sampler: one pad per key
        {
            const int first = PadLayout::noteForPad (src);
            soundBox.addItem ("Slices: pads " + String (first) + "-" + String (first + n - 1), sliceItemId);
            soundBox.setSelectedId (sliceItemId, dontSendNotification);
            return;
        }
        if (src >= 0 && soundBox.indexOfItemId (src + 2) < 0)      // a pad that's been emptied: still show it
            soundBox.addItem (String (PadLayout::noteForPad (src)) + "  (empty)", src + 2);
        soundBox.setSelectedId (src + 2, dontSendNotification);
    }
    else if (keep > 0) soundBox.setSelectedId (keep, dontSendNotification);
}

void SequencerPage::syncTrackControls()
{
    for (int i = 0; i <= Keys::numTracks; ++i)
    {
        trackButtons[i].setButtonText (i == 0 ? String ("Drums") : proc.keysTracks[i - 1].name);
        trackButtons[i].setToggleState (i == selectedTrack, dontSendNotification);
    }
    const bool keys = selectedTrack > 0;
    for (Component* c : std::initializer_list<Component*> { &soundLabel, &soundBox, &rootLabel, &rootBox, &monoToggle })
        c->setVisible (keys);
    if (! keys) { glideSlider.setVisible (false); return; }
    const auto& kt = proc.keysTracks[selectedTrack - 1];
    rebuildSoundBox();
    rootBox.setSelectedId (kt.root.load(), dontSendNotification);
    monoToggle.setToggleState (kt.mono.load(), dontSendNotification);
    glideSlider.setValue (kt.glideMs.load(), dontSendNotification);
    glideSlider.updateText();
    glideSlider.setVisible (kt.mono.load());
    scaleRootBox.setSelectedId (proc.scaleRoot.load() + 1, dontSendNotification);
    scaleTypeBox.setSelectedId (proc.scaleType.load() + 1, dontSendNotification);
}

void SequencerPage::selectTrack (int t)
{
    selectedTrack = jlimit (0, Keys::numTracks, t);
    const bool keys = selectedTrack > 0;
    viewport.setVisible (! keys);
    stepPanel.setVisible (! keys);
    allPadsToggle.setVisible (! keys);
    tempoPadToggle.setVisible (! keys);
    followToggle.setVisible (! keys);
    for (Component* c : std::initializer_list<Component*> { &rollViewport, &velLane, &notePanel, &scaleRootBox, &scaleTypeBox, &snapToggle, &ghostToggle, &padsPlayToggle })
        c->setVisible (keys);
    if (keys)
    {
        roll.track = selectedTrack - 1;
        roll.selPitch = roll.selStep = -1;
        roll.lastLen = proc.keysTracks[roll.track].mono.load() ? 2 : 4;
        // scroll to the notes, or around the root if the track is empty
        int top = proc.keysTracks[roll.track].root.load() + 14;
        if (auto notes = currentLane()->get()) { top = 0; for (const auto& n : *notes) top = jmax (top, (int) n.pitch + 4); }
        rollViewport.setViewPosition (0, jlimit (0, roll.preferredHeight(), roll.yForPitch (jmin (Keys::highNote, top))));
    }
    syncTrackControls();
    syncPatternControls();
    updateHint();
    resized();
    notePanel.refresh();
}

void SequencerPage::updateHint()
{
    if (selectedTrack > 0)
        hint.setText ("Click the grid to add a note (drag right for a longer one), click a note to select it, drag to move it, drag its right edge to resize, "
                      "click it again to remove it. With \"Pads play this track\" on, the TempoPAD plays it (Key mode = keyboard layout); arm Rec to record.",
                      dontSendNotification);
    else
        ctrlModeChanged();
}

PatternData& SequencerPage::pattern() { return proc.patterns[jlimit (0, Seq::numPatterns - 1, proc.currentPattern.load())]; }

void SequencerPage::syncPatternControls()
{
    auto& pat = pattern();
    auto* ln = currentLane();
    lengthBox.setSelectedId (ln != nullptr ? ln->length.load() : pat.length.load(), dontSendNotification);
    const int s2 = ln != nullptr ? ln->stepsPer2Beats.load() : pat.stepsPer2Beats.load();
    int resId = 5;
    for (int i = 0; i < numRes; ++i) if (resSpb2[i] == s2) resId = i + 1;
    resBox.setSelectedId (resId, dontSendNotification);
    swingSlider.setValue (proc.swing.load(), dontSendNotification);
    bpmSlider.setValue (proc.internalBpm.load(), dontSendNotification);
    recButton.setToggleState (proc.seqRecord.load(), dontSendNotification);
    setPage (jmin (grid.page, pageCount() - 1));
    padsChanged();
    roll.repaint(); velLane.repaint(); notePanel.refresh();
}

void SequencerPage::setPage (int p)
{
    grid.page = roll.page = jlimit (0, 3, p);
    const int pages = pageCount();
    for (int i = 0; i < 4; ++i)
    {
        pageButtons[i].setVisible (pages > 1 && i < pages);
        pageButtons[i].setToggleState (i == grid.page, dontSendNotification);
    }
    grid.repaint(); roll.repaint(); velLane.repaint();
}

void SequencerPage::padsChanged()
{
    auto& pat = pattern();
    grid.rows.clear();
    for (int pad : padsByNote())
        if (allPadsToggle.getToggleState() || ! proc.getPad (pad)->layers.empty() || pat.padHasSteps (pad))
            grid.rows.push_back (pad);
    grid.setSize (jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()),
                  (int) grid.rows.size() * StepGrid::rowH + StepGrid::laneHeaderH + Keys::numTracks * StepGrid::laneRowH);
    grid.repaint();
    roll.setSize (jmax (100, rollViewport.getWidth() - rollViewport.getScrollBarThickness()), roll.preferredHeight());
    if (selectedTrack > 0) rebuildSoundBox();
}

void SequencerPage::ctrlModeChanged()
{
    String h = defaultHint;
    if (proc.tempoPadModes.load())
    {
        const int m = proc.ctrlMode.load();
        if (m == PadRomplerProcessor::ctrlSteps)
        {
            if (selectedTrack != 0) { selectTrack (0); return; }
            auto ps = proc.getPad (ctx.getSelectedPad());
            h = "TempoPAD User mode: each pad turns a step on or off for " + (ps->name.isEmpty() ? String ("the selected pad") : ps->name)
              + " (top-left pad = step 1, reading across like a page). Switch to Drum mode, hit another pad, and come back to program that one.";
        }
        else if (m == PadRomplerProcessor::ctrlPatterns)
            h = "TempoPAD Easy mode: the top row of pads picks patterns 1-8 (it changes at the end of the bar while playing). "
                "The other pads play as normal. Switch to Drum mode to go back.";
    }
    if (selectedTrack == 0) hint.setText (h, dontSendNotification);
    grid.repaint();
}

void SequencerPage::setStatus (const String& s)
{
    statusLabel.setText (s, dontSendNotification);
    statusAt = Time::getMillisecondCounter();
}

void SequencerPage::undo()
{
    if (history.undo()) { setStatus ("Undone."); syncPatternControls(); stepChanged(); }
}

void SequencerPage::redo()
{
    if (history.redo()) { setStatus ("Redone."); syncPatternControls(); stepChanged(); }
}

void SequencerPage::stepChanged()
{
    stepPanel.refresh();
    grid.repaint();
}

void SequencerPage::tick()
{
    proc.keysInput = (isShowing() && selectedTrack > 0 && padsPlayToggle.getToggleState()) ? selectedTrack - 1 : -1;
    proc.drainKeyRecordings();
    if (proc.keysRecorded.load() != lastKeysRecorded) { lastKeysRecorded = proc.keysRecorded.load(); roll.repaint(); velLane.repaint(); grid.repaint(); }
    if (selectedTrack > 0)
    {
        const int ks = proc.keysStep[selectedTrack - 1].load();
        if (ks != lastKeysStep)
        {
            lastKeysStep = ks;
            if (ks >= 0 && ks / 16 != roll.page) setPage (ks / 16);    // follow the playhead
            roll.repaint();
        }
    }
    undoButton.setEnabled (history.canUndo());
    redoButton.setEnabled (history.canRedo());
    fillButton.setToggleState (proc.fillOn.load(), dontSendNotification);
    if (statusAt != 0 && Time::getMillisecondCounter() - statusAt > 5000) { statusLabel.setText ({}, dontSendNotification); statusAt = 0; }
    if (++panelTicks % 6 == 0) stepPanel.refresh();
    tempoPadToggle.setToggleState (proc.tempoPadModes.load(), dontSendNotification);
    recButton.setToggleState (proc.seqRecord.load(), dontSendNotification);
    const auto edits = proc.stepEdits.load();
    if (edits != lastStepEdits)
    {
        lastStepEdits = edits;
        const int st = proc.lastStepEdit.load();
        padsChanged();
        if (st >= 0 && st / 16 != grid.page && ! (followToggle.getToggleState() && proc.playStep.load() >= 0)) setPage (st / 16);
        if (proc.ctrlMode.load() == PadRomplerProcessor::ctrlSteps) ctrlModeChanged();
    }
    const bool host = proc.hostPlaying.load();
    const bool internal = proc.internalPlaying.load();
    playButton.setButtonText (internal ? "Stop" : "Play");
    playButton.setToggleState (internal, dontSendNotification);
    transportInfo.setText (host ? "Following LUNA  (" + String (proc.hostBpm.load(), 1) + " BPM)"
                                : internal ? "Running on its own" : "Stopped - press Play in LUNA or here",
                           dontSendNotification);

    const int cur = proc.currentPattern.load(), queued = proc.queuedPattern.load();
    ++blink;
    for (int i = 0; i < Seq::numPatterns; ++i)
    {
        const bool on = (i == cur) || (i == queued && (blink / 8) % 2 == 0);
        patButtons[i].setToggleState (on, dontSendNotification);
    }
    if (cur != lastPattern) { lastPattern = cur; syncPatternControls(); roll.selPitch = roll.selStep = -1; notePanel.refresh(); }

    const int step = proc.playStep.load();
    if (step != lastStep)
    {
        lastStep = step;
        if (step >= 0 && selectedTrack == 0 && followToggle.getToggleState() && step / 16 != grid.page) setPage (step / 16);
        grid.repaint();
    }
    if (proc.seqRecord.load() && (host || internal)) grid.repaint();   // show live-recorded hits as they land
}

void SequencerPage::paint (Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (barArea.toFloat(), 8.0f);
}

void SequencerPage::resized()
{
    auto area = getLocalBounds();
    barArea = area.removeFromTop (168);
    auto b = barArea.reduced (12, 10);

    auto row1 = b.removeFromTop (30);
    playButton.setBounds (row1.removeFromLeft (70)); row1.removeFromLeft (6);
    recButton.setBounds (row1.removeFromLeft (56)); row1.removeFromLeft (14);
    bpmLabel.setBounds (row1.removeFromLeft (32));
    bpmSlider.setBounds (row1.removeFromLeft (110)); row1.removeFromLeft (10);
    transportInfo.setBounds (row1.removeFromLeft (230)); row1.removeFromLeft (10);
    dupButton.setBounds (row1.removeFromRight (110)); row1.removeFromRight (6);
    clearButton.setBounds (row1.removeFromRight (64));

    b.removeFromTop (10);
    auto row2 = b.removeFromTop (28);
    patLabel.setBounds (row2.removeFromLeft (52));
    for (auto& pb : patButtons) { pb.setBounds (row2.removeFromLeft (32)); row2.removeFromLeft (3); }
    row2.removeFromLeft (14);
    lengthLabel.setBounds (row2.removeFromLeft (46));
    lengthBox.setBounds (row2.removeFromLeft (100)); row2.removeFromLeft (10);
    resLabel.setBounds (row2.removeFromLeft (40));
    resBox.setBounds (row2.removeFromLeft (110)); row2.removeFromLeft (10);
    swingLabel.setBounds (row2.removeFromLeft (44));
    swingSlider.setBounds (row2.removeFromLeft (190));

    b.removeFromTop (10);
    auto row3 = b.removeFromTop (28);
    captureButton.setBounds (row3.removeFromLeft (84)); row3.removeFromLeft (6);
    fillButton.setBounds (row3.removeFromLeft (56)); row3.removeFromLeft (6);
    rollBox.setBounds (row3.removeFromLeft (110)); row3.removeFromLeft (14);
    undoButton.setBounds (row3.removeFromLeft (60)); row3.removeFromLeft (6);
    redoButton.setBounds (row3.removeFromLeft (60)); row3.removeFromLeft (14);
    statusLabel.setBounds (row3);

    b.removeFromTop (10);
    auto row4 = b.removeFromTop (28);
    for (int i = 0; i <= Keys::numTracks; ++i) { trackButtons[i].setBounds (row4.removeFromLeft (i == 0 ? 72 : 84)); row4.removeFromLeft (4); }
    row4.removeFromLeft (12);
    soundLabel.setBounds (row4.removeFromLeft (40));
    soundBox.setBounds (row4.removeFromLeft (170)); row4.removeFromLeft (10);
    rootLabel.setBounds (row4.removeFromLeft (32));
    rootBox.setBounds (row4.removeFromLeft (66)); row4.removeFromLeft (10);
    monoToggle.setBounds (row4.removeFromLeft (62));
    glideSlider.setBounds (row4.removeFromLeft (150));

    area.removeFromTop (8);
    auto pagesRow = area.removeFromTop (26);
    {
        auto pr = pagesRow;
        tempoPadToggle.setBounds (pr.removeFromRight (140));
        allPadsToggle.setBounds (pr.removeFromRight (130));
        followToggle.setBounds (pr.removeFromRight (140));
        pr = pagesRow;
        padsPlayToggle.setBounds (pr.removeFromRight (162));
        ghostToggle.setBounds (pr.removeFromRight (140));
        snapToggle.setBounds (pr.removeFromRight (112));
        scaleTypeBox.setBounds (pr.removeFromRight (136).reduced (0, 1)); pr.removeFromRight (4);
        scaleRootBox.setBounds (pr.removeFromRight (58).reduced (0, 1));
    }
    pagesRow.removeFromLeft (selectedTrack > 0 ? PianoRoll::keyW : StepGrid::nameW);
    for (auto& pb : pageButtons) { pb.setBounds (pagesRow.removeFromLeft (70)); pagesRow.removeFromLeft (4); }
    area.removeFromTop (6);
    hint.setBounds (area.removeFromBottom (34));
    area.removeFromBottom (4);
    auto side = area.removeFromRight (210);
    stepPanel.setBounds (side);
    notePanel.setBounds (side);
    area.removeFromRight (10);
    viewport.setBounds (area);
    auto keysArea = area;
    velLane.setBounds (keysArea.removeFromBottom (58));
    keysArea.removeFromBottom (6);
    rollViewport.setBounds (keysArea);
    padsChanged();
}
