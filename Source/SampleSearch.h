#pragma once
#include "Common.h"

// Search box results for the sample browser: every audio file under the current folder (and its subfolders)
// whose name or folder path contains all the words typed, e.g. "808 kick" finds "808 Kits/Kick 01.wav".
// The folder is indexed once in the background; typing filters the index instantly.
class SampleSearch : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::Timer
{
public:
    SampleSearch();
    ~SampleSearch() override;

    void setRoot (const juce::File& root);          // (re)index this folder
    void reindex() { auto r = root; root = juce::File(); setRoot (r); }
    void setQuery (const juce::String& text);
    juce::String getQuery() const { return query; }
    bool active() const { return query.trim().isNotEmpty(); }

    juce::Array<juce::File> selectedFiles() const;
    void selectFirst();                             // Enter in the search box
    juce::String status() const;                    // "Indexing... 1200 samples" / "35 of 4100 samples"

    std::function<void (const juce::File&)> onPreview;        // single click
    std::function<void (const juce::File&)> onOpen;           // double click / Enter
    std::function<void (const juce::File&)> onShowInFolder;   // right-click
    std::function<void()> onStatusChanged;

    void paint (juce::Graphics&) override;
    void resized() override { list.setBounds (getLocalBounds()); }

    // for tests
    static bool matches (const juce::String& lowerRelPath, const juce::StringArray& lowerWords);
    bool isIndexing() const { return indexer != nullptr && indexer->isThreadRunning(); }
    int numResults() const { return (int) results.size(); }
    juce::File result (int i) const { return juce::isPositiveAndBelow (i, (int) results.size()) ? results[(size_t) i].file : juce::File(); }

private:
    struct Entry { juce::File file; juce::String lowerRel, lowerName, folder; };
    struct Indexer : public juce::Thread
    {
        Indexer (const juce::File& r) : juce::Thread ("FlipKit64 sample search"), root (r) {}
        void run() override;
        juce::File root;
        juce::CriticalSection lock;
        std::vector<Entry> found;           // guarded by lock
        std::atomic<int> count { 0 };
    };
    std::unique_ptr<Indexer> indexer;
    juce::File root;
    std::vector<Entry> index;               // message thread copy
    std::vector<Entry> results;
    juce::String query;
    juce::ListBox list { {}, this };
    int lastCount = -1;
    static constexpr int maxResults = 3000;

    void filter();
    void timerCallback() override;

    int getNumRows() override { return (int) results.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override { return EditorContext::dragId; }
};
