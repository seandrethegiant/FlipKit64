#pragma once
#include "Common.h"

// The selected pad's ADSR envelope, drawn over its waveform. Drag the points (attack peak,
// decay/sustain point, release end) to change the times and sustain level; drag a diamond up or
// down to bend that segment. Double-click a point or diamond to put it back.
class EnvelopeGraph : public juce::Component
{
public:
    explicit EnvelopeGraph (EditorContext& c) : ctx (c), proc (c.getProc()) {}

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    void refresh();                         // the pad (or its settings) changed
    double dotMs = -1.0;                    // "Hear it" position along the envelope, -1 = hidden
    double dotOffMs = 0.0;                  // when "Hear it" lets go
    bool oneShot = false;

    // geometry, public for the panel and tests
    struct Geo { float left, top, bot, wA, wD, xA, xD, offX, xR, yS, wR; };
    Geo geo() const;
    std::function<void()> onChange;

    static constexpr float maxA = 2000.0f, minD = 10.0f, maxD = 5000.0f, minR = 5.0f, maxR = 5000.0f;

private:
    EditorContext& ctx;
    PadRomplerProcessor& proc;
    PadState env;                           // copy of the envelope fields being shown
    std::vector<float> peaks;               // waveform outline (0..1), one per pixel
    enum Part { none = -1, pA, pD, pR, cA, cD, cR };
    Part drag = none, hover = none;
    float dragY0 = 0.0f, curve0 = 0.0f;

    Part partAt (juce::Point<float>) const;
    juce::Point<float> handle (Part) const;
    void apply (const std::function<void (PadState&)>&);
    void buildPeaks();
    juce::Path segment (juce::Point<float> p0, juce::Point<float> p1, float curve) const;
};

// Presets, the graph, the A/D/S/R readout, Hear it and Reset.
class EnvelopePanel : public juce::Component, private juce::Timer
{
public:
    explicit EnvelopePanel (EditorContext&);
    ~EnvelopePanel() override { stopHearing(); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    void hearIt();
    EnvelopeGraph graph;
    juce::TextButton hearButton { "Hear it" }, resetButton { "Reset" };

private:
    void timerCallback() override;
    void stopHearing();
    void preset (float a, float d, float s, float r, float ca, float cd, float cr);
    EditorContext& ctx;
    PadRomplerProcessor& proc;
    juce::TextButton pluckButton { "Pluck" }, keysButton { "Keys" }, swellButton { "Swell" };
    juce::Label values[4];
    double hearStart = 0.0, hearOff = 0.0, hearEnd = 0.0;
    int hearPad = -1;
    bool released = false;
};
