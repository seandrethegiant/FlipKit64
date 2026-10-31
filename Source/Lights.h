#pragma once
#include "PluginProcessor.h"

// TempoPAD pad lights. In its Easy mode the TempoPAD takes Synido's live colour message,
// F0 65 (pad R G B)... F7, pad = note - 36 in FlipKit64's layout, colours 0-127, nothing saved
// (see docs/tempopad-protocol.md). This works out what the pads should show ~30 times a
// second and sends only the pads that changed, plus a full frame every second so the picture
// appears as soon as someone switches the TempoPAD into Easy mode.
namespace Lights
{
    struct RGB { uint8_t r = 0, g = 0, b = 0; bool operator== (const RGB& o) const { return r == o.r && g == o.g && b == o.b; } };
    using Frame = std::array<RGB, PadLayout::numPads>;      // indexed by TempoPAD pad (note - 36)

    enum Layout { layoutPatterns = 0, layoutPads, layoutSteps, numLayouts };
    constexpr int stepsWithPicker = 32;   // steps layout: patterns this short leave the bottom half for picking the sound

    // What the pads show, from the processor's state. `now` in ms drives blinking and flash fades.
    struct Inputs
    {
        int layout = layoutPatterns;
        bool loaded[PadLayout::numPads] {};              // FlipKit64 pad index
        float flash[PadLayout::numPads] {};              // 1 = just hit, fades to 0
        int currentPattern = 0, queuedPattern = -1;
        bool patternUsed[Seq::numPatterns] {};
        int stepPad = 0, stepLength = 16, playStep = -1;
        bool stepOn[Seq::maxSteps] {};
        bool blinkOn = true;

        // Sampler tab open: slices on the pads, tap chop progress
        bool sampler = false;
        int slice[PadLayout::numPads];                   // FlipKit64 pad -> slice number, -1 = none
        bool assigned[PadLayout::numPads] {};            // that slice is already on the pad
        int selectedSlice = -1;
        bool tapping = false;
        float tapProgress = 0.0f;                        // 0..1 through the sample
        bool tapped[PadLayout::numPads] {};              // chopped onto during this tap chop

        // Recording (Sampler's Record): 1 = waiting for sound, 2 = recording; shown on any tab
        int recording = 0;
        float recLevel[PadLayout::cols] {};              // input level 0..1 per column, oldest left, newest right
        float recProgress = 0.0f;                        // 0..1 of the longest recording
        Inputs() { std::fill (std::begin (slice), std::end (slice), -1); }
    };

    // What the Sampler page tells the lights (message thread).
    struct SamplerInfo
    {
        bool active = false;
        int slice[PadLayout::numPads];
        bool assigned[PadLayout::numPads] {};
        int selectedSlice = -1;
        bool tapping = false;
        float tapProgress = 0.0f;
        bool tapped[PadLayout::numPads] {};
        SamplerInfo() { std::fill (std::begin (slice), std::end (slice), -1); }
    };
    Frame render (const Inputs&);
    int meterRows (float level);                            // input level (linear) -> rows lit, 0..7

    // F0 65 (pad R G B)* F7 for the pads that differ from `last` (all pads when `full`), or empty.
    std::vector<uint8_t> encode (const Frame& next, const Frame& last, bool full);

    RGB zoneColour (int padIndex);                          // FlipKit64 pad index -> its quadrant colour
    inline int ledForPad (int padIndex) { return PadLayout::noteForPad (padIndex) - 36; }
}

// Owns the MIDI output to the TempoPAD (direct, or the Chromebook bridge's "FlipKit64 lights" port)
// and drives it from the message thread.
class PadLights : private juce::Timer
{
public:
    explicit PadLights (PadRomplerProcessor&);
    ~PadLights() override;
    juce::String deviceName() const { return openName; }
    std::function<Lights::SamplerInfo()> samplerInfo;      // set by the editor while it's open
    Lights::Inputs gather();                                // public for tests

private:
    void timerCallback() override;
    void findDevice();
    void blackout();
    PadRomplerProcessor& proc;
    std::unique_ptr<juce::MidiOutput> out;
    juce::String openName;
    Lights::Frame last {};
    uint32_t lastHits[PadLayout::numPads] {};
    float flash[PadLayout::numPads] {};
    float recHistory[PadLayout::cols] {};
    float recPeak = 0.0f;
    int ticks = 0;
    bool wasEnabled = false;
};
