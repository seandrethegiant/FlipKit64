#pragma once
#include "Common.h"
#include "EnvelopeEditor.h"
#include "SamplerPage.h"
#include "SequencerPage.h"
#include "NetBrowser.h"
#include "PreviewStrip.h"

class PadsPage;

// One pad in the 8x8 grid.
class PadButton : public juce::Component,
                  public juce::FileDragAndDropTarget,
                  public juce::DragAndDropTarget
{
public:
    PadButton (EditorContext& ctx, int index, int row, int col);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dragOver = false; repaint(); }
    void itemDropped (const SourceDetails&) override;

    void flash() { flashLevel = 1.0f; repaint(); }
    void decay();

    const int index, row, col, note;
    bool selected = false;

private:
    EditorContext& ctx;
    bool dragOver = false;
    float flashLevel = 0.0f;
};

// Layer list for the selected pad.
class LayerList : public juce::ListBoxModel
{
public:
    explicit LayerList (EditorContext& e) : ctx (e) {}
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
private:
    EditorContext& ctx;
};

// "Pads" tab: the 8x8 grid plus the selected pad's settings.
class PadsPage : public juce::Component
{
public:
    explicit PadsPage (EditorContext&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refreshPanel();
    void tick();                      // pad flashes
    juce::OwnedArray<PadButton> padButtons;

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;
    uint32_t lastHits[PadLayout::numPads] {};

    juce::Label padTitle, nameLabel, hint, modeLabel, chokeLabel, playLabel;
    juce::ComboBox modeBox, chokeBox, playBox;
    juce::TextButton stopAllButton { "Stop sounds" }, clearAllButton { "Clear all..." };
    Knob level { "Level" }, pan { "Pan" }, tune { "Tune" }, fine { "Fine" },
         start { "Start" }, end { "End" }, cutoff { "Filter" }, reso { "Reso" };
public:
    EnvelopePanel envelope { ctx };
private:
    juce::ToggleButton reverseToggle { "Reverse" }, monoToggle { "Mono" };
    LayerList layerModel { ctx };
    juce::ListBox layerList { "Layers", &layerModel };
    juce::TextButton upButton { "Up" }, downButton { "Down" }, removeButton { "Remove" }, clearButton { "Clear pad" };
    juce::Rectangle<int> gridArea, panelArea;
    bool refreshing = false;

    Knob* knobs() { return &level; }
    void setupKnob (Knob&, double min, double max, double interval, double skewMid,
                    std::function<juce::String (double)> fmt, std::function<void (PadState&, double)> apply);
};

class PadRomplerEditor : public juce::AudioProcessorEditor,
                         public juce::DragAndDropContainer,
                         public EditorContext,
                         private juce::FileBrowserListener,
                         private juce::Timer
{
public:
    explicit PadRomplerEditor (PadRomplerProcessor&);
    ~PadRomplerEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    // EditorContext
    PadRomplerProcessor& getProc() override { return proc; }
    int getSelectedPad() const override { return selectedPad; }
    void selectPad (int index) override;
    juce::Array<juce::File> browserSelection() const override;
    void dropFiles (int padIndex, const juce::Array<juce::File>& files) override;
    void openInSampler (const juce::File& file) override;
    void tempoPadModesChanged (bool on) override;
    void clearAllRequested() override;

    void showTab (int tab);
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void selectionChanged() override;
    void fileClicked (const juce::File&, const juce::MouseEvent&) override;
    juce::File lastPreviewed;
    juce::uint32 lastPreviewMs = 0;
    void previewNow (const juce::File&);
    void fileDoubleClicked (const juce::File&) override;
    void browserRootChanged (const juce::File&) override {}
    void setBrowserRoot (const juce::File&);

    PadRomplerProcessor& proc;
    int selectedPad = 0;
    int currentTab = 0;

    // browser
    juce::TimeSliceThread scanThread { "FlipKit64 file scanner" };
    juce::WildcardFileFilter audioFilter { "*.wav;*.wave;*.aif;*.aiff;*.flac;*.ogg;*.mp3", "*", "Audio files" };
    juce::DirectoryContentsList dirList { &audioFilter, scanThread };
    juce::FileTreeComponent fileTree { dirList };
    juce::ComboBox placesBox;
    struct Place
    {
        juce::String name;
        juce::File dir;          // a local folder...
        juce::String netKey;     // ...or a network location ("server/share")
        bool isNet() const { return netKey.isNotEmpty(); }
    };
    std::vector<Place> places;                                 // browser locations (saved in settings)
    NetBrowser netBrowser;
    PreviewStrip previewStrip { *this };
    Net::SharedLibrary netLib;
    bool netActive = false;
    juce::String currentNetKey;
    void showNetPlace (const juce::String& key);
    void openNetworkDialog (const Net::Location& prefill, bool editing);
    std::unique_ptr<juce::PropertiesFile> settings;
    void loadPlaces();
    void savePlaces();
    void rebuildPlacesBox();
    juce::ToggleButton previewToggle { "Preview on click" };
    juce::Label browserPath;
    std::unique_ptr<juce::FileChooser> chooser;

    // pages
    PadsPage padsPage { *this };
    SamplerPage samplerPage { *this };
    SequencerPage sequencerPage { *this };
    juce::TextButton tabButtons[3] { juce::TextButton ("PADS"), juce::TextButton ("SAMPLER"), juce::TextButton ("SEQUENCER") };

    // top bar
    juce::TextButton saveKitButton { "Save kit..." }, loadKitButton { "Load kit..." };
    juce::HyperlinkButton supportLink { "Support FlipKit64", juce::URL ("https://ko-fi.com/seandrethegiant") };
    juce::Slider masterSlider;
    juce::Label masterLabel;
    juce::Label ctrlLabel;                 // which TempoPAD mode FlipKit64 is following
    int lastCtrlMode = -1, tabBeforeCtrl = -1;
    void updateCtrlLabel();
    void saveKitAs (std::function<void (bool saved)> then);
    void doClear (int what);               // 1 = all pads, 2 = new kit

    juce::Rectangle<int> browserArea, contentArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadRomplerEditor)
};
