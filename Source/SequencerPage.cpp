#include "SequencerPage.h"

using namespace juce;
using namespace UI;

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
    const int spb = jmax (1, pat.stepsPerBeat.load());
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
                const float a = 0.35f + 0.65f * vel / 127.0f;
                g.setColour (hue.withAlpha (a));
                g.fillRoundedRectangle (cell.reduced (2.0f), 3.0f);
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
        }
    }
}

void StepGrid::mouseDown (const MouseEvent& e)
{
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
    if (pat.vel[pad][step].load() == 0)
    {
        pat.vel[pad][step].store (e.mods.isShiftDown() ? 127 : 100);
        pat.ratchet[pad][step].store (1);
        turnedOn = true;
        ctx.getProc().triggerPad (pad, e.mods.isShiftDown() ? 1.0f : 100.0f / 127.0f);
    }
    else turnedOn = false;
    dragStartVel = pat.vel[pad][step].load();
    repaint();
}

void StepGrid::mouseDrag (const MouseEvent& e)
{
    if (dragPad < 0) return;
    const int dy = dragStartY - e.y;
    if (! moved && std::abs (dy) < 4) return;
    moved = true;
    owner.pattern().vel[dragPad][dragStep].store ((uint8_t) jlimit (1, 127, dragStartVel + dy));
    repaint();
}

void StepGrid::mouseUp (const MouseEvent&)
{
    if (dragPad >= 0 && ! turnedOn && ! moved)
        owner.pattern().vel[dragPad][dragStep].store (0);
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
    m.addSeparator();
    m.addItem (2000, "Clear step", vel > 0);
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                     [this, pad, step] (int res)
                     {
                         auto& p = owner.pattern();
                         if (res <= 0) return;
                         if (res <= 127) p.vel[pad][step].store ((uint8_t) res);
                         else if (res > 1000 && res <= 1004)
                         {
                             if (p.vel[pad][step].load() == 0) p.vel[pad][step].store (100);
                             p.ratchet[pad][step].store ((uint8_t) (res - 1000));
                         }
                         else if (res == 2000) p.vel[pad][step].store (0);
                         repaint();
                     });
}

// ===================================================================================== SequencerPage

static const int resSpb[] = { 2, 3, 4, 6, 8 };

SequencerPage::SequencerPage (EditorContext& c) : ctx (c), proc (c.getProc())
{
    for (auto* b : { &playButton, &recButton, &clearButton, &dupButton }) { styleButton (*b); addAndMakeVisible (b); }
    playButton.onClick = [this] { proc.internalPlaying = ! proc.internalPlaying.load(); };
    recButton.setClickingTogglesState (true);
    recButton.setColour (TextButton::buttonOnColourId, rec);
    recButton.setTooltip ("Live record: hits you play on the TempoPAD go into the pattern, snapped to the nearest step");
    recButton.onClick = [this] { proc.seqRecord = recButton.getToggleState(); };
    clearButton.onClick = [this] { pattern().clear(); grid.repaint(); padsChanged(); };
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
    lengthBox.onChange = [this] { pattern().length = lengthBox.getSelectedId(); setPage (jmin (grid.page, pattern().length.load() / 16 - 1)); };
    addAndMakeVisible (lengthBox);

    resLabel.setText ("Steps", dontSendNotification); styleLabel (resLabel, 11.0f); addAndMakeVisible (resLabel);
    resBox.addItem ("1/8", 1); resBox.addItem ("1/8 triplet", 2); resBox.addItem ("1/16", 3); resBox.addItem ("1/16 triplet", 4); resBox.addItem ("1/32", 5);
    resBox.onChange = [this] { pattern().stepsPerBeat = resSpb[jlimit (1, 5, resBox.getSelectedId()) - 1]; grid.repaint(); };
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

    defaultHint = String ("Click a step to turn it on (Shift = accent). Drag up/down on a step for velocity. "
                  "Right-click for velocity and ratchets (rolls). Arm Rec and play the TempoPAD while it loops to record. "
                  "Plays with LUNA's transport; Play runs it on its own.");
    hint.setText (defaultHint, dontSendNotification);
    styleLabel (hint, 11.0f);
    hint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hint);

    syncPatternControls();
    padsChanged();
}

PatternData& SequencerPage::pattern() { return proc.patterns[jlimit (0, Seq::numPatterns - 1, proc.currentPattern.load())]; }

void SequencerPage::syncPatternControls()
{
    auto& pat = pattern();
    lengthBox.setSelectedId (pat.length.load(), dontSendNotification);
    int resId = 3;
    for (int i = 0; i < 5; ++i) if (resSpb[i] == pat.stepsPerBeat.load()) resId = i + 1;
    resBox.setSelectedId (resId, dontSendNotification);
    swingSlider.setValue (proc.swing.load(), dontSendNotification);
    bpmSlider.setValue (proc.internalBpm.load(), dontSendNotification);
    recButton.setToggleState (proc.seqRecord.load(), dontSendNotification);
    setPage (jmin (grid.page, jmax (0, pat.length.load() / 16 - 1)));
    padsChanged();
}

void SequencerPage::setPage (int p)
{
    grid.page = jlimit (0, 3, p);
    const int pages = jmax (1, pattern().length.load() / 16);
    for (int i = 0; i < 4; ++i)
    {
        pageButtons[i].setVisible (pages > 1 && i < pages);
        pageButtons[i].setToggleState (i == grid.page, dontSendNotification);
    }
    grid.repaint();
}

void SequencerPage::padsChanged()
{
    auto& pat = pattern();
    grid.rows.clear();
    for (int pad : padsByNote())
        if (allPadsToggle.getToggleState() || ! proc.getPad (pad)->layers.empty() || pat.padHasSteps (pad))
            grid.rows.push_back (pad);
    grid.setSize (jmax (100, viewport.getWidth() - viewport.getScrollBarThickness()), jmax (1, (int) grid.rows.size() * StepGrid::rowH));
    grid.repaint();
}

void SequencerPage::ctrlModeChanged()
{
    String h = defaultHint;
    if (proc.tempoPadModes.load())
    {
        const int m = proc.ctrlMode.load();
        if (m == PadRomplerProcessor::ctrlSteps)
        {
            auto ps = proc.getPad (ctx.getSelectedPad());
            h = "TempoPAD User mode: each pad turns a step on or off for " + (ps->name.isEmpty() ? String ("the selected pad") : ps->name)
              + " (top-left pad = step 1, reading across like a page). Switch to Drum mode, hit another pad, and come back to program that one.";
        }
        else if (m == PadRomplerProcessor::ctrlPatterns)
            h = "TempoPAD Easy mode: the top row of pads picks patterns 1-8 (it changes at the end of the bar while playing). "
                "The other pads play as normal. Switch to Drum mode to go back.";
    }
    hint.setText (h, dontSendNotification);
    grid.repaint();
}

void SequencerPage::tick()
{
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
    if (cur != lastPattern) { lastPattern = cur; syncPatternControls(); }

    const int step = proc.playStep.load();
    if (step != lastStep)
    {
        lastStep = step;
        if (step >= 0 && followToggle.getToggleState() && step / 16 != grid.page) setPage (step / 16);
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
    barArea = area.removeFromTop (92);
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

    area.removeFromTop (8);
    auto pagesRow = area.removeFromTop (26);
    tempoPadToggle.setBounds (pagesRow.removeFromRight (140));
    allPadsToggle.setBounds (pagesRow.removeFromRight (130));
    followToggle.setBounds (pagesRow.removeFromRight (140));
    pagesRow.removeFromLeft (StepGrid::nameW);
    for (auto& pb : pageButtons) { pb.setBounds (pagesRow.removeFromLeft (70)); pagesRow.removeFromLeft (4); }
    area.removeFromTop (6);
    hint.setBounds (area.removeFromBottom (34));
    area.removeFromBottom (4);
    viewport.setBounds (area);
    padsChanged();
}
