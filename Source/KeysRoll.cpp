#include "KeysRoll.h"
#include "SequencerPage.h"

using namespace juce;
using namespace UI;

Colour KeysUI::trackColour (int t)
{
    static const Colour c[] = { zoneCyan, zoneYellow, zonePurple, zoneBlue };
    return c[jlimit (0, 3, t)];
}

String KeysUI::noteName (int pitch) { return MidiMessage::getMidiNoteName (pitch, true, true, 3); }

// ===================================================================================== PianoRoll

PianoRoll::PianoRoll (SequencerPage& o) : owner (o) {}

KeysLane& PianoRoll::lane() const { return owner.pattern().lanes[jlimit (0, Keys::numTracks - 1, track)]; }

int PianoRoll::stepAt (int x) const
{
    return page * 16 + jlimit (0, 15, (int) std::floor ((x - keyW) / jmax (1.0f, cellW())));
}

int PianoRoll::findNote (const Keys::NoteList& list, int pitch, int step) const
{
    for (int i = (int) list.size() - 1; i >= 0; --i)
    {
        const auto& n = list[(size_t) i];
        if (n.pitch == pitch && step >= n.step && step < n.step + n.len) return i;
    }
    return -1;
}

bool PianoRoll::selectedNote (Keys::Note& out) const
{
    auto notes = lane().get();
    if (notes == nullptr) return false;
    for (const auto& n : *notes)
        if (n.pitch == selPitch && n.step == selStep) { out = n; return true; }
    return false;
}

void PianoRoll::setNotes (Keys::NoteList list)
{
    lane().set (owner.getProc().makeNotes (std::move (list)));
    repaint();
    if (onChange) onChange();
}

void PianoRoll::replaceSelected (std::function<void (Keys::Note&)> change)
{
    auto notes = lane().get();
    if (notes == nullptr) return;
    Keys::NoteList list = *notes;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].pitch != selPitch || list[i].step != selStep) continue;
        auto n = list[i];
        change (n);
        list.erase (list.begin() + (long) i);
        // a note already sitting where this one lands is replaced
        list.erase (std::remove_if (list.begin(), list.end(), [&] (const Keys::Note& m) { return m.pitch == n.pitch && m.step == n.step; }), list.end());
        list.push_back (n);
        selPitch = n.pitch; selStep = n.step;
        setNotes (std::move (list));
        return;
    }
}

void PianoRoll::removeSelected()
{
    auto notes = lane().get();
    if (notes == nullptr) return;
    Keys::NoteList list = *notes;
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Keys::Note& n) { return n.pitch == selPitch && n.step == selStep; }), list.end());
    selPitch = selStep = -1;
    setNotes (std::move (list));
}

int PianoRoll::snapPitch (int p) const
{
    auto& proc = owner.getProc();
    if (! snap) return p;
    for (int d = 0; d < 12; ++d)
        if (Keys::inScale (proc.scaleType.load(), proc.scaleRoot.load(), p - d)) return jmax (Keys::lowNote, p - d);
    return p;
}

void PianoRoll::paint (Graphics& g)
{
    auto& proc = owner.getProc();
    auto& ln = lane();
    const int len = jlimit (1, Seq::maxSteps, ln.length.load());
    const int s2 = jmax (1, ln.stepsPer2Beats.load());
    const int spb = (s2 % 2 == 0) ? s2 / 2 : s2;
    const int scale = proc.scaleType.load(), root = proc.scaleRoot.load();
    const float cw = cellW();
    const auto clip = g.getClipBounds();
    const int w = getWidth();

    // rows
    const int pTop = jmin (Keys::highNote, Keys::highNote - clip.getY() / rowH + 1);
    const int pBottom = jmax (Keys::lowNote, Keys::highNote - clip.getBottom() / rowH - 1);
    for (int p = pTop; p >= pBottom; --p)
    {
        const int y = yForPitch (p);
        const bool in = Keys::inScale (scale, root, p);
        const bool isRoot = ((p - root) % 12 + 12) % 12 == 0;
        g.setColour (in ? (isRoot && scale != 0 ? Colour (0xff30264b) : Colour (0xff2a2042)) : Colour (0xff1b1430));
        g.fillRect (keyW, y, w - keyW, rowH);
        g.setColour (Colour (0xff1a1329));
        g.fillRect (keyW, y + rowH - 1, w - keyW, 1);

        const bool black = KeysUI::isBlack (p);
        g.setColour (black ? Colour (0xff120c1c) : Colour (0xff3a2f55));
        g.fillRect (0, y, keyW - 2, rowH - 1);
        if (p % 12 == 0)
        {
            g.setColour (text);
            g.setFont (Font (10.0f, Font::bold));
            g.drawText (KeysUI::noteName (p), 6, y, keyW - 10, rowH - 1, Justification::centredLeft);
        }
    }

    // step columns, beat lines, steps past the lane's length
    for (int c = 0; c < 16; ++c)
    {
        const int step = page * 16 + c;
        const float x = keyW + c * cw;
        if (step >= len) { g.setColour (bg.withAlpha (0.75f)); g.fillRect (x, (float) clip.getY(), cw, (float) clip.getHeight()); }
        g.setColour (step % spb == 0 ? line.brighter (0.15f) : Colour (0xff241b3a));
        g.fillRect (x, (float) clip.getY(), step % spb == 0 ? 1.5f : 1.0f, (float) clip.getHeight());
    }

    // the other Keys tracks, faintly (they may use other step sizes: placed by time)
    const double sl = ln.stepLength();
    if (ghosts)
        for (int u = 0; u < Keys::numTracks; ++u)
        {
            if (u == track) continue;
            const auto& other = owner.pattern().lanes[u];
            auto on = other.get();
            if (on == nullptr) continue;
            const double osl = other.stepLength();
            g.setColour (KeysUI::trackColour (u).withAlpha (0.22f));
            for (const auto& n : *on)
            {
                const double s = n.step * osl / sl, e = (n.step + n.len) * osl / sl;
                const float x0 = xForStep (s), x1 = xForStep (e);
                if (x1 < keyW || x0 > w) continue;
                g.fillRoundedRectangle (jmax ((float) keyW, x0) + 1.0f, (float) yForPitch (n.pitch) + 1.0f,
                                        jmin ((float) w, x1) - jmax ((float) keyW, x0) - 2.0f, rowH - 2.0f, 2.5f);
            }
        }

    // this track's notes
    const Colour col = KeysUI::trackColour (track);
    if (auto notes = ln.get())
        for (const auto& n : *notes)
        {
            if (n.step >= len) continue;
            const float x0 = xForStep (n.step), x1 = xForStep (jmin (n.step + n.len, len));
            if (x1 < keyW || x0 > w) continue;
            auto r = Rectangle<float> (jmax ((float) keyW, x0) + 1.0f, (float) yForPitch (n.pitch) + 1.0f,
                                       jmin ((float) w, x1) - jmax ((float) keyW, x0) - 2.0f, rowH - 2.0f);
            g.setColour (col.withAlpha (0.55f + 0.45f * n.vel / 127.0f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (Colours::black.withAlpha (0.25f));
            g.fillRect (r.getRight() - 4.0f, r.getY(), 4.0f, r.getHeight());       // the resize edge
            if (r.getWidth() > 28.0f)
            {
                g.setColour (Colour (0xff120c1c));
                g.setFont (Font (9.5f, Font::bold));
                g.drawText (KeysUI::noteName (n.pitch), r.reduced (5.0f, 0.0f), Justification::centredLeft);
            }
            if (n.pitch == selPitch && n.step == selStep) { g.setColour (text); g.drawRoundedRectangle (r, 3.0f, 2.0f); }
        }

    // playhead
    const int ps = proc.keysStep[jlimit (0, Keys::numTracks - 1, track)].load();
    if (ps >= 0 && ps / 16 == page)
    {
        const float x = keyW + (ps % 16) * cw;
        g.setColour (accent.withAlpha (0.10f));
        g.fillRect (x, (float) clip.getY(), cw, (float) clip.getHeight());
        g.setColour (accent);
        g.fillRect (x, (float) clip.getY(), 2.0f, (float) clip.getHeight());
    }
}

void PianoRoll::mouseMove (const MouseEvent& e)
{
    MouseCursor cur = MouseCursor::NormalCursor;
    if (e.x > keyW)
        if (auto notes = lane().get())
        {
            const int i = findNote (*notes, pitchAt (e.y), stepAt (e.x));
            if (i >= 0)
            {
                const auto& n = (*notes)[(size_t) i];
                cur = e.x > xForStep (n.step + n.len) - 7.0f ? MouseCursor::LeftRightResizeCursor : MouseCursor::DraggingHandCursor;
            }
        }
    setMouseCursor (cur);
}

void PianoRoll::mouseDown (const MouseEvent& e)
{
    auto& proc = owner.getProc();
    const int pitch = pitchAt (e.y);
    if (e.x < keyW) { proc.triggerKey (track, pitch, 0.85f); return; }
    const int step = stepAt (e.x);
    const int len = jlimit (1, Seq::maxSteps, lane().length.load());
    auto notes = lane().get();
    Keys::NoteList list = notes != nullptr ? *notes : Keys::NoteList();
    const int i = findNote (list, pitch, step);
    moved = false;
    downStep = step; downPitch = pitch;

    if (i >= 0)
    {
        const auto n = list[(size_t) i];
        if (e.mods.isPopupMenu()) { selPitch = n.pitch; selStep = n.step; removeSelected(); drag = none; return; }
        wasSelected = (n.pitch == selPitch && n.step == selStep);
        selPitch = n.pitch; selStep = n.step;
        origStep = n.step; origPitch = n.pitch;
        drag = e.x > xForStep (n.step + n.len) - 7.0f ? resizing : moving;
        proc.triggerKey (track, n.pitch, n.vel / 127.0f);
        repaint();
        if (onChange) onChange();
        return;
    }
    if (e.mods.isPopupMenu() || step >= len) { drag = none; return; }
    const int p = snapPitch (pitch);
    const int nl = jlimit (1, len - step, lastLen);
    list.erase (std::remove_if (list.begin(), list.end(), [&] (const Keys::Note& m) { return m.pitch == p && m.step == step; }), list.end());
    list.push_back ({ (uint8_t) p, (uint8_t) step, (uint8_t) nl, (uint8_t) 100 });
    selPitch = p; selStep = step;
    origStep = step; origPitch = p;
    wasSelected = false;
    drag = resizing;                                   // keep dragging right to make it longer
    proc.triggerKey (track, p, 100.0f / 127.0f);
    setNotes (std::move (list));
}

void PianoRoll::mouseDrag (const MouseEvent& e)
{
    if (drag == none) return;
    Keys::Note cur;
    if (! selectedNote (cur)) return;
    const int len = jlimit (1, Seq::maxSteps, lane().length.load());
    const int step = stepAt (jlimit (keyW, getWidth() - 1, e.x));
    if (drag == resizing)
    {
        const int nl = jlimit (1, len - cur.step, step - cur.step + 1);
        if (nl != cur.len)
        {
            lastLen = nl;
            moved = true;
            replaceSelected ([nl] (Keys::Note& n) { n.len = (uint8_t) nl; });
        }
        return;
    }
    const int ns = jlimit (0, jmax (0, len - cur.len), origStep + (step - downStep));
    int np = jlimit (Keys::lowNote, Keys::highNote, origPitch + (pitchAt (e.y) - downPitch));
    if (np != cur.pitch && snap) np = snapPitch (np);
    if (ns != cur.step || np != cur.pitch)
    {
        moved = true;
        const bool newPitch = np != cur.pitch;
        replaceSelected ([ns, np] (Keys::Note& n) { n.step = (uint8_t) ns; n.pitch = (uint8_t) np; });
        if (newPitch) owner.getProc().triggerKey (track, np, cur.vel / 127.0f);
    }
}

void PianoRoll::mouseUp (const MouseEvent&)
{
    if (drag == moving && ! moved && wasSelected) removeSelected();   // second click on the selected note removes it
    drag = none;
}

// ===================================================================================== VelocityLane

void VelocityLane::paint (Graphics& g)
{
    g.setColour (Colour (0xff1b1430));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
    g.setColour (muted);
    g.setFont (Font (10.0f));
    g.drawText ("Velocity", 6, 0, PianoRoll::keyW - 8, getHeight(), Justification::centredLeft);
    const int len = jlimit (1, Seq::maxSteps, roll.lane().length.load());
    const float h = (float) getHeight() - 8.0f;
    const Colour col = KeysUI::trackColour (roll.track);
    if (auto notes = roll.lane().get())
        for (const auto& n : *notes)
        {
            if (n.step >= len || n.step / 16 != roll.page) continue;
            const float x = roll.xForStep (n.step) + 4.0f;
            const float bh = h * n.vel / 127.0f;
            const bool sel = n.pitch == roll.selPitch && n.step == roll.selStep;
            g.setColour (sel ? text : col.withAlpha (0.8f));
            g.fillRoundedRectangle (x, 4.0f + h - bh, 6.0f, bh, 2.0f);
        }
}

void VelocityLane::setFrom (const MouseEvent& e, bool pick)
{
    if (pick)
    {
        const int step = roll.stepAt (e.x);
        auto notes = roll.lane().get();
        if (notes == nullptr) return;
        if (roll.selStep != step)                      // pick the highest note starting on that step
        {
            int best = -1;
            for (const auto& n : *notes) if (n.step == step && n.pitch > best) best = n.pitch;
            if (best < 0) return;
            roll.selPitch = best; roll.selStep = step;
        }
    }
    const float h = (float) getHeight() - 8.0f;
    const int v = jlimit (1, 127, (int) std::lround ((h + 4.0f - (float) e.y) / h * 127.0f));
    roll.replaceSelected ([v] (Keys::Note& n) { n.vel = (uint8_t) v; });
    repaint();
}

void VelocityLane::mouseDown (const MouseEvent& e) { setFrom (e, true); }
void VelocityLane::mouseDrag (const MouseEvent& e) { setFrom (e, false); }

// ===================================================================================== NotePanel

NotePanel::NotePanel (SequencerPage& o, PianoRoll& r) : owner (o), roll (r)
{
    styleLabel (title, 13.0f, text);
    title.setFont (Font (13.0f, Font::bold));
    addAndMakeVisible (title);
    styleLabel (emptyLabel, 12.0f);
    emptyLabel.setJustificationType (Justification::topLeft);
    emptyLabel.setText ("Click the grid to add a note (drag right to make it longer). Click a note to select it, drag to move it, "
                        "click it again to remove it. Click the keyboard to hear a note.", dontSendNotification);
    addAndMakeVisible (emptyLabel);
    auto setup = [this] (Label& l, const String& name, Slider& s, double lo, double hi)
    {
        l.setText (name, dontSendNotification); styleLabel (l, 11.0f); addAndMakeVisible (l);
        s.setSliderStyle (Slider::LinearHorizontal);
        s.setRange (lo, hi, 1.0);
        s.setTextBoxStyle (Slider::TextBoxRight, false, 70, 20);
        s.setColour (Slider::thumbColourId, accent);
        s.setColour (Slider::trackColourId, accent.withAlpha (0.5f));
        s.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        s.setColour (Slider::textBoxTextColourId, text);
        addAndMakeVisible (s);
    };
    setup (velLabel, "Velocity", vel, 1, 127);
    setup (lenLabel, "Length (steps)", len, 1, Seq::maxSteps);
    vel.onValueChange = [this] { const int v = (int) vel.getValue(); roll.replaceSelected ([v] (Keys::Note& n) { n.vel = (uint8_t) v; }); };
    len.onValueChange = [this]
    {
        const int l = (int) len.getValue();
        roll.lastLen = l;
        roll.replaceSelected ([l] (Keys::Note& n) { n.len = (uint8_t) l; });
    };
    styleButton (removeButton);
    removeButton.onClick = [this] { roll.removeSelected(); };
    addAndMakeVisible (removeButton);
    refresh();
}

void NotePanel::refresh()
{
    Keys::Note n {};
    const bool on = roll.selectedNote (n);
    for (Component* c : std::initializer_list<Component*> { &velLabel, &vel, &lenLabel, &len, &removeButton }) c->setVisible (on);
    emptyLabel.setVisible (! on);
    if (! on) { title.setText ("NOTE", dontSendNotification); return; }
    title.setText (KeysUI::noteName (n.pitch) + ", step " + String (n.step + 1), dontSendNotification);
    vel.setValue (n.vel, dontSendNotification);
    const int laneLen = jlimit (1, Seq::maxSteps, roll.lane().length.load());
    len.setRange (1.0, (double) jmax (2, laneLen - n.step), 1.0);
    len.setValue (n.len, dontSendNotification);
}

void NotePanel::paint (Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void NotePanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    title.setBounds (r.removeFromTop (20));
    r.removeFromTop (6);
    emptyLabel.setBounds (r.withHeight (110));
    velLabel.setBounds (r.removeFromTop (16)); vel.setBounds (r.removeFromTop (22)); r.removeFromTop (8);
    lenLabel.setBounds (r.removeFromTop (16)); len.setBounds (r.removeFromTop (22)); r.removeFromTop (12);
    removeButton.setBounds (r.removeFromTop (26));
}
