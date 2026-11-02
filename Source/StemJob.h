#pragma once
#include "PluginProcessor.h"
#include "StemSeparator.h"

// ---------------------------------------------------------------------------------------------
// One "split into stems" run for the UI: downloads the model if it isn't there yet, then separates,
// on a background thread, with a progress window (and Cancel) on screen. It owns itself and
// reports back on the message thread. Only one runs at a time.
//
// The four stems are written as 24-bit WAVs into  Recordings/Edits/<name> stems/ .
// ---------------------------------------------------------------------------------------------
class StemJob : private juce::Thread, private juce::Timer
{
public:
    // error is empty on success, "cancelled" if the user cancelled, otherwise a message to show.
    using Done = std::function<void (const juce::String& error, const juce::File& folder,
                                     const juce::Array<juce::File>& stems)>;

    static bool busy();
    static void start (const SamplePtr& source, Done onDone);   // message thread only
    static void cancelAndWait();                                 // editor closing: stop without a callback

private:
    StemJob (const SamplePtr&, Done);
    ~StemJob() override;
    void run() override;
    void timerCallback() override;
    void finish (bool callDone);
    bool progress (float f);

    SamplePtr source;
    Done done;
    juce::Component::SafePointer<juce::AlertWindow> window;

    juce::CriticalSection lock;          // guards phaseText
    juce::String phaseText;
    std::atomic<float> fraction { 0.0f };
    std::shared_ptr<std::atomic<bool>> cancelRequested { std::make_shared<std::atomic<bool>> (false) };   // shared with the window's callback, which can outlive this object

    juce::String error;
    juce::File outFolder;
    juce::Array<juce::File> outFiles;
    juce::String shownPhase;

    JUCE_DECLARE_NON_COPYABLE (StemJob)
};
