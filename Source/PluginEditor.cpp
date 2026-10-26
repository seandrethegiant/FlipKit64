#include "PluginEditor.h"

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

    Colour fill = loaded ? hue.withMultipliedSaturation (0.55f).withMultipliedBrightness (0.45f)
                         : panel.brighter (0.04f);
    if (isMouseOver()) fill = fill.brighter (0.08f);
    if (flashLevel > 0.0f) fill = fill.interpolatedWith (hue.brighter (0.4f), flashLevel);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 6.0f);

    if (selected || dragOver)
    {
        g.setColour (dragOver ? Colours::white : accent);
        g.drawRoundedRectangle (r, 6.0f, 2.0f);
    }
    else
    {
        g.setColour (loaded ? hue.withAlpha (0.6f) : line);
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }

    auto inner = r.reduced (6.0f, 4.0f);
    g.setColour (muted);
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
            g.setColour (hue.brighter (0.3f));
            g.drawText (badge, r.reduced (6.0f, 4.0f).removeFromTop (12.0f), Justification::topRight);
        }
        g.setColour (text);
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
    ctx.getProc().triggerPad (index, vel);
}

void PadButton::filesDropped (const StringArray& files, int, int)
{
    dragOver = false;
    Array<File> fs;
    for (auto& f : files) { File file (f); if (file.existsAsFile()) fs.add (file); }
    ctx.dropFiles (index, fs);
}

bool PadButton::isInterestedInDragSource (const SourceDetails& d)
{
    return d.description.toString() == EditorContext::dragId;
}

void PadButton::itemDropped (const SourceDetails&)
{
    dragOver = false;
    ctx.dropFiles (index, ctx.browserSelection());
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
    chokeBox.addItem ("Off", 1);
    for (int i = 1; i <= 8; ++i) chokeBox.addItem ("Group " + String (i), i + 1);
    chokeBox.setTooltip ("Pads in the same choke group cut each other off (e.g. closed hat stops open hat)");
    chokeBox.onChange = [this] { if (refreshing) return; const int g = chokeBox.getSelectedId() - 1;
                                 proc.updatePad (ctx.getSelectedPad(), [g] (PadState& p) { p.choke = g; }); };
    addAndMakeVisible (chokeBox);

    auto dB  = [] (double v) { return v <= -35.9 ? String ("-inf") : String (v, 1) + " dB"; };
    auto ms  = [] (double v) { return v < 1000.0 ? String ((int) v) + " ms" : String (v / 1000.0, 2) + " s"; };
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
    setupKnob (attack, 0.0, 2000.0, 1.0, 100.0, ms, [] (PadState& p, double v) { p.attackMs = (float) v; });
    setupKnob (decay, 10.0, 5000.0, 1.0, 400.0, [ms] (double v) { return v >= 4999.0 ? String ("Off") : ms (v); },
               [] (PadState& p, double v) { p.decayMs = (float) v; });
    setupKnob (cutoff, 20.0, 20000.0, 1.0, 1000.0,
               [] (double v) { return v >= 19999.0 ? String ("Off") : v < 1000.0 ? String ((int) v) + " Hz" : String (v / 1000.0, 1) + " kHz"; },
               [] (PadState& p, double v) { p.cutoff = (float) v; });
    setupKnob (reso, 0.0, 1.0, 0.01, 0.0, [] (double v) { return String ((int) std::round (v * 100)) + "%"; },
               [] (PadState& p, double v) { p.reso = (float) v; });
    level.slider.setDoubleClickReturnValue (true, 0.0);
    pan.slider.setDoubleClickReturnValue (true, 0.0);
    tune.slider.setDoubleClickReturnValue (true, 0.0);
    fine.slider.setDoubleClickReturnValue (true, 0.0);
    decay.slider.setDoubleClickReturnValue (true, 5000.0);
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

    hint.setText ("Drop one sample to replace a pad; drop several (or hold Shift) to add layers. "
                  "Click a pad to hear it (lower = softer).", dontSendNotification);
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
    level.slider.setValue (Decibels::gainToDecibels (pad->gain, -36.0f), dontSendNotification);
    pan.slider.setValue (pad->pan, dontSendNotification);
    tune.slider.setValue (pad->tune, dontSendNotification);
    fine.slider.setValue (pad->fine, dontSendNotification);
    start.slider.setValue (pad->start, dontSendNotification);
    end.slider.setValue (pad->end, dontSendNotification);
    attack.slider.setValue (pad->attackMs, dontSendNotification);
    decay.slider.setValue (pad->decayMs, dontSendNotification);
    cutoff.slider.setValue (pad->cutoff, dontSendNotification);
    reso.slider.setValue (pad->reso, dontSendNotification);
    reverseToggle.setToggleState (pad->reverse, dontSendNotification);
    monoToggle.setToggleState (pad->mono, dontSendNotification);
    layerList.updateContent();
    layerList.repaint();
}

void PadsPage::tick()
{
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
    pr.removeFromTop (20);
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
    }
    pr.removeFromTop (8);
    Knob* rows[3][4] = { { &level, &pan, &tune, &fine }, { &start, &end, &attack, &decay }, { &cutoff, &reso, nullptr, nullptr } };
    const int kw = pr.getWidth() / 4, kh = 80;
    for (auto& r : rows)
    {
        auto rr = pr.removeFromTop (kh);
        pr.removeFromTop (6);
        for (int i = 0; i < 4; ++i)
        {
            auto kcell = rr.removeFromLeft (kw);
            if (r[i] != nullptr) r[i]->setBounds (kcell.reduced (2, 0));
            else if (i == 2) { reverseToggle.setBounds (kcell.withHeight (24).withY (kcell.getY() + 14)); monoToggle.setBounds (kcell.withHeight (24).withY (kcell.getY() + 40)); }
        }
    }
    pr.removeFromTop (6);
    hint.setBounds (pr.removeFromBottom (44));
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

    // ---- browser
    scanThread.startThread (Thread::Priority::low);
    fileTree.setMultiSelectEnabled (true);
    fileTree.setDragAndDropDescription (dragId);
    fileTree.addListener (this);
    fileTree.setColour (TreeView::backgroundColourId, panel);
    fileTree.setColour (FileTreeComponent::textColourId, text);
    fileTree.setColour (TreeView::selectedItemBackgroundColourId, accent.withAlpha (0.3f));
    addAndMakeVisible (fileTree);
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
                                      places.push_back ({ f.getFileName().isEmpty() ? f.getFullPathName() : f.getFileName(), f });
                                      savePlaces();
                                      setBrowserRoot (f);
                                  });
        }
        else if (id == 1001)       // remove the current place
        {
            places.erase (std::remove_if (places.begin(), places.end(),
                                          [this] (const std::pair<String, File>& p) { return p.second == proc.browserRoot; }),
                          places.end());
            savePlaces();
            if (! places.empty()) setBrowserRoot (places.front().second); else rebuildPlacesBox();
        }
        else if (isPositiveAndBelow (id - 1, (int) places.size()))
            setBrowserRoot (places[(size_t) (id - 1)].second);
    };
    addAndMakeVisible (placesBox);
    previewToggle.setToggleState (true, dontSendNotification);
    previewToggle.setColour (ToggleButton::textColourId, muted);
    addAndMakeVisible (previewToggle);
    styleLabel (browserPath, 11.0f);
    addAndMakeVisible (browserPath);
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
    saveKitButton.onClick = [this]
    {
        chooser = std::make_unique<FileChooser> ("Save kit", proc.browserRoot.getChildFile ("My Kit.flipkit"), "*.flipkit");
        chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting,
                              [this] (const FileChooser& fc) { auto f = fc.getResult(); if (f != File()) proc.saveKit (f.withFileExtension ("flipkit")); });
    };
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

    showTab (0);
    selectPad (0);
    setSize (1280, 780);
    startTimerHz (30);
}

PadRomplerEditor::~PadRomplerEditor()
{
    proc.onPadsChanged = nullptr;
    fileTree.removeListener (this);
    scanThread.stopThread (2000);
}

void PadRomplerEditor::showTab (int tab)
{
    currentTab = tab;
    padsPage.setVisible (tab == 0);
    samplerPage.setVisible (tab == 1);
    sequencerPage.setVisible (tab == 2);
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
        if (l.contains ("|")) places.push_back ({ l.upToFirstOccurrenceOf ("|", false, false), File (l.fromFirstOccurrenceOf ("|", false, false)) });
    if (places.empty())     // first run: bring over locations from Pad Rompler (the beta's old name), if any
    {
        PropertiesFile::Options old = o;
        old.applicationName = "Pad Rompler"; old.folderName = "Pad Rompler";
        PropertiesFile oldSettings (old);
        StringArray oldLines;
        oldLines.addLines (oldSettings.getValue ("places"));
        for (auto& l : oldLines)
            if (l.contains ("|")) places.push_back ({ l.upToFirstOccurrenceOf ("|", false, false), File (l.fromFirstOccurrenceOf ("|", false, false)) });
    }
    if (places.empty())
    {
        places.push_back ({ "Music", File::getSpecialLocation (File::userMusicDirectory) });
        places.push_back ({ "Documents", File::getSpecialLocation (File::userDocumentsDirectory) });
        places.push_back ({ "Desktop", File::getSpecialLocation (File::userDesktopDirectory) });
    }
    {   // always offer the recordings folder
        bool hasRec = false;
        for (auto& p : places) hasRec = hasRec || p.second == PadRomplerProcessor::recordingsFolder();
        if (! hasRec) places.push_back ({ "My recordings", PadRomplerProcessor::recordingsFolder() });
        savePlaces();
    }
}

void PadRomplerEditor::savePlaces()
{
    StringArray lines;
    for (auto& p : places) lines.add (p.first + "|" + p.second.getFullPathName());
    if (settings != nullptr) { settings->setValue ("places", lines.joinIntoString ("\n")); settings->saveIfNeeded(); }
}

void PadRomplerEditor::rebuildPlacesBox()
{
    placesBox.clear (dontSendNotification);
    int current = 0;
    for (int i = 0; i < (int) places.size(); ++i)
    {
        placesBox.addItem (places[(size_t) i].first, i + 1);
        if (places[(size_t) i].second == proc.browserRoot) current = i + 1;
    }
    placesBox.addSeparator();
    placesBox.addItem ("Add a folder...", 1000);
    placesBox.addItem ("Remove this location", 1001);
    if (current > 0) placesBox.setSelectedId (current, dontSendNotification);
    else placesBox.setText (proc.browserRoot.getFileName(), dontSendNotification);
}

void PadRomplerEditor::setBrowserRoot (const File& f)
{
    proc.browserRoot = f;
    rebuildPlacesBox();
    if (f.isDirectory())
    {
        dirList.setDirectory (f, true, true);
        browserPath.setText (f.getFullPathName(), dontSendNotification);
    }
    else browserPath.setText (f.getFullPathName() + "  (not found - click Folder...)", dontSendNotification);
    fileTree.refresh();
}

Array<File> PadRomplerEditor::browserSelection() const
{
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
    const bool append = ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    proc.assignFiles (padIndex, files, append);
    selectPad (padIndex);
}

void PadRomplerEditor::openInSampler (const File& file)
{
    samplerPage.loadFile (file);
    showTab (1);
}

void PadRomplerEditor::selectPad (int index)
{
    selectedPad = jlimit (0, PadLayout::numPads - 1, index);
    padsPage.refreshPanel();
    samplerPage.selectedPadChanged();
}

void PadRomplerEditor::selectionChanged()
{
    if (! previewToggle.getToggleState()) return;
    auto f = fileTree.getSelectedFile (0);
    if (f.existsAsFile()) proc.previewFile (f);
}

void PadRomplerEditor::fileDoubleClicked (const File& f)
{
    if (! f.existsAsFile()) return;
    if (currentTab == 1) samplerPage.loadFile (f);       // in the sampler: open for chopping
    else dropFiles (selectedPad, { f });                  // otherwise: put it on the selected pad
}

void PadRomplerEditor::timerCallback()
{
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
    br.removeFromTop (4);
    browserPath.setBounds (br.removeFromTop (18));
    br.removeFromTop (4);
    fileTree.setBounds (br);
}
