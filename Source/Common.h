#pragma once
#include "PluginProcessor.h"

namespace UI
{
    // Purple / orange / white theme
    const juce::Colour bg      { 0xff150f22 };   // deep purple-black
    const juce::Colour panel   { 0xff211836 };
    const juce::Colour line    { 0xff3a2c58 };
    const juce::Colour text    { 0xfff6f3fb };   // near white
    const juce::Colour muted   { 0xff9d92b8 };
    const juce::Colour left    { 0xffff7a1f };   // left half of the pad grid (notes 36-67): orange
    const juce::Colour right   { 0xffa45cff };   // right half (notes 68-99): purple
    const juce::Colour accent  { 0xffff8c2e };   // orange
    const juce::Colour rec     { 0xffff3d5a };

    // Pad zones, matching the controller's default drum-mode colours (4x4 quadrants).
    const juce::Colour zonePurple { 0xffc84dff };   // notes 52-67 (top left)
    const juce::Colour zoneBlue   { 0xff7389fb };   // notes 84-99 (top right)
    const juce::Colour zoneCyan   { 0xff1ad9fc };   // notes 36-51 (bottom left)
    const juce::Colour zoneYellow { 0xfffee150 };   // notes 68-83 (bottom right)

    inline juce::Colour padHue (int pad)
    {
        const bool leftHalf = (pad % PadLayout::cols) < 4;
        const bool topHalf  = (pad / PadLayout::cols) < 4;
        return leftHalf ? (topHalf ? zonePurple : zoneCyan) : (topHalf ? zoneBlue : zoneYellow);
    }

    inline void styleButton (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, panel.brighter (0.08f));
        b.setColour (juce::TextButton::buttonOnColourId, accent.darker (0.2f));
        b.setColour (juce::TextButton::textColourOffId, text);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    }

    inline void styleLabel (juce::Label& l, float size = 12.0f, juce::Colour c = muted)
    {
        l.setFont (juce::Font (size));
        l.setColour (juce::Label::textColourId, c);
    }

    // Pads in "MPC order": ascending note number (bottom-left pad first, as the TempoPAD sends them).
    inline std::vector<int> padsByNote()
    {
        std::vector<int> v;
        for (int i = 0; i < PadLayout::numPads; ++i) v.push_back (i);
        std::sort (v.begin(), v.end(), [] (int a, int b) { return PadLayout::noteForPad (a) < PadLayout::noteForPad (b); });
        return v;
    }
}

// What the pages need from the main editor.
struct EditorContext
{
    virtual ~EditorContext() = default;
    virtual PadRomplerProcessor& getProc() = 0;
    virtual int getSelectedPad() const = 0;
    virtual void selectPad (int index) = 0;
    virtual juce::Array<juce::File> browserSelection() const = 0;
    virtual void dropFiles (int padIndex, const juce::Array<juce::File>& files) = 0;
    virtual void openInSampler (const juce::File& file) = 0;
    static constexpr const char* dragId = "PadRomplerFiles";
};

// Small labelled rotary knob.
class Knob : public juce::Component
{
public:
    Knob (const juce::String& name)
    {
        label.setText (name, juce::dontSendNotification);
        UI::styleLabel (label, 11.0f, UI::text.withAlpha (0.75f));
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        slider.setColour (juce::Slider::rotarySliderFillColourId, UI::accent);
        slider.setColour (juce::Slider::rotarySliderOutlineColourId, UI::line);
        slider.setColour (juce::Slider::thumbColourId, UI::text);
        slider.setColour (juce::Slider::textBoxTextColourId, UI::text);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (slider);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (15));
        slider.setBounds (r);
    }
    juce::Label label;
    juce::Slider slider;
};
