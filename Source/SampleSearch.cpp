#include "SampleSearch.h"
#include "KitFill.h"

using namespace juce;
using namespace UI;

static bool isAudioFile (const File& f) { return f.hasFileExtension ("wav;wave;aif;aiff;flac;ogg;mp3"); }

void SampleSearch::Indexer::run()
{
    std::vector<Entry> batch;
    auto flush = [&]
    {
        const ScopedLock sl (lock);
        for (auto& e : batch) found.push_back (std::move (e));
        batch.clear();
    };
    int n = 0;
    for (const auto& it : RangedDirectoryIterator (root, true, "*", File::findFiles))
    {
        if (threadShouldExit()) return;
        const auto f = it.getFile();
        if (! isAudioFile (f)) continue;
        Entry e;
        e.file = f;
        e.folder = f.getParentDirectory().getRelativePathFrom (root);
        if (e.folder == ".") e.folder = {};
        e.lowerName = f.getFileNameWithoutExtension().toLowerCase();
        e.lowerRel = (e.folder + "/" + e.lowerName).toLowerCase().replaceCharacters ("_-\\", "  /");
        batch.push_back (std::move (e));
        count = ++n;
        if (batch.size() >= 500) flush();
        if (n >= 200000) break;                                   // a whole drive: stop somewhere sensible
    }
    flush();
}

SampleSearch::SampleSearch()
{
    list.setRowHeight (34);
    list.setMultipleSelectionEnabled (true);
    list.setColour (ListBox::backgroundColourId, panel);
    list.setColour (ListBox::outlineColourId, Colours::transparentBlack);
    addAndMakeVisible (list);
}

SampleSearch::~SampleSearch()
{
    stopTimer();
    if (indexer != nullptr) indexer->stopThread (2000);
}

void SampleSearch::setRoot (const File& r)
{
    if (r == root && indexer != nullptr) return;
    root = r;
    if (indexer != nullptr) indexer->stopThread (2000);
    indexer.reset();
    index.clear();
    results.clear();
    list.updateContent();
    lastCount = -1;
    if (! root.isDirectory()) { if (onStatusChanged) onStatusChanged(); return; }
    indexer = std::make_unique<Indexer> (root);
    indexer->startThread (Thread::Priority::low);
    startTimerHz (5);
}

void SampleSearch::timerCallback()
{
    if (indexer == nullptr) { stopTimer(); return; }
    {
        const ScopedLock sl (indexer->lock);
        for (auto& e : indexer->found) index.push_back (std::move (e));
        indexer->found.clear();
    }
    const bool done = ! indexer->isThreadRunning();
    if ((int) index.size() != lastCount || done)
    {
        lastCount = (int) index.size();
        if (active()) filter();
        if (onStatusChanged) onStatusChanged();
    }
    if (done)
    {
        const ScopedLock sl (indexer->lock);
        if (indexer->found.empty()) stopTimer();
    }
}

bool SampleSearch::matches (const String& lowerRelPath, const StringArray& lowerWords)
{
    for (auto& w : lowerWords) if (! lowerRelPath.contains (w)) return false;
    return true;
}

void SampleSearch::setQuery (const String& text)
{
    query = text;
    filter();
    if (onStatusChanged) onStatusChanged();
}

void SampleSearch::filter()
{
    StringArray words;
    words.addTokens (query.toLowerCase().replaceCharacters ("_-\\", "  /"), " ", "\"");
    words.removeEmptyStrings();
    const auto selectedBefore = selectedFiles();
    results.clear();
    if (! words.isEmpty())
    {
        std::vector<const Entry*> inName, inPath;
        for (const auto& e : index)
        {
            if (! matches (e.lowerRel, words)) continue;
            (matches (e.lowerName, words) ? inName : inPath).push_back (&e);   // name matches first
            if ((int) (inName.size() + inPath.size()) >= maxResults * 4) break;
        }
        auto byName = [] (const Entry* a, const Entry* b)
        {
            const int c = KitFill::naturalCompare (a->file.getFileName(), b->file.getFileName());
            return c != 0 ? c < 0 : a->folder < b->folder;
        };
        std::sort (inName.begin(), inName.end(), byName);
        std::sort (inPath.begin(), inPath.end(), byName);
        for (auto* v : { &inName, &inPath })
            for (auto* e : *v)
                if ((int) results.size() < maxResults) results.push_back (*e);
    }
    list.updateContent();
    list.deselectAllRows();
    for (int i = 0; i < (int) results.size(); ++i)
        if (selectedBefore.contains (results[(size_t) i].file)) list.selectRow (i, false, false);
    list.repaint();
}

String SampleSearch::status() const
{
    const String where = root.getFileName().isNotEmpty() ? root.getFileName() : root.getFullPathName();
    const bool indexing = isIndexing();
    const String count = String ((int) index.size()) + " samples";
    if (! active()) return indexing ? "Getting " + where + " ready to search... " + count : count + " in " + where + " (and its folders)";
    String s = String ((int) results.size()) + ((int) results.size() >= maxResults ? "+" : "") + (results.size() == 1 ? " match" : " matches");
    s << (indexing ? " so far (still looking: " + count + ")" : " in " + count);
    return s;
}

Array<File> SampleSearch::selectedFiles() const
{
    Array<File> fs;
    for (int i = 0; i < list.getNumSelectedRows(); ++i)
    {
        const int r = list.getSelectedRow (i);
        if (isPositiveAndBelow (r, (int) results.size()) && results[(size_t) r].file.existsAsFile()) fs.add (results[(size_t) r].file);
    }
    return fs;
}

void SampleSearch::selectFirst()
{
    if (results.empty()) return;
    const int r = jmax (0, list.getSelectedRow());
    list.selectRow (r);
    if (onOpen) onOpen (results[(size_t) r].file);
}

void SampleSearch::paint (Graphics& g)
{
    g.fillAll (panel);
    if (results.empty() && active())
    {
        g.setColour (muted);
        g.setFont (13.0f);
        g.drawFittedText (isIndexing() ? "Looking..." : "No samples match \"" + query.trim() + "\"",
                          getLocalBounds().reduced (12).removeFromTop (60), Justification::centred, 3);
    }
}

void SampleSearch::paintListBoxItem (int row, Graphics& g, int w, int h, bool selected)
{
    if (! isPositiveAndBelow (row, (int) results.size())) return;
    const auto& e = results[(size_t) row];
    if (selected) { g.setColour (right.withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
    g.setColour (text);
    g.setFont (13.5f);
    g.drawText (e.file.getFileNameWithoutExtension(), 8, 2, w - 12, h / 2, Justification::bottomLeft, true);
    g.setColour (muted);
    g.setFont (11.0f);
    g.drawText (e.folder.isEmpty() ? String ("(top folder)") : e.folder.replaceCharacter ('\\', '/'),
                8, h / 2 + 1, w - 12, h / 2 - 3, Justification::topLeft, true);
}

void SampleSearch::listBoxItemClicked (int row, const MouseEvent& e)
{
    if (! isPositiveAndBelow (row, (int) results.size())) return;
    const auto f = results[(size_t) row].file;
    if (e.mods.isPopupMenu())
    {
        PopupMenu m;
        m.addItem (1, "Show in its folder");
        m.showMenuAsync (PopupMenu::Options(), [this, f] (int r) { if (r == 1 && onShowInFolder) onShowInFolder (f); });
        return;
    }
    if (! e.mods.isAnyModifierKeyDown() && onPreview) onPreview (f);
}

void SampleSearch::listBoxItemDoubleClicked (int row, const MouseEvent&)
{
    if (isPositiveAndBelow (row, (int) results.size()) && onOpen) onOpen (results[(size_t) row].file);
}

void SampleSearch::returnKeyPressed (int row)
{
    if (isPositiveAndBelow (row, (int) results.size()) && onOpen) onOpen (results[(size_t) row].file);
}
