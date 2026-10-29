#include "NetBrowser.h"

using namespace juce;
using namespace UI;

NetBrowser::NetBrowser()
{
    list.setMultipleSelectionEnabled (true);
    list.setRowHeight (24);
    list.setColour (ListBox::backgroundColourId, panel);
    list.setColour (ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible (list);

    styleLabel (status, 11.0f);
    addAndMakeVisible (status);

    styleLabel (errorLabel, 13.0f, text);
    errorLabel.setJustificationType (Justification::topLeft);
    addChildComponent (errorLabel);
    for (auto* b : { &retryButton, &editButton }) { styleButton (*b); addChildComponent (b); }
    retryButton.onClick = [this] { navigate (path); };
    editButton.onClick  = [this] { if (onEdit) onEdit(); };
}

void NetBrowser::setLocation (const Net::Location& l)
{
    loc = l;
    navigate (Net::joinPath ({}, loc.folder));
}

String NetBrowser::currentDisplayPath() const
{
    auto p = "\\\\" + loc.server + "\\" + loc.share;
    if (path.isNotEmpty()) p << "\\" << path.replaceCharacter ('/', '\\');
    return p;
}

void NetBrowser::navigate (const String& newPath)
{
    path = newPath;
    loading = true;
    error.clear();
    entries.clear();
    errorLabel.setVisible (false); retryButton.setVisible (false); editButton.setVisible (false);
    list.setVisible (true);
    list.deselectAllRows();
    list.updateContent();
    status.setText ("Connecting to " + loc.server + "...", dontSendNotification);
    if (onPathChanged) onPathChanged();
    repaint();

    const int gen = ++generation;
    SafePointer<NetBrowser> safe (this);
    lib->list (loc, path, [safe, gen] (bool ok, Array<Net::Entry> result, String err)
    {
        if (safe == nullptr || gen != safe->generation) return;
        safe->loading = false;
        if (! ok) { safe->showError (err); return; }
        safe->entries = result;
        int files = 0;
        for (auto& e : result) files += e.isDir ? 0 : 1;
        safe->status.setText (result.isEmpty() ? "No folders or audio files here"
                                               : String (result.size() - files) + " folders, " + String (files) + " samples",
                              dontSendNotification);
        safe->list.updateContent();
        safe->list.repaint();
    });
}

void NetBrowser::showError (const String& e)
{
    error = e;
    list.setVisible (false);
    errorLabel.setText (e, dontSendNotification);
    errorLabel.setVisible (true); retryButton.setVisible (true); editButton.setVisible (true);
    status.setText ("Not connected", dontSendNotification);
    resized();
}

const Net::Entry* NetBrowser::entryForRow (int row) const
{
    const int i = row - (hasUpRow() ? 1 : 0);
    return isPositiveAndBelow (i, entries.size()) ? &entries.getReference (i) : nullptr;
}

bool NetBrowser::isCached (const Net::Entry& e) const
{
    const auto f = Net::cacheFileFor (loc, remotePathFor (e));
    return f.existsAsFile() && f.getSize() == e.size;
}

int NetBrowser::getNumRows() { return entries.size() + (hasUpRow() ? 1 : 0); }

void NetBrowser::paintListBoxItem (int row, Graphics& g, int w, int h, bool selected)
{
    if (selected) g.fillAll (right.withAlpha (0.35f));
    auto r = Rectangle<int> (0, 0, w, h).reduced (6, 0);
    auto icon = r.removeFromLeft (18).toFloat().withSizeKeepingCentre (14.0f, 11.0f);
    r.removeFromLeft (6);

    if (hasUpRow() && row == 0)
    {
        g.setColour (muted);
        g.setFont (Font (13.0f));
        g.drawText ("..  (up a folder)", r, Justification::centredLeft);
        return;
    }
    auto* e = entryForRow (row);
    if (e == nullptr) return;

    if (e->isDir)
    {
        g.setColour (right.withAlpha (0.9f));
        g.fillRoundedRectangle (icon.withTrimmedTop (2.0f), 2.0f);
        g.fillRoundedRectangle (icon.withWidth (6.0f).withHeight (4.0f), 1.0f);
    }
    else
    {
        g.setColour (accent);                         // little waveform glyph
        const float hs[] = { 0.35f, 0.8f, 0.55f, 1.0f, 0.45f };
        for (int i = 0; i < 5; ++i)
        {
            const float bh = icon.getHeight() * hs[i];
            g.fillRect (icon.getX() + (float) i * 3.0f, icon.getCentreY() - bh * 0.5f, 2.0f, bh);
        }
    }

    if (! e->isDir)
    {
        const auto mb = (double) e->size / (1024.0 * 1024.0);
        const String size = mb >= 1.0 ? String (mb, 1) + " MB" : String (jmax<int64> (1, e->size / 1024)) + " KB";
        g.setColour (muted);
        g.setFont (Font (10.5f));
        auto sr = r.removeFromRight (56);
        g.drawText (size, sr, Justification::centredRight);
        if (isCached (*e))                             // downloaded already: small cyan dot
        {
            g.setColour (zoneCyan);
            g.fillEllipse ((float) sr.getX() - 4.0f, (float) h * 0.5f - 2.5f, 5.0f, 5.0f);
        }
        r.removeFromRight (8);
    }
    g.setColour (text);
    g.setFont (Font (13.0f));
    g.drawText (e->name, r, Justification::centredLeft, true);
}

String NetBrowser::getTooltipForRow (int row)
{
    if (auto* e = entryForRow (row))
        return e->isDir ? "Double-click to open" : "Click to hear it, drag it onto a pad, or double-click to load it";
    return {};
}

void NetBrowser::selectedRowsChanged (int lastRowSelected)
{
    if (list.getNumSelectedRows() == 1) previewRow (lastRowSelected);
}

void NetBrowser::listBoxItemClicked (int row, const MouseEvent& e)
{
    // Clicking the selected sample again plays it again (quick drum-hit auditioning).
    if (e.mods.isAnyModifierKeyDown()) return;
    if (row == lastPreviewRow && Time::getMillisecondCounter() - lastPreviewMs < 250) return;
    previewRow (row);
}

void NetBrowser::previewRow (int lastRowSelected)
{
    auto* e = entryForRow (lastRowSelected);
    if (e == nullptr || e->isDir) return;
    if (previewEnabled && ! previewEnabled()) return;
    lastPreviewRow = lastRowSelected;
    lastPreviewMs = Time::getMillisecondCounter();

    ++downloading; repaint();
    const auto target = remotePathFor (*e);
    const int gen = generation;
    SafePointer<NetBrowser> safe (this);
    lib->fetch (loc, target, [safe, gen, lastRowSelected] (File f, String err)
    {
        if (safe == nullptr) return;
        --safe->downloading; safe->list.repaint(); safe->repaint();
        if (gen != safe->generation) return;
        if (f.existsAsFile())
        {
            if (safe->list.isRowSelected (lastRowSelected) && safe->onPreview) safe->onPreview (f);
        }
        else safe->status.setText (err, dontSendNotification);
    });
}

void NetBrowser::listBoxItemDoubleClicked (int row, const MouseEvent&) { activate (row); }
void NetBrowser::returnKeyPressed (int lastRowSelected) { activate (lastRowSelected); }

void NetBrowser::activate (int row)
{
    if (hasUpRow() && row == 0)
    {
        navigate (path.containsChar ('/') ? path.upToLastOccurrenceOf ("/", false, false) : String());
        return;
    }
    auto* e = entryForRow (row);
    if (e == nullptr) return;
    if (e->isDir) { navigate (remotePathFor (*e)); return; }

    ++downloading; repaint();
    SafePointer<NetBrowser> safe (this);
    lib->fetch (loc, remotePathFor (*e), [safe] (File f, String err)
    {
        if (safe == nullptr) return;
        --safe->downloading; safe->list.repaint(); safe->repaint();
        if (f.existsAsFile()) { if (safe->onOpen) safe->onOpen (f); }
        else safe->status.setText (err, dontSendNotification);
    });
}

var NetBrowser::getDragSourceDescription (const SparseSet<int>& rows)
{
    for (int i = 0; i < rows.size(); ++i)
        if (auto* e = entryForRow (rows[i]))
            if (! e->isDir) return EditorContext::dragId;
    return {};
}

Array<File> NetBrowser::selectedFilesNow()
{
    Array<File> out;
    for (int i = 0; i < list.getNumSelectedRows(); ++i)
    {
        auto* e = entryForRow (list.getSelectedRow (i));
        if (e == nullptr || e->isDir) continue;
        const auto cached = Net::cacheFileFor (loc, remotePathFor (*e));
        if (cached.existsAsFile() && cached.getSize() == e->size) { out.add (cached); continue; }

        MouseCursor::showWaitCursor();
        String err;
        auto f = lib->fetchNow (loc, remotePathFor (*e), err, 120000);
        MouseCursor::hideWaitCursor();
        if (f.existsAsFile()) out.add (f);
        else status.setText (err, dontSendNotification);
    }
    list.repaint();
    return out;
}

void NetBrowser::paint (Graphics& g)
{
    if (downloading > 0)
    {
        g.setColour (accent);
        g.setFont (Font (11.0f, Font::bold));
        g.drawText ("Downloading...", getLocalBounds().removeFromTop (16), Justification::centredRight);
    }
}

void NetBrowser::resized()
{
    auto r = getLocalBounds();
    status.setBounds (r.removeFromTop (16).withTrimmedRight (90));
    r.removeFromTop (4);
    list.setBounds (r);

    auto er = r.reduced (4, 8);
    errorLabel.setBounds (er.removeFromTop (120));
    er.removeFromTop (6);
    auto buttons = er.removeFromTop (28);
    retryButton.setBounds (buttons.removeFromLeft (90));
    buttons.removeFromLeft (8);
    editButton.setBounds (buttons.removeFromLeft (90));
}
