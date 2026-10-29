#pragma once
#include "Common.h"
#include "Network.h"

// Sample browser for a network location: folders and audio files on a PC's shared folder.
// Selected files are downloaded into the local cache on demand, then behave like local files.
class NetBrowser : public juce::Component,
                   private juce::ListBoxModel
{
public:
    NetBrowser();

    void setLocation (const Net::Location&);
    const Net::Location& getLocation() const { return loc; }
    juce::String currentDisplayPath() const;

    // The selected audio files as local (cached) files, downloading any that aren't cached yet.
    juce::Array<juce::File> selectedFilesNow();

    std::function<void (const juce::File&)> onPreview;   // single click (when preview is on)
    std::function<void (const juce::File&)> onOpen;      // double click on a file
    std::function<void()> onEdit;                        // "Edit..." after a connection error
    std::function<void()> onPathChanged;
    std::function<bool()> previewEnabled;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int lastRowSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void previewRow (int row);
    int lastPreviewRow = -1;
    juce::uint32 lastPreviewMs = 0;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    juce::String getTooltipForRow (int row) override;

    bool hasUpRow() const { return path.isNotEmpty(); }
    const Net::Entry* entryForRow (int row) const;
    juce::String remotePathFor (const Net::Entry& e) const { return Net::joinPath (path, e.name); }
    bool isCached (const Net::Entry& e) const;
    void activate (int row);
    void navigate (const juce::String& newPath);
    void showError (const juce::String&);

    Net::SharedLibrary lib;
    Net::Location loc;
    juce::String path;
    juce::Array<Net::Entry> entries;
    bool loading = false;
    juce::String error;
    int generation = 0;              // ignores answers to requests that are no longer current
    int downloading = 0;

    juce::ListBox list { "Network files", this };
    juce::Label status, errorLabel;
    juce::TextButton retryButton { "Retry" }, editButton { "Edit..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetBrowser)
};
