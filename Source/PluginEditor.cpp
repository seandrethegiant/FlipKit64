#include "PluginEditor.h"
#include "Lights.h"
#include "KitFill.h"

using namespace juce;
using namespace UI;

// ===================================================================================== PadButton

PadButton::PadButton (EditorContext& c, int i, int r, int co)
    : index (i), row (r), col (co), note (PadLayout::noteFor (r, co)), ctx (c)
{
    setRepaintsOnMouseActivity (true);
}

void PadButton::paint (Graphics& g)
{
    auto pad = ctx.getProc().getPad (index);
    const bool loaded = ! pad->layers.empty();
    const Colour hue = padHue (index);
    auto r = getLocalBounds().toFloat().reduced (3.0f);

    // Loaded pads light up in their zone colour (like the controller's drum mode);
    // empty pads keep a faint tint of it so the four zones stay visible.
    Colour fill = loaded ? hue.withMultipliedBrightness (0.92f)
                         : panel.interpolatedWith (hue, 0.10f);
    if (isMouseOver()) fill = loaded ? fill.brighter (0.12f) : fill.interpolatedWith (hue, 0.12f);
    if (flashLevel > 0.0f) fill = fill.interpolatedWith (Colours::white, flashLevel * 0.85f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 6.0f);

    if (selected || dragOver)
    {
        g.setColour (Colours::white);
        g.drawRoundedRectangle (r, 6.0f, 2.5f);
    }
    else
    {
        g.setColour (loaded ? hue.brighter (0.25f) : hue.withAlpha (0.25f));
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }

    const Colour ink = loaded ? Colour (0xff120c1c) : muted;   // dark text on lit pads
    auto inner = r.reduced (6.0f, 4.0f);
    g.setColour (loaded ? ink.withAlpha (0.6f) : muted);
    g.setFont (Font (10.0f));
    g.drawText (String (note), inner.removeFromTop (12.0f), Justification::topLeft);

    if (loaded)
    {
        String badge;
        if (pad->layers.size() > 1)
            badge << (pad->mode == LayerMode::RoundRobin ? "RR " : pad->mode == LayerMode::Velocity ? "VEL " : "RND ")
                  << (int) pad->layers.size();
        if (pad->choke > 0) badge << (badge.isEmpty() ? "" : " ") << "C" << pad->choke;
        if (badge.isNotEmpty())
        {
            g.setColour (ink.withAlpha (0.75f));
            g.drawText (badge, r.reduced (6.0f, 4.0f).removeFromTop (12.0f), Justification::topRight);
        }
        g.setColour (ink);
        g.setFont (Font (12.0f, Font::bold));
        g.drawFittedText (pad->name, inner.toNearestInt(), Justification::centred, 2, 0.8f);
    }
}

void PadButton::decay()
{
    if (flashLevel > 0.0f) { flashLevel = jmax (0.0f, flashLevel - 0.12f); repaint(); }
}

void PadButton::mouseDown (const MouseEvent& e)
{
    ctx.selectPad (index);
    // click lower on the pad = softer hit, so velocity layers can be auditioned
    const float vel = jlimit (0.05f, 1.0f, 1.0f - (float) e.y / (float) jmax (1, getHeight()) * 0.95f);
    ctx.getProc().triggerPad (index, vel, true);       // a Hold pad plays until the mouse is let go
}

void PadButton::mouseUp (const MouseEvent&) { ctx.getProc().releasePad (index); }

void PadButton::filesDropped (const StringArray& files, int, int)
{
    dragOver = false;
    Array<File> fs;
    for (auto& f : files) { File file (f); if (file.existsAsFile() || file.isDirectory()) fs.add (file); }
    ctx.dropFiles (index, fs);
}

bool PadButton::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString() == EditorContext::dragId;
}

void PadButton::itemDropped (const SourceDetails&)
{
    dragOver = false;
    ctx.dropFiles (index, ctx.browserItems());
}

// ===================================================================================== LayerList

int LayerList::getNumRows() { return (int) ctx.getProc().getPad (ctx.getSelectedPad())->layers.size(); }

void LayerList::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    auto pad = ctx.getProc().getPad (ctx.getSelectedPad());
    if (! isPositiveAndBelow (row, (int) pad->layers.size())) return;
    if (sel) { g.setColour (accent.withAlpha (0.25f)); g.fillRect (0, 0, w, h); }

    String prefix = String (row + 1) + ".  ";
    if (pad->mode == LayerMode::Velocity)
    {
        const int n = (int) pad->layers.size();
        const int lo = (int) std::ceil (127.0 * row / n), hi = (int) (127.0 * (row + 1) / n);
        prefix = "vel " + String (jmax (1, lo)) + "-" + String (hi) + "   ";
    }
    g.setColour (muted);
    g.setFont (Font (12.0f));
    g.drawText (prefix, 6, 0, 90, h, Justification::centredLeft);
    g.setColour (text);
    g.drawText (pad->layers[(size_t) row]->file.getFileName(), 96, 0, w - 100, h, Justification::centredLeft);
}

void LayerList::listBoxItemDoubleClicked (int row, const MouseEvent&)
{
    auto pad = ctx.getProc().getPad (ctx.getSelectedPad());
    if (isPositiveAndBelow (row, (int) pad->layers.size()))
        ctx.getProc().previewFile (pad->layers[(size_t) row]->file);
}

// ===================================================================================== PadsPage

PadsPage::PadsPage (EditorContext& c) : ctx (c), proc (c.getProc())
{
    for (int r = 0; r < PadLayout::rows; ++r)
        for (int co = 0; co < PadLayout::cols; ++co)
            addAndMakeVisible (padButtons.add (new PadButton (ctx, PadLayout::padIndex (r, co), r, co)));
    for (int i = 0; i < PadLayout::numPads; ++i) lastHits[i] = proc.hitCounter[i].load();

    styleLabel (padTitle, 15.0f, text);
    padTitle.setFont (Font (15.0f, Font::bold));
    addAndMakeVisible (padTitle);

    nameLabel.setEditable (false, true);
    nameLabel.setColour (Label::textColourId, text);
    nameLabel.setColour (Label::backgroundColourId, panel.brighter (0.05f));
    nameLabel.setTooltip ("Double-click to rename the pad");
    nameLabel.onTextChange = [this] { auto t = nameLabel.getText(); proc.updatePad (ctx.getSelectedPad(), [t] (PadState& p) { p.name = t; }); };
    addAndMakeVisible (nameLabel);

    modeLabel.setText ("Layers", dontSendNotification);  styleLabel (modeLabel, 11.0f); addAndMakeVisible (modeLabel);
    chokeLabel.setText ("Choke", dontSendNotification);  styleLabel (chokeLabel, 11.0f); addAndMakeVisible (chokeLabel);
    modeBox.addItem ("Round-robin", 1);
    modeBox.addItem ("Velocity layers", 2);
    modeBox.addItem ("Random", 3);
    modeBox.onChange = [this] { if (refreshing) return; const auto m = (LayerMode) (modeBox.getSelectedId() - 1);
                                proc.updatePad (ctx.getSelectedPad(), [m] (PadState& p) { p.mode = m; }); };
    addAndMakeVisible (modeBox);
    playLabel.setText ("Play", dontSendNotification); styleLabel (playLabel, 11.0f); addAndMakeVisible (playLabel);
    playBox.addItem ("One-shot (plays to the end)", 1);
    playBox.addItem ("Hold (plays while held)", 2);
    playBox.addItem ("Toggle (press again to stop)", 3);
    playBox.setTooltip ("One-shot for drum hits. Hold: plays only while you hold the pad (one step from the sequencer). "
                        "Toggle: one press starts it, the next stops it - good for long loops and records.");
    playBox.onChange = [this] { if (refreshing) return; const auto m = (PlayMode) (playBox.getSelectedId() - 1);
                                proc.updatePad (ctx.getSelectedPad(), [m] (PadState& p) { p.play = m; }); };
    addAndMakeVisible (playBox);
    for (auto* b : { &stopAllButton, &clearAllButton }) { styleButton (*b); addAndMakeVisible (b); }
    stopAllButton.setTooltip ("Stop every sound that's still playing (Esc)");
    stopAllButton.onClick = [this] { ctx.stopAllRequested(); };
    clearAllButton.setTooltip ("Clear all the pads, or start a new kit (you can save first)");
    clearAllButton.onClick = [this] { ctx.clearAllRequested(); };
    chokeBox.addItem ("Off", 1);
    for (int i = 1; i <= 8; ++i) chokeBox.addItem ("Group " + String (i), i + 1);
    chokeBox.setTooltip ("Pads in the same choke group cut each other off (e.g. closed hat stops open hat)");
    chokeBox.onChange = [this] { if (refreshing) return; const int g = chokeBox.getSelectedId() - 1;
                                 proc.updatePad (ctx.getSelectedPad(), [g] (PadState& p) { p.choke = g; }); };
    addAndMakeVisible (chokeBox);

    auto dB  = [] (double v) { return v <= -35.9 ? String ("-inf") : String (v, 1) + " dB"; };
    setupKnob (level, -36.0, 12.0, 0.1, 0.0, dB, [] (PadState& p, double v) { p.gain = Decibels::decibelsToGain ((float) v, -36.0f); });
    setupKnob (pan, -1.0, 1.0, 0.01, 0.0,
               [] (double v) { return std::abs (v) < 0.01 ? String ("C") : (v < 0 ? "L" : "R") + String ((int) std::round (std::abs (v) * 100)); },
               [] (PadState& p, double v) { p.pan = (float) v; });
    setupKnob (tune, -24.0, 24.0, 1.0, 0.0, [] (double v) { return (v > 0 ? "+" : "") + String ((int) v) + " st"; },
               [] (PadState& p, double v) { p.tune = (float) v; });
    setupKnob (fine, -100.0, 100.0, 1.0, 0.0, [] (double v) { return (v > 0 ? "+" : "") + String ((int) v) + " ct"; },
               [] (PadState& p, double v) { p.fine = (float) v; });
    setupKnob (start, 0.0, 1.0, 0.001, 0.0, [] (double v) { return String (v * 100.0, 1) + "%"; },
               [] (PadState& p, double v) { p.start = (float) jmin (v, (double) p.end - 0.001); });
    setupKnob (end, 0.0, 1.0, 0.001, 0.0, [] (double v) { return String (v * 100.0, 1) + "%"; },
               [] (PadState& p, double v) { p.end = (float) jmax (v, (double) p.start + 0.001); });
    addAndMakeVisible (envelope);
    setupKnob (cutoff, 20.0, 20000.0, 1.0, 1000.0,
               [] (double v) { return v >= 19999.0 ? String ("Off") : v < 1000.0 ? String ((int) v) + " Hz" : String (v / 1000.0, 1) + " kHz"; },
               [] (PadState& p, double v) { p.cutoff = (float) v; });
    setupKnob (reso, 0.0, 1.0, 0.01, 0.0, [] (double v) { return String ((int) std::round (v * 100)) + "%"; },
               [] (PadState& p, double v) { p.reso = (float) v; });
    level.slider.setDoubleClickReturnValue (true, 0.0);
    pan.slider.setDoubleClickReturnValue (true, 0.0);
    tune.slider.setDoubleClickReturnValue (true, 0.0);
    fine.slider.setDoubleClickReturnValue (true, 0.0);
    cutoff.slider.setDoubleClickReturnValue (true, 20000.0);

    for (auto* t : { &reverseToggle, &monoToggle })
    {
        t->setColour (ToggleButton::textColourId, text);
        t->setColour (ToggleButton::tickColourId, accent);
        addAndMakeVisible (t);
    }
    monoToggle.setTooltip ("Mono: hitting the pad again cuts off the previous hit");
    reverseToggle.onClick = [this] { if (refreshing) return; const bool b = reverseToggle.getToggleState();
                                     proc.updatePad (ctx.getSelectedPad(), [b] (PadState& p) { p.reverse = b; }); };
    monoToggle.onClick = [this] { if (refreshing) return; const bool b = monoToggle.getToggleState();
                                  proc.updatePad (ctx.getSelectedPad(), [b] (PadState& p) { p.mono = b; }); };

    layerList.setColour (ListBox::backgroundColourId, panel.brighter (0.03f));
    layerList.setRowHeight (20);
    addAndMakeVisible (layerList);

    for (auto* b : { &upButton, &downButton, &removeButton, &clearButton }) { styleButton (*b); addAndMakeVisible (b); }
    upButton.onClick     = [this] { const int r = layerList.getSelectedRow(); proc.moveLayer (ctx.getSelectedPad(), r, -1); layerList.selectRow (r - 1); };
    downButton.onClick   = [this] { const int r = layerList.getSelectedRow(); proc.moveLayer (ctx.getSelectedPad(), r, +1); layerList.selectRow (r + 1); };
    removeButton.onClick = [this] { proc.removeLayer (ctx.getSelectedPad(), layerList.getSelectedRow()); };
    clearButton.onClick  = [this] { proc.clearPad (ctx.getSelectedPad()); };

    defaultHint = "Drop a sample on a pad to replace it. Drop several, or a folder, to fill pads from there "
                  "(Shift+drop adds layers instead). Right-click a folder in the browser to load it as a kit.";
    hint.setText (defaultHint, dontSendNotification);
    styleLabel (hint, 11.0f);
    hint.setJustificationType (Justification::topLeft);
    addAndMakeVisible (hint);
}

void PadsPage::setupKnob (Knob& k, double min, double max, double interval, double skewMid,
                          std::function<String (double)> fmt, std::function<void (PadState&, double)> apply)
{
    k.slider.setRange (min, max, interval);
    if (skewMid > min && skewMid < max) k.slider.setSkewFactorFromMidPoint (skewMid);
    k.slider.textFromValueFunction = fmt;
    k.slider.updateText();
    k.slider.onValueChange = [this, &k, apply]
    {
        if (refreshing) return;
        const double v = k.slider.getValue();
        proc.updatePad (ctx.getSelectedPad(), [apply, v] (PadState& p) { apply (p, v); });
    };
    addAndMakeVisible (k);
}

void PadsPage::refreshPanel()
{
    const ScopedValueSetter<bool> svs (refreshing, true);
    const int sel = ctx.getSelectedPad();
    auto pad = proc.getPad (sel);
    auto* b = padButtons[sel];
    for (auto* pb : padButtons) { pb->selected = (pb->index == sel); pb->repaint(); }
    padTitle.setText ("Pad " + String (b->row + 1) + "-" + String (b->col + 1) + "   (note " + String (b->note) + ")",
                      dontSendNotification);
    nameLabel.setText (pad->name.isEmpty() ? String ("(empty)") : pad->name, dontSendNotification);
    modeBox.setSelectedId ((int) pad->mode + 1, dontSendNotification);
    chokeBox.setSelectedId (pad->choke + 1, dontSendNotification);
    playBox.setSelectedId ((int) pad->play + 1, dontSendNotification);
    level.slider.setValue (Decibels::gainToDecibels (pad->gain, -36.0f), dontSendNotification);
    pan.slider.setValue (pad->pan, dontSendNotification);
    tune.slider.setValue (pad->tune, dontSendNotification);
    fine.slider.setValue (pad->fine, dontSendNotification);
    start.slider.setValue (pad->start, dontSendNotification);
    end.slider.setValue (pad->end, dontSendNotification);
    cutoff.slider.setValue (pad->cutoff, dontSendNotification);
    reso.slider.setValue (pad->reso, dontSendNotification);
    reverseToggle.setToggleState (pad->reverse, dontSendNotification);
    monoToggle.setToggleState (pad->mono, dontSendNotification);
    layerList.updateContent();
    layerList.repaint();
    envelope.graph.refresh();
    envelope.refresh();
}

void PadsPage::showStatus (const String& message)
{
    hint.setText (message, dontSendNotification);
    hint.setColour (Label::textColourId, accent);
    statusAt = Time::getMillisecondCounter();
}

void PadsPage::tick()
{
    if (statusAt != 0 && Time::getMillisecondCounter() - statusAt > 8000)
    {
        hint.setText (defaultHint, dontSendNotification);
        hint.setColour (Label::textColourId, muted);
        statusAt = 0;
    }
    for (auto* b : padButtons)
    {
        const auto h = proc.hitCounter[b->index].load (std::memory_order_relaxed);
        if (h != lastHits[b->index]) { lastHits[b->index] = h; b->flash(); }
        else b->decay();
    }
}

void PadsPage::paint (Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (panelArea.toFloat(), 8.0f);
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText ("SELECTED PAD", panelArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);
}

void PadsPage::resized()
{
    auto area = getLocalBounds();
    panelArea = area.removeFromRight (310);
    area.removeFromRight (12);

    const int side = jmin (area.getWidth(), area.getHeight());
    gridArea = area.withSizeKeepingCentre (side, side).withY (area.getY());
    const float cell = (float) side / 8.0f;
    for (auto* b : padButtons)
        b->setBounds (Rectangle<float> (gridArea.getX() + b->col * cell, gridArea.getY() + b->row * cell, cell, cell).toNearestInt());

    auto pr = panelArea.reduced (12);
    {
        auto hdr = pr.removeFromTop (20);
        clearAllButton.setBounds (hdr.removeFromRight (86));
        hdr.removeFromRight (6);
        stopAllButton.setBounds (hdr.removeFromRight (92));
    }
    padTitle.setBounds (pr.removeFromTop (22));
    pr.removeFromTop (4);
    nameLabel.setBounds (pr.removeFromTop (24));
    pr.removeFromTop (8);
    {
        auto row = pr.removeFromTop (14);
        modeLabel.setBounds (row.removeFromLeft (row.getWidth() / 2));
        chokeLabel.setBounds (row);
        row = pr.removeFromTop (24);
        modeBox.setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        chokeBox.setBounds (row);
        pr.removeFromTop (6);
        row = pr.removeFromTop (24);
        playLabel.setBounds (row.removeFromLeft (36));
        playBox.setBounds (row);
    }
    pr.removeFromTop (10);
    envelope.setBounds (pr.removeFromTop (196));
    pr.removeFromTop (8);
    Knob* rows[2][4] = { { &level, &pan, &tune, &fine }, { &start, &end, &cutoff, &reso } };
    const int kw = pr.getWidth() / 4, kh = 78;
    for (auto& r : rows)
    {
        auto rr = pr.removeFromTop (kh);
        pr.removeFromTop (4);
        for (int i = 0; i < 4; ++i) r[i]->setBounds (rr.removeFromLeft (kw).reduced (2, 0));
    }
    {
        auto tr = pr.removeFromTop (24);
        reverseToggle.setBounds (tr.removeFromLeft (tr.getWidth() / 2));
        monoToggle.setBounds (tr);
    }
    pr.removeFromTop (6);
    hint.setBounds (pr.removeFromBottom (30));
    pr.removeFromBottom (6);
    auto btns = pr.removeFromBottom (24);
    const int bw = (btns.getWidth() - 18) / 4;
    upButton.setBounds (btns.removeFromLeft (bw)); btns.removeFromLeft (6);
    downButton.setBounds (btns.removeFromLeft (bw)); btns.removeFromLeft (6);
    removeButton.setBounds (btns.removeFromLeft (bw)); btns.removeFromLeft (6);
    clearButton.setBounds (btns);
    pr.removeFromBottom (6);
    layerList.setBounds (pr);
}

// ===================================================================================== Editor

PadRomplerEditor::PadRomplerEditor (PadRomplerProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setOpaque (true);
    getLookAndFeel().setColour (ResizableWindow::backgroundColourId, bg);
    getLookAndFeel().setColour (ComboBox::backgroundColourId, panel.brighter (0.06f));
    getLookAndFeel().setColour (ComboBox::textColourId, text);
    getLookAndFeel().setColour (ComboBox::outlineColourId, line);
    getLookAndFeel().setColour (PopupMenu::backgroundColourId, panel.brighter (0.04f));
    getLookAndFeel().setColour (PopupMenu::textColourId, text);
    getLookAndFeel().setColour (PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.4f));
    getLookAndFeel().setColour (ComboBox::arrowColourId, text);
    getLookAndFeel().setColour (TextButton::buttonColourId, panel.brighter (0.08f));
    getLookAndFeel().setColour (DirectoryContentsDisplayComponent::highlightColourId, right.withAlpha (0.35f));
    getLookAndFeel().setColour (ToggleButton::textColourId, text);
    getLookAndFeel().setColour (ToggleButton::tickColourId, accent);
    getLookAndFeel().setColour (ToggleButton::tickDisabledColourId, muted);
    getLookAndFeel().setColour (ScrollBar::thumbColourId, right.withAlpha (0.6f));
    getLookAndFeel().setColour (Slider::backgroundColourId, line);
    getLookAndFeel().setColour (Slider::trackColourId, right);
    getLookAndFeel().setColour (Slider::thumbColourId, accent);
    getLookAndFeel().setColour (TextEditor::backgroundColourId, panel.brighter (0.06f));
    getLookAndFeel().setColour (TextEditor::outlineColourId, line);
    getLookAndFeel().setColour (ListBox::backgroundColourId, panel);
    getLookAndFeel().setColour (AlertWindow::backgroundColourId, panel.brighter (0.03f));
    getLookAndFeel().setColour (AlertWindow::textColourId, text);
    getLookAndFeel().setColour (AlertWindow::outlineColourId, line);
    getLookAndFeel().setColour (TextEditor::textColourId, text);
    getLookAndFeel().setColour (TextEditor::focusedOutlineColourId, accent);
    getLookAndFeel().setColour (Label::textColourId, text);

    // ---- browser
    scanThread.startThread (Thread::Priority::low);
    fileTree.setMultiSelectEnabled (true);
    fileTree.setDragAndDropDescription (dragId);
    fileTree.addListener (this);
    fileTree.setColour (TreeView::backgroundColourId, panel);
    fileTree.setColour (FileTreeComponent::textColourId, text);
    fileTree.setColour (TreeView::selectedItemBackgroundColourId, right.withAlpha (0.35f));
    addAndMakeVisible (fileTree);
    addChildComponent (netBrowser);
    addAndMakeVisible (previewStrip);
    netBrowser.previewEnabled = [this] { return previewToggle.getToggleState(); };
    netBrowser.onPreview = [this] (const File& f) { proc.previewFile (f); };
    netBrowser.onOpen = [this] (const File& f) { fileDoubleClicked (f); };
    netBrowser.onPathChanged = [this] { if (netActive) browserPath.setText (netBrowser.currentDisplayPath(), dontSendNotification); };
    netBrowser.onEdit = [this] { Net::Location l; if (netLib->findLocation (currentNetKey, l)) openNetworkDialog (l, true); };
    loadPlaces();
    placesBox.setTooltip ("Sample locations - your drum kits, music library, recordings. Add any folder.");
    placesBox.onChange = [this]
    {
        const int id = placesBox.getSelectedId();
        if (id == 1000)            // add a folder
        {
            rebuildPlacesBox();
            chooser = std::make_unique<FileChooser> ("Add a folder to the browser", proc.browserRoot);
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectDirectories,
                                  [this] (const FileChooser& fc)
                                  {
                                      auto f = fc.getResult();
                                      if (! f.isDirectory()) return;
                                      places.push_back ({ f.getFileName().isEmpty() ? f.getFullPathName() : f.getFileName(), f, {} });
                                      savePlaces();
                                      setBrowserRoot (f);
                                  });
        }
        else if (id == 1002)       // add a network location
        {
            rebuildPlacesBox();
            if (! Net::isAvailable())
            {
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::InfoIcon, "Network locations",
                    "On Windows, choose \"Add a folder...\" and type your PC's share path, for example \\\\PC-NAME\\Music");
                return;
            }
            openNetworkDialog ({}, false);
        }
        else if (id == 1001)       // remove the current place
        {
            const bool removingNet = netActive;
            const auto key = currentNetKey;
            places.erase (std::remove_if (places.begin(), places.end(), [&] (const Place& p)
                          {
                              return removingNet ? p.netKey == key : (! p.isNet() && p.dir == proc.browserRoot);
                          }), places.end());
            if (removingNet) netLib->removeLocation (key);
            savePlaces();
            if (! places.empty())
            {
                if (places.front().isNet()) showNetPlace (places.front().netKey); else setBrowserRoot (places.front().dir);
            }
            else setBrowserRoot (proc.browserRoot);
        }
        else if (isPositiveAndBelow (id - 1, (int) places.size()))
        {
            const auto& p = places[(size_t) (id - 1)];
            if (p.isNet()) showNetPlace (p.netKey); else setBrowserRoot (p.dir);
        }
    };
    addAndMakeVisible (placesBox);
    previewToggle.setToggleState (true, dontSendNotification);
    previewToggle.setColour (ToggleButton::textColourId, muted);
    addAndMakeVisible (previewToggle);
    styleLabel (browserPath, 11.0f);
    addAndMakeVisible (browserPath);

    // ---- search: every sample in this location and its folders
    searchBox.setTextToShowWhenEmpty ("Search samples (Ctrl+F)", muted);
    searchBox.setTooltip ("Type words from the name or folder, e.g. \"808 kick\". Enter or double-click puts it on the selected pad, "
                          "drag it to any pad, Esc clears.");
    searchBox.setFont (Font (13.5f));
    searchBox.setIndents (8, 4);
    searchBox.onTextChange = [this] { search.setQuery (searchBox.getText()); updateSearchView(); };
    searchBox.onEscapeKey = [this] { searchBox.clear(); search.setQuery ({}); updateSearchView(); };
    searchBox.onReturnKey = [this] { search.selectFirst(); };
    searchBox.onKey = [this] (const KeyPress& k)
    {
        if (k == KeyPress::downKey && search.active() && search.numResults() > 0)   // into the results
        {
            if (auto* lb = dynamic_cast<ListBox*> (search.getChildComponent (0)))
            {
                lb->grabKeyboardFocus();
                if (lb->getSelectedRow() < 0) lb->selectRow (0);
            }
            return true;
        }
        return false;
    };
    addAndMakeVisible (searchBox);
    addChildComponent (search);
    search.onPreview = [this] (const File& f) { if (previewToggle.getToggleState()) previewNow (f); };
    search.onOpen = [this] (const File& f) { fileDoubleClicked (f); };
    search.onStatusChanged = [this] { updateSearchView(); };
    search.onShowInFolder = [this] (const File& f)
    {
        searchBox.clear(); search.setQuery ({}); updateSearchView();
        fileTree.setSelectedFile (f);
    };
    setBrowserRoot (proc.browserRoot);

    // ---- pages & tabs
    addChildComponent (padsPage);
    addChildComponent (samplerPage);
    addChildComponent (sequencerPage);
    for (int i = 0; i < 3; ++i)
    {
        styleButton (tabButtons[i]);
        tabButtons[i].setClickingTogglesState (false);
        tabButtons[i].onClick = [this, i] { showTab (i); };
        addAndMakeVisible (tabButtons[i]);
    }

    // ---- top bar
    for (auto* b : { &saveKitButton, &loadKitButton }) { styleButton (*b); addAndMakeVisible (b); }
    supportLink.setColour (HyperlinkButton::textColourId, accent);
    supportLink.setFont (Font (12.0f, Font::bold), false, Justification::centredRight);
    supportLink.setTooltip ("FlipKit64 is free - tips on Ko-fi keep it going");
    addAndMakeVisible (supportLink);
    saveKitButton.onClick = [this] { saveKitAs (nullptr); };
    loadKitButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Load kit", proc.browserRoot, "*.flipkit;*.padkit");
        chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                              [this] (const FileChooser& fc) { auto f = fc.getResult(); if (f.existsAsFile()) { proc.loadKit (f); setBrowserRoot (proc.browserRoot); } });
    };
    masterSlider.setSliderStyle (Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    masterSlider.setRange (0.0, 1.5, 0.01);
    masterSlider.setValue (proc.masterGain.load(), dontSendNotification);
    masterSlider.setColour (Slider::thumbColourId, accent);
    masterSlider.setColour (Slider::trackColourId, accent.withAlpha (0.5f));
    masterSlider.onValueChange = [this] { proc.masterGain = (float) masterSlider.getValue(); };
    addAndMakeVisible (masterSlider);
    masterLabel.setText ("Master", dontSendNotification);
    styleLabel (masterLabel, 12.0f);
    addAndMakeVisible (masterLabel);

    proc.onPadsChanged = [this]
    {
        padsPage.refreshPanel();
        for (auto* b : padsPage.padButtons) b->repaint();
        sequencerPage.padsChanged();
    };

    if (proc.lights != nullptr) proc.lights->samplerInfo = [this] { return samplerPage.lightInfo(); };
    proc.tempoPadModes = settings->getBoolValue ("tempoPadModes", true);
    proc.padLights = settings->getBoolValue ("padLights", true);
    proc.velocityMode = jlimit (0, 2, settings->getIntValue ("velocityMode", 0));
    proc.transportMsg[PadRomplerProcessor::tbStop]   = settings->getIntValue ("transportStop", PadRomplerProcessor::defaultStopCode);
    proc.transportMsg[PadRomplerProcessor::tbPlay]   = settings->getIntValue ("transportPlay", 0);
    proc.transportMsg[PadRomplerProcessor::tbRecord] = settings->getIntValue ("transportRecord", 0);
    smartSort = settings->getBoolValue ("smartSort", true);
    proc.easyLayout = jlimit (0, 2, settings->getIntValue ("easyLayout", 0));
    styleLabel (ctrlLabel, 11.0f, accent);
    ctrlLabel.setFont (Font (11.0f, Font::bold));
    ctrlLabel.setJustificationType (Justification::centred);
    ctrlLabel.setTooltip ("FlipKit64 follows the TempoPAD's mode buttons: Drum = play pads, "
                          "User = step entry, Easy = pattern select. Turn this off in the Sequencer tab.");
    addAndMakeVisible (ctrlLabel);

    showTab (0);
    selectPad (0);
    updateCtrlLabel();
    setSize (1280, 780);
    startTimerHz (30);
}

PadRomplerEditor::~PadRomplerEditor()
{
    proc.onPadsChanged = nullptr;
    proc.samplerView = false;
    if (logStream != nullptr) midiLogChanged (false);
    if (proc.lights != nullptr) proc.lights->samplerInfo = nullptr;
    fileTree.removeListener (this);
    scanThread.stopThread (2000);
}

void PadRomplerEditor::showTab (int tab)
{
    currentTab = tab;
    padsPage.setVisible (tab == 0);
    samplerPage.setVisible (tab == 1);
    sequencerPage.setVisible (tab == 2);
    proc.samplerView = tab == 1;
    for (int i = 0; i < 3; ++i) tabButtons[i].setToggleState (i == tab, dontSendNotification);
    if (tab == 2) sequencerPage.padsChanged();
}

void PadRomplerEditor::loadPlaces()
{
    PropertiesFile::Options o;
    o.applicationName = "FlipKit64";
    o.folderName = "FlipKit64";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<PropertiesFile> (o);

    places.clear();
    StringArray lines;
    lines.addLines (settings->getValue ("places"));
    for (auto& l : lines)
    {
        if (! l.contains ("|")) continue;
        const auto name = l.upToFirstOccurrenceOf ("|", false, false);
        const auto where = l.fromFirstOccurrenceOf ("|", false, false);
        if (where.startsWith ("smb://"))
        {
            Net::Location loc;
            if (Net::isAvailable() && netLib->findLocation (where.substring (6), loc)) places.push_back ({ name, {}, loc.key() });
        }
        else places.push_back ({ name, File (where), {} });
    }
    if (places.empty())     // first run: bring over locations from Pad Rompler (the beta's old name), if any
    {
        PropertiesFile::Options old = o;
        old.applicationName = "Pad Rompler"; old.folderName = "Pad Rompler";
        PropertiesFile oldSettings (old);
        StringArray oldLines;
        oldLines.addLines (oldSettings.getValue ("places"));
        for (auto& l : oldLines)
            if (l.contains ("|")) places.push_back ({ l.upToFirstOccurrenceOf ("|", false, false), File (l.fromFirstOccurrenceOf ("|", false, false)), {} });
    }
    if (places.empty())
    {
        places.push_back ({ "Music", File::getSpecialLocation (File::userMusicDirectory), {} });
        places.push_back ({ "Documents", File::getSpecialLocation (File::userDocumentsDirectory), {} });
        places.push_back ({ "Desktop", File::getSpecialLocation (File::userDesktopDirectory), {} });
    }
    {   // always offer the recordings folder
        bool hasRec = false;
        for (auto& p : places) hasRec = hasRec || (! p.isNet() && p.dir == PadRomplerProcessor::recordingsFolder());
        if (! hasRec) places.push_back ({ "My recordings", PadRomplerProcessor::recordingsFolder(), {} });
        savePlaces();
    }
}

void PadRomplerEditor::savePlaces()
{
    StringArray lines;
    for (auto& p : places) lines.add (p.name + "|" + (p.isNet() ? "smb://" + p.netKey : p.dir.getFullPathName()));
    if (settings != nullptr) { settings->setValue ("places", lines.joinIntoString ("\n")); settings->saveIfNeeded(); }
}

void PadRomplerEditor::rebuildPlacesBox()
{
    placesBox.clear (dontSendNotification);
    int current = 0;
    for (int i = 0; i < (int) places.size(); ++i)
    {
        const auto& p = places[(size_t) i];
        placesBox.addItem (p.name, i + 1);
        if (netActive ? p.netKey == currentNetKey : (! p.isNet() && p.dir == proc.browserRoot)) current = i + 1;
    }
    placesBox.addSeparator();
    placesBox.addItem ("Add a folder...", 1000);
    placesBox.addItem ("Add a network location...", 1002);
    placesBox.addItem ("Remove this location", 1001);
    if (current > 0) placesBox.setSelectedId (current, dontSendNotification);
    else placesBox.setText (netActive ? netBrowser.getLocation().name : proc.browserRoot.getFileName(), dontSendNotification);
}

void PadRomplerEditor::setBrowserRoot (const File& f)
{
    proc.browserRoot = f;
    netActive = false;
    currentNetKey.clear();
    netBrowser.setVisible (false);
    fileTree.setVisible (true);
    rebuildPlacesBox();
    if (f.isDirectory())
    {
        dirList.setDirectory (f, true, true);
        browserPath.setText (f.getFullPathName(), dontSendNotification);
    }
    else browserPath.setText (f.getFullPathName() + "  (not found - click Folder...)", dontSendNotification);
    fileTree.refresh();
    search.setRoot (f);
    updateSearchView();
}

void PadRomplerEditor::updateSearchView()
{
    searchBox.setEnabled (! netActive);
    searchBox.setTextToShowWhenEmpty (netActive ? "Search works in folders on this computer" : "Search samples (Ctrl+F)", muted);
    const bool searching = search.active() && ! netActive;
    search.setVisible (searching);
    fileTree.setVisible (! netActive && ! searching);
    netBrowser.setVisible (netActive);
    if (searching) browserPath.setText (search.status(), dontSendNotification);
    else if (netActive) browserPath.setText (netBrowser.currentDisplayPath(), dontSendNotification);
    else if (proc.browserRoot.isDirectory()) browserPath.setText (proc.browserRoot.getFullPathName(), dontSendNotification);
}

Array<File> PadRomplerEditor::browserItems() const
{
    if (netActive) return browserSelection();
    if (search.isVisible()) return search.selectedFiles();
    Array<File> fs;
    for (int i = 0; i < fileTree.getNumSelectedFiles(); ++i)
    {
        auto f = fileTree.getSelectedFile (i);
        if (f.existsAsFile() || f.isDirectory()) fs.add (f);
    }
    return fs;
}

Array<File> PadRomplerEditor::browserSelection() const
{
    if (netActive)                               // network: download (if needed) and hand over the local copies
        return const_cast<NetBrowser&> (netBrowser).selectedFilesNow();
    if (search.isVisible()) return search.selectedFiles();
    Array<File> fs;
    for (int i = 0; i < fileTree.getNumSelectedFiles(); ++i)
    {
        auto f = fileTree.getSelectedFile (i);
        if (f.existsAsFile()) fs.add (f);
    }
    return fs;
}

void PadRomplerEditor::dropFiles (int padIndex, const Array<File>& files)
{
    if (files.isEmpty()) return;
    const bool shift = ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    const bool anyFolder = std::any_of (files.begin(), files.end(), [] (const File& f) { return f.isDirectory(); });
    if (shift || (files.size() == 1 && ! anyFolder))        // one sample replaces the pad; Shift adds layers
    {
        Array<File> audio = KitFill::audioFiles (files);
        if (audio.isEmpty()) return;
        proc.assignFiles (padIndex, audio, shift);
        selectPad (padIndex);
        return;
    }
    fillPads (padIndex, files, false);                       // several samples or a folder: fill pads from here
}

int PadRomplerEditor::fillPads (int startPad, const Array<File>& filesOrFolders, bool wholeKit)
{
    auto audio = KitFill::audioFiles (filesOrFolders);
    if (audio.isEmpty()) { padsPage.showStatus ("No samples found there"); return 0; }
    const auto slots = KitFill::layout (audio, smartSort, wholeKit);
    const auto order = UI::padsByNote();
    const int startPos = (int) (std::find (order.begin(), order.end(), startPad) - order.begin());

    fillUndo.clear();
    for (int i = 0; i < PadLayout::numPads; ++i) fillUndo.push_back (proc.getPad (i));
    if (wholeKit) proc.clearAllPads();

    int filled = 0, left = 0, first = -1, last = -1;
    for (int k = 0; k < slots.size(); ++k)
    {
        if (slots[k] == File()) continue;
        if (startPos + k >= (int) order.size()) { ++left; continue; }
        const int pad = order[(size_t) (startPos + k)];
        proc.assignFiles (pad, { slots[k] }, false);
        const auto name = KitFill::padName (slots[k]);
        proc.updatePad (pad, [name] (PadState& p) { p.name = name; });
        if (first < 0) first = pad;
        last = pad; ++filled;
    }
    selectPad (first >= 0 ? first : startPad);
    String msg = String (filled) + (filled == 1 ? " sample" : " samples") + " on pads "
               + String (PadLayout::noteForPad (first >= 0 ? first : startPad)) + "-" + String (PadLayout::noteForPad (last >= 0 ? last : startPad));
    if (left > 0) msg << " (" << left << " didn't fit)";
    msg << ". Ctrl+Z puts the pads back.";
    padsPage.showStatus (msg);
    return filled;
}

bool PadRomplerEditor::undoFill()
{
    if (fillUndo.size() != (size_t) PadLayout::numPads) return false;
    for (int i = 0; i < PadLayout::numPads; ++i) proc.setPad (i, fillUndo[(size_t) i]);
    fillUndo.clear();
    padsPage.showStatus ("Pads put back as they were");
    return true;
}

void PadRomplerEditor::showFolderMenu (const File& folder)
{
    PopupMenu m;
    m.addSectionHeader (folder.getFileName());
    m.addItem (1, "Load folder as kit (replaces the pads)");
    m.addItem (2, "Fill pads from the selected pad (" + String (PadLayout::noteForPad (selectedPad)) + ")");
    m.addSeparator();
    m.addItem (3, "Sort by sound type (kicks, snares, hats... each on its own row)", true, smartSort);
    m.showMenuAsync (PopupMenu::Options(), [this, folder] (int r)
    {
        if (r == 1) { fillPads (UI::padsByNote().front(), { folder }, true); showTab (0); }
        else if (r == 2) { fillPads (selectedPad, { folder }, false); showTab (0); }
        else if (r == 3)
        {
            smartSort = ! smartSort;
            if (settings != nullptr) { settings->setValue ("smartSort", smartSort); settings->saveIfNeeded(); }
        }
    });
}

void PadRomplerEditor::openInSampler (const File& file)
{
    samplerPage.loadFile (file);
    showTab (1);
}

void PadRomplerEditor::selectPad (int index)
{
    selectedPad = jlimit (0, PadLayout::numPads - 1, index);
    proc.stepTarget = selectedPad;
    padsPage.refreshPanel();
    samplerPage.selectedPadChanged();
}

void PadRomplerEditor::previewNow (const File& f)
{
    lastPreviewed = f;
    lastPreviewMs = Time::getMillisecondCounter();
    proc.previewFile (f);
}

void PadRomplerEditor::selectionChanged()
{
    if (! previewToggle.getToggleState()) return;
    auto f = fileTree.getSelectedFile (0);
    if (f.existsAsFile() && fileTree.getNumSelectedFiles() == 1) previewNow (f);
}

void PadRomplerEditor::fileClicked (const File& f, const MouseEvent& e)
{
    if (e.mods.isPopupMenu() && f.isDirectory()) { showFolderMenu (f); return; }
    // Clicking the selected sample again plays it again (quick drum-hit auditioning).
    if (! previewToggle.getToggleState() || ! f.existsAsFile() || e.mods.isAnyModifierKeyDown()) return;
    if (f == lastPreviewed && Time::getMillisecondCounter() - lastPreviewMs < 250) return;   // just played by the selection change
    previewNow (f);
}

void PadRomplerEditor::fileDoubleClicked (const File& f)
{
    if (! f.existsAsFile()) return;
    if (currentTab == 1) samplerPage.loadFile (f);       // in the sampler: open for chopping
    else dropFiles (selectedPad, { f });                  // otherwise: put it on the selected pad
}

void PadRomplerEditor::saveKitAs (std::function<void (bool)> then)
{
    chooser = std::make_unique<FileChooser> ("Save kit", proc.browserRoot.getChildFile ("My Kit.flipkit"), "*.flipkit");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                          [this, then] (const FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              const bool saved = f != File() && proc.saveKit (f.withFileExtension ("flipkit"));
                              if (then) then (saved);
                          });
}

void PadRomplerEditor::doClear (int what)
{
    if (what == 2) proc.newKit(); else proc.clearAllPads();
    selectPad (selectedPad);
    sequencerPage.selectTrack (what == 2 ? 0 : sequencerPage.getSelectedTrack());
    repaint();
}

void PadRomplerEditor::clearAllRequested()
{
    SafePointer<PadRomplerEditor> safe (this);
    auto* w = new AlertWindow ("Clear all", "What do you want to clear?\n\n"
                               "Clear the pads: removes every sample; your patterns stay.\n"
                               "New kit: clears the pads, every pattern and the Keys tracks.",
                               MessageBoxIconType::NoIcon);
    w->addButton ("Clear the pads", 1);
    w->addButton ("New kit", 2);
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    w->enterModalState (true, ModalCallbackFunction::create ([safe] (int what)
    {
        if (safe == nullptr || what == 0) return;
        auto* ask = new AlertWindow ("Save your kit first?",
                                     what == 2 ? "Starting a new kit clears your pads, patterns and Keys tracks."
                                               : "This removes every sample from the pads.",
                                     MessageBoxIconType::NoIcon);
        ask->addButton ("Save...", 1);
        ask->addButton ("Don't save", 2);
        ask->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
        ask->enterModalState (true, ModalCallbackFunction::create ([safe, what] (int choice)
        {
            if (safe == nullptr || choice == 0) return;
            if (choice == 2) { safe->doClear (what); return; }
            safe->saveKitAs ([safe, what] (bool saved) { if (saved && safe != nullptr) safe->doClear (what); });   // cancelled save = nothing cleared
        }), true);
    }), true);
}

void PadRomplerEditor::tempoPadModesChanged (bool on)
{
    proc.tempoPadModes = on;
    if (settings != nullptr) { settings->setValue ("tempoPadModes", on); settings->saveIfNeeded(); }
    if (! on) proc.ctrlMode = PadRomplerProcessor::ctrlNone;
    updateCtrlLabel();
}

void PadRomplerEditor::stopAllRequested()
{
    previewStrip.stop();
    samplerPage.stopAuditions();
    proc.stopAllSounds();
}

void PadRomplerEditor::padLightsChanged (bool on)
{
    proc.padLights = on;
    if (settings != nullptr) { settings->setValue ("padLights", on); settings->saveIfNeeded(); }
}

void PadRomplerEditor::easyLayoutChanged (int layout)
{
    proc.easyLayout = jlimit (0, 2, layout);
    if (settings != nullptr) { settings->setValue ("easyLayout", proc.easyLayout.load()); settings->saveIfNeeded(); }
}

void PadRomplerEditor::velocityModeChanged (int mode)
{
    proc.velocityMode = jlimit (0, 2, mode);
    if (settings != nullptr) { settings->setValue ("velocityMode", proc.velocityMode.load()); settings->saveIfNeeded(); }
}

void PadRomplerEditor::transportButtonsChanged()
{
    if (settings == nullptr) return;
    const char* keys[] = { "transportStop", "transportPlay", "transportRecord" };
    for (int b = 0; b < PadRomplerProcessor::numTransportButtons; ++b) settings->setValue (keys[b], proc.transportMsg[b].load());
    settings->saveIfNeeded();
}

void PadRomplerEditor::midiLogChanged (bool on)
{
    if (! on)
    {
        drainMidiLog();
        proc.midiLog = false;
        if (logStream != nullptr) { logStream->writeText ("--- log stopped\n", false, false, "\n"); logStream->flush(); }
        logStream.reset();
        return;
    }
    // Next to FlipKit64.exe when it can write there (the standalone), else in Documents.
    auto dir = File::getSpecialLocation (File::currentExecutableFile).getParentDirectory();
    logFile = dir.getChildFile ("FlipKit64 MIDI log.txt");
    if (! dir.hasWriteAccess() || PluginHostType::getPluginLoadedAs() != AudioProcessor::wrapperType_Standalone)
        logFile = File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("FlipKit64 MIDI log.txt");
    logFile.getParentDirectory().createDirectory();
    logFile.deleteFile();
    logStream = std::make_unique<FileOutputStream> (logFile);
    if (logStream->failedToOpen()) { logStream.reset(); return; }
    logStream->writeText ("FlipKit64 MIDI log, " + Time::getCurrentTime().toString (true, true) + ", " + String (proc.logRate(), 0) + " Hz\n"
                          , false, false, "\n");
    {   // the settings that decide how Keys notes sound
        String ctx;
        const char* modes[] = { "One-shot", "Hold", "Toggle" };
        for (int t = 0; t < Keys::numTracks; ++t)
        {
            auto& kt = proc.keysTracks[t];
            const int src = kt.sourcePad.load();
            ctx << "Keys " << (t + 1) << " (" << kt.name << "): ";
            if (src < 0) { ctx << "no sound\n"; continue; }
            ctx << "pad " << PadLayout::noteForPad (src) << ", root " << kt.root.load() << ", slices " << kt.sliceCount.load()
                << ", mono " << (kt.mono.load() ? "on" : "off") << ", glide " << kt.glideMs.load() << " ms, level " << kt.level.load() << "\n";
            const int nPads = jmax (1, kt.sliceCount.load());
            for (int k = 0; k < jmin (nPads, 8); ++k)
            {
                const int pi = PadLayout::padForNote (PadLayout::noteForPad (src) + k);
                if (pi < 0) break;
                if (auto p = proc.getPad (pi); p != nullptr && ! p->layers.empty())
                    ctx << "    pad " << PadLayout::noteForPad (pi) << " \"" << p->name << "\": " << modes[jlimit (0, 2, (int) p->play)]
                        << ", gain " << String (p->gain, 2) << ", attack " << p->attackMs << " decay " << p->decayMs << " sustain " << String (p->sustain, 2)
                        << " release " << p->releaseMs << " ms, layers " << (int) p->layers.size() << ", mono " << (p->mono ? "on" : "off")
                        << ", choke " << p->choke << "\n";
            }
        }
        ctx << "Pads play Keys track: " << (proc.keysInput.load() >= 0 ? String (proc.keysInput.load() + 1) : String ("none (drums)")) << "\n\ntime (s)  what\n";
        logStream->writeText (ctx, false, false, "\n");
    }
    while (proc.logFifo.getNumReady() > 0) { int s1, n1, s2, n2; proc.logFifo.prepareToRead (proc.logFifo.getNumReady(), s1, n1, s2, n2); proc.logFifo.finishedRead (n1 + n2); }
    proc.midiLog = true;
}

void PadRomplerEditor::drainMidiLog()
{
    if (logStream == nullptr) return;
    auto& f = proc.logFifo;
    const int ready = f.getNumReady();
    if (ready <= 0) return;
    int s1, n1, s2, n2;
    f.prepareToRead (ready, s1, n1, s2, n2);
    const double rate = jmax (1.0, proc.logRate());
    auto who = [] (const PadRomplerProcessor::LogEvent& e)
    {
        if (e.track >= 0) return "Keys " + String (e.track + 1);
        if (e.pad >= 0) return "pad " + String (PadLayout::noteForPad (e.pad));
        return String ("preview");
    };
    auto line = [&] (const PadRomplerProcessor::LogEvent& e)
    {
        String t = String (e.clock / rate, 3).paddedLeft (' ', 9) + "  ";
        using E = PadRomplerProcessor::LogEvent;
        switch (e.kind)
        {
            case E::midi:
            {
                String hex;
                for (int k = 0; k < jmin (3, e.size); ++k) hex << String::toHexString (e.bytes[k]).paddedLeft ('0', 2).toUpperCase() << " ";
                const int st = e.bytes[0] & 0xF0, ch = (e.bytes[0] & 0x0F) + 1;
                String what;
                if (e.bytes[0] == 0xF0) what = "SysEx, " + String (e.size) + " bytes";
                else if (st == 0x90 && e.bytes[2] > 0) what = "note on   ch " + String (ch) + "  note " + String (e.bytes[1]) + "  velocity " + String (e.bytes[2]);
                else if (st == 0x80 || st == 0x90) what = "note off  ch " + String (ch) + "  note " + String (e.bytes[1]);
                else if (st == 0xA0) what = "pressure  ch " + String (ch) + "  note " + String (e.bytes[1]) + "  " + String (e.bytes[2]);
                else if (st == 0xB0) what = "CC        ch " + String (ch) + "  " + String (e.bytes[1]) + " = " + String (e.bytes[2]);
                else if (st == 0xD0) what = "channel pressure ch " + String (ch) + "  " + String (e.bytes[1]);
                else if (st == 0xE0) what = "pitch bend ch " + String (ch);
                return t + "MIDI   " + hex.paddedRight (' ', 10) + what;
            }
            case E::soundStart:
                return t + "  sound starts   " + who (e) + "  volume " + String (e.a, 3)
                     + (std::abs (e.b) > 0.01f ? "  pitch " + String (e.b, 1) : String())
                     + (e.gate < 0 ? "  (until let go)" : e.gate > 0 ? "  (for " + String (roundToInt (e.gate / rate * 1000.0)) + " ms)" : "  (one-shot)");
            case E::soundRelease:
                return t + "  sound let go   " + who (e) + "  after " + String (roundToInt (e.a * 1000.0f)) + " ms, level " + String (e.b, 2)
                     + (e.how == 2 ? "  (quick fade: stopped, choked or cut by the next note)" : "  (release)");
            case E::soundEnd:
            {
                const char* how[] = { "reached the end of the sample", "release finished", "faded out after being cut", "decayed to silence" };
                return t + "  sound ends     " + who (e) + "  after " + String (roundToInt (e.a * 1000.0f)) + " ms  (" + how[jlimit (0, 3, e.how)] + ")";
            }
            case E::soundStolen:
                return t + "  sound CUT OFF  " + who (e) + "  after " + String (roundToInt (e.a * 1000.0f)) + " ms  (out of voices: a new sound took its place)";
            default: return t + "?";
        }
    };
    String out;
    for (int i = 0; i < n1; ++i) out << line (proc.logBuf[s1 + i]) << "\n";
    for (int i = 0; i < n2; ++i) out << line (proc.logBuf[s2 + i]) << "\n";
    f.finishedRead (n1 + n2);
    logStream->writeText (out, false, false, "\n");
    logStream->flush();
}

void PadRomplerEditor::updateCtrlLabel()
{
    String t;
    if (proc.tempoPadModes.load())
        switch (proc.ctrlMode.load())
        {
            case PadRomplerProcessor::ctrlPads:     t = "TEMPOPAD: PADS"; break;
            case PadRomplerProcessor::ctrlSteps:    t = "TEMPOPAD: STEPS"; break;
            case PadRomplerProcessor::ctrlPatterns: t = "TEMPOPAD: PATTERNS"; break;
            default: break;
        }
    ctrlLabel.setText (t, dontSendNotification);
}

void PadRomplerEditor::timerCallback()
{
    sequencerPage.historyTick();
    drainMidiLog();
    // Follow the TempoPAD's mode buttons: User/Easy open the sequencer, Drum goes back.
    const int mode = proc.ctrlMode.load();
    if (mode != lastCtrlMode)
    {
        const bool toSeq = mode == PadRomplerProcessor::ctrlSteps || mode == PadRomplerProcessor::ctrlPatterns;
        if (proc.tempoPadModes.load())
        {
            if (toSeq && currentTab != 2) { tabBeforeCtrl = currentTab; showTab (2); }
            if (mode == PadRomplerProcessor::ctrlSteps) selectPad (proc.stepTarget.load());
            if (mode == PadRomplerProcessor::ctrlPads && tabBeforeCtrl >= 0) { showTab (tabBeforeCtrl); tabBeforeCtrl = -1; }
        }
        lastCtrlMode = mode;
        updateCtrlLabel();
        sequencerPage.ctrlModeChanged();
    }
    // Steps picked on the TempoPAD (Easy mode's bottom half): the screen follows
    if (mode == PadRomplerProcessor::ctrlSteps && proc.tempoPadModes.load() && proc.stepTarget.load() != selectedPad)
        selectPad (proc.stepTarget.load());
    previewStrip.tick();
    if (padsPage.isVisible()) padsPage.tick();
    samplerPage.tick();
    if (sequencerPage.isVisible()) sequencerPage.tick();
}

void PadRomplerEditor::paint (Graphics& g)
{
    g.fillAll (bg);
    auto top = getLocalBounds().removeFromTop (52);
    g.setColour (text);
    g.setFont (Font (20.0f, Font::bold));
    g.drawText ("FLIPKIT64", top.reduced (16, 0), Justification::centredLeft);
    g.setColour (accent);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText (FLIPKIT_VERSION_LABEL, top.reduced (16, 0).withTrimmedLeft (122).withTrimmedTop (4), Justification::centredLeft);

    g.setColour (panel);
    g.fillRoundedRectangle (browserArea.toFloat(), 8.0f);
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText ("SAMPLES", browserArea.reduced (12, 8).removeFromTop (16), Justification::centredLeft);
}

void PadRomplerEditor::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (52).reduced (16, 12);
    top.removeFromLeft (205);
    for (auto& t : tabButtons) { t.setBounds (top.removeFromLeft (110)); top.removeFromLeft (6); }
    masterSlider.setBounds (top.removeFromRight (150));
    masterLabel.setBounds (top.removeFromRight (54));
    top.removeFromRight (16);
    loadKitButton.setBounds (top.removeFromRight (96));
    top.removeFromRight (8);
    saveKitButton.setBounds (top.removeFromRight (96));
    top.removeFromRight (8);
    supportLink.setBounds (top.removeFromRight (140));
    ctrlLabel.setBounds (top.reduced (6, 0));

    area.reduce (12, 0);
    area.removeFromBottom (12);
    browserArea = area.removeFromLeft (270);
    area.removeFromLeft (12);
    contentArea = area;
    padsPage.setBounds (contentArea);
    samplerPage.setBounds (contentArea);
    sequencerPage.setBounds (contentArea);

    auto br = browserArea.reduced (10);
    br.removeFromTop (22);
    auto bTop = br.removeFromTop (26);
    previewToggle.setBounds (bTop.removeFromRight (124));
    bTop.removeFromRight (6);
    placesBox.setBounds (bTop);
    br.removeFromTop (6);
    searchBox.setBounds (br.removeFromTop (26));
    br.removeFromTop (4);
    browserPath.setBounds (br.removeFromTop (18));
    br.removeFromTop (4);
    previewStrip.setBounds (br.removeFromBottom (PreviewStrip::preferredHeight));
    br.removeFromBottom (4);
    fileTree.setBounds (br);
    netBrowser.setBounds (br);
    search.setBounds (br);
}

bool PadRomplerEditor::keyPressed (const KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'F' || key.getKeyCode() == 'f') && ! netActive)
    {
        searchBox.grabKeyboardFocus();
        searchBox.selectAll();
        return true;
    }
    if (key == KeyPress::spaceKey)
    {
        // Space plays / pauses; if the selected file isn't the one in the player, load it first.
        const auto sel = netActive ? Array<File>() : browserSelection();
        auto current = proc.playerSampleRef();
        if (sel.size() == 1 && (current == nullptr || current->file != sel[0]))
            proc.previewFile (sel[0]);
        else
            previewStrip.togglePlay();
        return true;
    }
    if (key == KeyPress::escapeKey) { previewStrip.stop(); samplerPage.stopAuditions(); proc.stopAllSounds(); return true; }
    const auto cmd = key.getModifiers().isCommandDown();
    if (cmd && (key.getKeyCode() == 'Z' || key.getKeyCode() == 'z'))
    {
        if (currentTab == 0 && ! key.getModifiers().isShiftDown() && undoFill()) return true;   // Pads tab: undo the last fill
        if (key.getModifiers().isShiftDown()) sequencerPage.redo(); else sequencerPage.undo();
        return true;
    }
    if (cmd && (key.getKeyCode() == 'Y' || key.getKeyCode() == 'y')) { sequencerPage.redo(); return true; }
    return false;
}

void PadRomplerEditor::showNetPlace (const String& key)
{
    Net::Location l;
    if (! netLib->findLocation (key, l)) return;
    netActive = true;
    currentNetKey = l.key();
    fileTree.setVisible (false);
    netBrowser.setVisible (true);
    netBrowser.setLocation (l);
    browserPath.setText (netBrowser.currentDisplayPath(), dontSendNotification);
    rebuildPlacesBox();
    updateSearchView();
}

void PadRomplerEditor::openNetworkDialog (const Net::Location& prefill, bool editing)
{
    auto* w = new AlertWindow (editing ? "Edit network location" : "Add a network location",
                               "A shared folder on your PC. To share one: on the PC, right-click the folder > "
                               "Properties > Sharing > Advanced Sharing > Share this folder.",
                               MessageBoxIconType::NoIcon);
    w->addTextEditor ("server", prefill.server, "PC name or IP address (e.g. 192.168.1.20)");
    w->addTextEditor ("share", prefill.share, "Shared folder name (e.g. Music)");
    w->addTextEditor ("folder", prefill.folder.replaceCharacter ('/', '\\'), "Start in a subfolder (optional)");
    w->addTextEditor ("user", prefill.user, "Windows username or Microsoft account email");
    w->addTextEditor ("password", prefill.password, "Password", true);
    w->addTextEditor ("name", prefill.name, "Name in FlipKit64 (optional)");
    w->addButton (editing ? "Save" : "Add", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    SafePointer<PadRomplerEditor> safe (this);
    const auto oldKey = prefill.key();
    w->enterModalState (true, ModalCallbackFunction::create ([safe, w, editing, oldKey] (int result)
    {
        if (result != 1 || safe == nullptr) return;
        Net::Location l;
        l.server   = w->getTextEditorContents ("server").trim();
        l.share    = w->getTextEditorContents ("share").trim();
        l.folder   = Net::joinPath ({}, w->getTextEditorContents ("folder"));
        l.user     = w->getTextEditorContents ("user").trim();
        l.password = w->getTextEditorContents ("password");
        l.name     = w->getTextEditorContents ("name").trim();

        // Accept a pasted path like \\PC\Music\Drum Kits in the PC field.
        auto full = l.server.replaceCharacter ('\\', '/').trimCharactersAtStart ("/");
        if (full.startsWithIgnoreCase ("smb:")) full = full.substring (4).trimCharactersAtStart ("/");
        if (full.containsChar ('/'))
        {
            auto parts = StringArray::fromTokens (full, "/", "");
            parts.removeEmptyStrings();
            l.server = parts[0];
            if (parts.size() > 1 && l.share.isEmpty()) l.share = parts[1];
            if (parts.size() > 2 && l.folder.isEmpty()) { parts.removeRange (0, 2); l.folder = parts.joinIntoString ("/"); }
        }
        if (l.server.isEmpty() || l.share.isEmpty())
        {
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Network location",
                                              "Please enter both the PC (name or IP address) and the shared folder name.");
            return;
        }
        if (l.name.isEmpty()) l.name = l.folder.isNotEmpty() ? l.folder.fromLastOccurrenceOf ("/", false, false) : l.share;

        auto& ed = *safe;
        if (editing && oldKey != l.key())
        {
            ed.netLib->removeLocation (oldKey);
            for (auto& p : ed.places) if (p.netKey == oldKey) p.netKey = l.key();
        }
        ed.netLib->saveLocation (l);
        bool known = false;
        for (auto& p : ed.places)
            if (p.netKey == l.key()) { p.name = l.name; known = true; }
        if (! known) ed.places.push_back ({ l.name, {}, l.key() });
        ed.savePlaces();
        ed.showNetPlace (l.key());
    }), true);
}
