#pragma once
#include "Common.h"

// Player strip under the sample browser: the previewed file's waveform (click or drag to jump),
// play/pause, stop, loop, and "Chop it" to open it in the Sampler.
class PreviewStrip : public juce::Component
{
public:
    explicit PreviewStrip (EditorContext&);

    void tick();                 // called ~30x a second by the editor
    void togglePlay();           // Space
    void stop();                 // Esc

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static constexpr int preferredHeight = 112;

private:
    class RoundButton : public juce::Button
    {
    public:
        enum Kind { playPause, stopKind };
        RoundButton (Kind k) : juce::Button (k == playPause ? "Play" : "Stop"), kind (k) {}
        void paintButton (juce::Graphics&, bool over, bool down) override;
        Kind kind;
        bool showPause = false;
    };

    void rebuildPeaks();
    void seekTo (int x);
    juce::String timeText() const;

    EditorContext& ctx;
    PadRomplerProcessor& proc;
    SamplePtr shown;
    std::vector<float> peaks;
    juce::Rectangle<int> waveArea, nameArea;
    double lastPos = -1.0;
    bool lastPlaying = false;

    RoundButton playButton { RoundButton::playPause }, stopButton { RoundButton::stopKind };
    juce::TextButton loopButton { "Loop" }, chopButton { "Chop it" };
    juce::Label nameLabel, timeLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreviewStrip)
};
