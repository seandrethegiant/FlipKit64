#include "EnvelopeEditor.h"

using namespace juce;
using namespace UI;

// times map to x on a log-like scale, so short attacks and long releases both get room
static float timeFrac (float ms, float lo, float hi) { const float k = 15.0f; return jlimit (0.0f, 1.0f, std::log1p ((ms - lo) / k) / std::log1p ((hi - lo) / k)); }
static float fracTime (float f, float lo, float hi)  { const float k = 15.0f; return lo + k * std::expm1 (jlimit (0.0f, 1.0f, f) * std::log1p ((hi - lo) / k)); }
static String msText (double v) { return v < 1000.0 ? String ((int) std::round (v)) + " ms" : String (v / 1000.0, 2) + " s"; }

// ===================================================================================== EnvelopeGraph

void EnvelopeGraph::refresh()
{
    auto pad = proc.getPad (ctx.getSelectedPad());
    env = pad != nullptr ? *pad : PadState();
    oneShot = env.play == PlayMode::OneShot;
    buildPeaks();
    repaint();
}

void EnvelopeGraph::buildPeaks()
{
    peaks.clear();
    if (env.layers.empty() || env.layers.front() == nullptr || getWidth() < 4) return;
    const auto& a = env.layers.front()->audio;
    const int len = a.getNumSamples();
    const int s0 = (int) (jlimit (0.0f, 1.0f, env.start) * len), s1 = (int) (jlimit (0.0f, 1.0f, env.end) * len);
    const int w = getWidth() - 12;
    if (s1 - s0 < w || w <= 0) return;
    peaks.resize ((size_t) w);
    float top = 1.0e-6f;
    for (int x = 0; x < w; ++x)
    {
        const int i0 = s0 + (int) ((int64) (s1 - s0) * x / w), i1 = s0 + (int) ((int64) (s1 - s0) * (x + 1) / w);
        float m = 0.0f;
        for (int c = 0; c < a.getNumChannels(); ++c) m = jmax (m, a.getMagnitude (c, i0, jmax (1, i1 - i0)));
        peaks[(size_t) x] = m;
        top = jmax (top, m);
    }
    for (auto& p : peaks) p /= top;
    if (env.reverse) std::reverse (peaks.begin(), peaks.end());
}

EnvelopeGraph::Geo EnvelopeGraph::geo() const
{
    Geo g;
    const float W = (float) getWidth();
    g.left = 8.0f; g.top = 14.0f; g.bot = (float) getHeight() - 10.0f;
    const float span = W - 14.0f;
    g.wA = span * 0.3f; g.wD = span * 0.3f;
    g.xA = g.left + g.wA * timeFrac (env.attackMs, 0.0f, maxA);
    g.xD = g.xA + g.wD * timeFrac (env.decayMs, minD, maxD);
    g.offX = jmax (g.xD + 18.0f, g.left + span * 0.68f);
    g.wR = jmax (1.0f, W - 6.0f - g.offX);
    g.xR = g.offX + g.wR * timeFrac (env.releaseMs, minR, maxR);
    g.yS = g.bot - jlimit (0.0f, 1.0f, env.sustain) * (g.bot - g.top);
    return g;
}

Path EnvelopeGraph::segment (Point<float> p0, Point<float> p1, float curve) const
{
    Path p;
    p.startNewSubPath (p0);
    for (int i = 1; i <= 32; ++i)
    {
        const float t = (float) i / 32.0f;
        p.lineTo (p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * envShape (t, curve));
    }
    return p;
}

Point<float> EnvelopeGraph::handle (Part part) const
{
    const auto g = geo();
    const Point<float> P0 (g.left, g.bot), PA (g.xA, g.top), PD (g.xD, g.yS), PO (g.offX, g.yS), PR (g.xR, g.bot);
    auto mid = [] (Point<float> a, Point<float> b, float c) { return Point<float> ((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * envShape (0.5f, c)); };
    switch (part)
    {
        case pA: return PA;
        case pD: return PD;
        case pR: return PR;
        case cA: return mid (P0, PA, env.curveA);
        case cD: return mid (PA, PD, env.curveD);
        case cR: return mid (PO, PR, env.curveR);
        default: return {};
    }
}

EnvelopeGraph::Part EnvelopeGraph::partAt (Point<float> p) const
{
    Part best = none;
    float bestD = 11.0f;
    for (Part part : { pA, pD, pR, cA, cD, cR })
    {
        if ((part == cD && env.sustain >= 0.999f)) continue;          // flat decay: nothing to bend
        const float d = handle (part).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = part; }
    }
    return best;
}

void EnvelopeGraph::paint (Graphics& g)
{
    g.setColour (bg.brighter (0.04f));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    const auto G = geo();
    const float mid = (G.top + G.bot) * 0.5f, amp = (G.bot - G.top) * 0.5f;

    if (! peaks.empty())                                         // the sound itself, faintly
    {
        Path w;
        w.startNewSubPath (6.0f, mid);
        for (size_t x = 0; x < peaks.size(); ++x) w.lineTo (6.0f + (float) x, mid - peaks[x] * amp);
        for (size_t x = peaks.size(); x-- > 0;) w.lineTo (6.0f + (float) x, mid + peaks[x] * amp);
        w.closeSubPath();
        g.setColour (line.withAlpha (0.7f));
        g.fillPath (w);
    }

    // let-go line
    {
        const float dl[] = { 3.0f, 3.0f };
        g.setColour (line.brighter (0.4f));
        g.drawDashedLine ({ G.offX, 8.0f, G.offX, G.bot + 4.0f }, dl, 2, 1.0f);
        g.setColour (muted); g.setFont (Font (9.5f));
        g.drawText (oneShot ? "let go (not one-shot)" : "let go", (int) G.offX + 4, 3, getWidth() - (int) G.offX - 6, 12, Justification::centredLeft);
    }

    const Point<float> P0 (G.left, G.bot), PA (G.xA, G.top), PD (G.xD, G.yS), PO (G.offX, G.yS), PR (G.xR, G.bot);
    Path envPath = segment (P0, PA, env.curveA);
    envPath.addPath (segment (PA, PD, env.curveD));
    Path rel = segment (PO, PR, env.curveR);
    Path full (envPath); full.lineTo (PO); full.addPath (rel);

    Path fill (full);
    fill.lineTo (P0); fill.closeSubPath();
    g.setColour (accent.withAlpha (0.16f)); g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (envPath, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));
    g.drawLine (PD.x, PD.y, PO.x, PO.y, 2.2f);
    g.setColour (oneShot ? accent.withAlpha (0.45f) : accent);
    g.strokePath (rel, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));

    for (Part part : { cA, cD, cR })                             // bend diamonds
    {
        if (part == cD && env.sustain >= 0.999f) continue;
        const auto c = handle (part);
        Path d; d.addRectangle (-4.5f, -4.5f, 9.0f, 9.0f);
        d.applyTransform (AffineTransform::rotation (MathConstants<float>::pi * 0.25f).translated (c));
        g.setColour (bg); g.fillPath (d);
        g.setColour (part == hover || part == drag ? text : accent); g.strokePath (d, PathStrokeType (1.5f));
    }
    for (Part part : { pA, pD, pR })                             // points
    {
        const auto c = handle (part);
        const float r = (part == hover || part == drag) ? 7.0f : 6.0f;
        g.setColour (text); g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);
        g.setColour (accent); g.drawEllipse (c.x - r, c.y - r, r * 2, r * 2, 2.0f);
    }

    if (dotMs >= 0.0)                                            // where "Hear it" is
    {
        const double a = env.attackMs, d = env.sustain < 0.999f ? env.decayMs : 0.0, off = dotOffMs;
        Point<float> p;
        if (dotMs < a)               { const float t = (float) (dotMs / jmax (1.0, a)); p = { P0.x + (PA.x - P0.x) * t, P0.y + (PA.y - P0.y) * envShape (t, env.curveA) }; }
        else if (dotMs < a + d)      { const float t = (float) ((dotMs - a) / jmax (1.0, d)); p = { PA.x + (PD.x - PA.x) * t, PA.y + (PD.y - PA.y) * envShape (t, env.curveD) }; }
        else if (dotMs < off)        { const float t = (float) jlimit (0.0, 1.0, (dotMs - a - d) / jmax (1.0, off - a - d)); p = { PD.x + (PO.x - PD.x) * t, G.yS }; }
        else                         { const float t = (float) jlimit (0.0, 1.0, (dotMs - off) / jmax (1.0f, env.releaseMs)); p = { PO.x + (PR.x - PO.x) * t, PO.y + (PR.y - PO.y) * envShape (t, env.curveR) }; }
        g.setColour (zoneCyan);
        g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
    }
}

void EnvelopeGraph::apply (const std::function<void (PadState&)>& change)
{
    change (env);
    const auto e = env;
    proc.updatePad (ctx.getSelectedPad(), [e] (PadState& p)
    {
        p.attackMs = e.attackMs; p.decayMs = e.decayMs; p.sustain = e.sustain; p.releaseMs = e.releaseMs;
        p.curveA = e.curveA; p.curveD = e.curveD; p.curveR = e.curveR;
    });
    repaint();
    if (onChange) onChange();
}

void EnvelopeGraph::mouseMove (const MouseEvent& e)
{
    const auto h = partAt (e.position);
    if (h != hover) { hover = h; repaint(); }
    setMouseCursor (h == pD ? MouseCursor::DraggingHandCursor : h == pA || h == pR ? MouseCursor::LeftRightResizeCursor
                    : h != none ? MouseCursor::UpDownResizeCursor : MouseCursor::NormalCursor);
}

void EnvelopeGraph::mouseDown (const MouseEvent& e)
{
    drag = partAt (e.position);
    dragY0 = e.position.y;
    curve0 = drag == cA ? env.curveA : drag == cD ? env.curveD : env.curveR;
}

void EnvelopeGraph::mouseDrag (const MouseEvent& e)
{
    if (drag == none) return;
    const auto G = geo();
    const float x = e.position.x, y = e.position.y;
    switch (drag)
    {
        case pA: apply ([&] (PadState& p) { p.attackMs = std::round (fracTime ((x - G.left) / G.wA, 0.0f, maxA)); }); break;
        case pD: apply ([&] (PadState& p)
                 {
                     p.decayMs = std::round (fracTime ((x - G.xA) / G.wD, minD, maxD));
                     p.sustain = jlimit (0.0f, 1.0f, (G.bot - y) / (G.bot - G.top));
                     if (p.sustain > 0.985f) p.sustain = 1.0f;          // snap: plays the whole sound
                     if (p.sustain < 0.015f) p.sustain = 0.0f;
                 }); break;
        case pR: apply ([&] (PadState& p) { p.releaseMs = std::round (fracTime ((x - G.offX) / G.wR, minR, maxR)); }); break;
        default:
        {
            // the diamond follows the mouse: find the curve whose midpoint is at this height
            const Point<float> P0 (G.left, G.bot), PA (G.xA, G.top), PD (G.xD, G.yS), PO (G.offX, G.yS), PR (G.xR, G.bot);
            const auto a = drag == cA ? P0 : drag == cD ? PA : PO;
            const auto b = drag == cA ? PA : drag == cD ? PD : PR;
            float c;
            if (std::abs (b.y - a.y) < 4.0f)
                c = jlimit (-1.0f, 1.0f, curve0 + (y - dragY0) / 40.0f);
            else
            {
                const float want = jlimit (0.0f, 1.0f, (y - a.y) / (b.y - a.y));
                float lo = -1.0f, hi = 1.0f;
                for (int i = 0; i < 24; ++i) { const float m = (lo + hi) * 0.5f; (envShape (0.5f, m) < want ? lo : hi) = m; }
                c = (lo + hi) * 0.5f;
                if (std::abs (c) < 0.04f) c = 0.0f;
            }
            apply ([&] (PadState& p) { (drag == cA ? p.curveA : drag == cD ? p.curveD : p.curveR) = c; });
        }
    }
}

void EnvelopeGraph::mouseUp (const MouseEvent&) { drag = none; repaint(); }

void EnvelopeGraph::mouseDoubleClick (const MouseEvent& e)
{
    const PadState d;
    switch (partAt (e.position))
    {
        case pA: apply ([&] (PadState& p) { p.attackMs = d.attackMs; }); break;
        case pD: apply ([&] (PadState& p) { p.decayMs = d.decayMs; p.sustain = d.sustain; }); break;
        case pR: apply ([&] (PadState& p) { p.releaseMs = d.releaseMs; }); break;
        case cA: apply ([&] (PadState& p) { p.curveA = d.curveA; }); break;
        case cD: apply ([&] (PadState& p) { p.curveD = d.curveD; }); break;
        case cR: apply ([&] (PadState& p) { p.curveR = d.curveR; }); break;
        default: break;
    }
}

// ===================================================================================== EnvelopePanel

EnvelopePanel::EnvelopePanel (EditorContext& c) : graph (c), ctx (c), proc (c.getProc())
{
    addAndMakeVisible (graph);
    graph.onChange = [this] { refresh(); };
    for (auto* b : { &pluckButton, &keysButton, &swellButton, &hearButton, &resetButton }) { styleButton (*b); addAndMakeVisible (b); }
    hearButton.setColour (TextButton::buttonColourId, accent);
    hearButton.setColour (TextButton::textColourOffId, Colours::black);
    pluckButton.onClick = [this] { preset (2, 180, 0.0f, 120, 0.0f, 0.8f, 0.6f); };
    keysButton.onClick  = [this] { preset (10, 600, 0.6f, 900, 0.0f, 0.6f, 0.6f); };
    swellButton.onClick = [this] { preset (900, 200, 0.9f, 1600, -0.6f, 0.0f, 0.3f); };
    pluckButton.setTooltip ("Short and snappy: fades to silence");
    keysButton.setTooltip ("Piano-like: falls to a held level, rings out when let go");
    swellButton.setTooltip ("Fades in slowly, long tail");
    hearButton.setTooltip ("Play the pad, let go after a moment, and follow the envelope");
    resetButton.setTooltip ("Back to plain: no fade in, plays the whole sound");
    hearButton.onClick = [this] { hearIt(); };
    resetButton.onClick = [this]
    {
        proc.updatePad (ctx.getSelectedPad(), [] (PadState& p) { p.resetEnvelope(); });
        graph.refresh(); refresh();
    };
    for (auto& v : values)
    {
        v.setJustificationType (Justification::centred);
        v.setColour (Label::backgroundColourId, panel.brighter (0.06f));
        v.setColour (Label::textColourId, text);
        v.setFont (Font (10.5f));
        addAndMakeVisible (v);
    }
}

void EnvelopePanel::preset (float a, float d, float s, float r, float ca, float cd, float cr)
{
    proc.updatePad (ctx.getSelectedPad(), [=] (PadState& p)
    {
        p.attackMs = a; p.decayMs = d; p.sustain = s; p.releaseMs = r; p.curveA = ca; p.curveD = cd; p.curveR = cr;
    });
    graph.refresh();
    refresh();
}

void EnvelopePanel::refresh()
{
    auto pad = proc.getPad (ctx.getSelectedPad());
    if (pad == nullptr) return;
    values[0].setText ("A " + msText (pad->attackMs), dontSendNotification);
    values[1].setText ("D " + (pad->sustain >= 0.999f ? String ("-") : msText (pad->decayMs)), dontSendNotification);
    values[2].setText ("S " + String ((int) std::round (pad->sustain * 100.0f)) + "%", dontSendNotification);
    values[3].setText ("R " + msText (pad->releaseMs), dontSendNotification);
}

void EnvelopePanel::hearIt()
{
    stopHearing();
    auto pad = proc.getPad (ctx.getSelectedPad());
    if (pad == nullptr || pad->layers.empty()) return;
    hearPad = ctx.getSelectedPad();
    const double d = pad->sustain < 0.999f ? pad->decayMs : 0.0;
    hearOff = jmax (500.0, pad->attackMs + d + 400.0);
    hearEnd = hearOff + pad->releaseMs + 60.0;
    hearStart = Time::getMillisecondCounterHiRes();
    released = false;
    proc.triggerPad (hearPad, 0.85f, true);
    graph.dotOffMs = hearOff;
    graph.dotMs = 0.0;
    hearButton.setButtonText ("Playing...");
    startTimerHz (40);
}

void EnvelopePanel::stopHearing()
{
    stopTimer();
    if (hearPad >= 0 && ! released) proc.auditionRelease (hearPad);
    hearPad = -1;
    graph.dotMs = -1.0;
    graph.repaint();
    hearButton.setButtonText ("Hear it");
}

void EnvelopePanel::timerCallback()
{
    const double t = Time::getMillisecondCounterHiRes() - hearStart;
    if (! released && t >= hearOff) { proc.auditionRelease (hearPad); released = true; }
    if (t >= hearEnd || hearPad != ctx.getSelectedPad()) { released = true; stopHearing(); return; }
    graph.dotMs = t;
    graph.repaint();
}

void EnvelopePanel::paint (Graphics& g)
{
    g.setColour (muted);
    g.setFont (Font (11.0f, Font::bold));
    g.drawText ("ENVELOPE", getLocalBounds().removeFromTop (20), Justification::centredLeft);
}

void EnvelopePanel::resized()
{
    auto r = getLocalBounds();
    auto hdr = r.removeFromTop (20);
    hdr.removeFromLeft (66);
    const int vw = (hdr.getWidth() - 9) / 4;
    for (int i = 0; i < 4; ++i) { values[i].setBounds (hdr.removeFromLeft (vw).reduced (0, 1)); if (i < 3) hdr.removeFromLeft (3); }
    r.removeFromTop (4);
    auto bottom = r.removeFromBottom (24);
    resetButton.setBounds (bottom.removeFromRight (48)); bottom.removeFromRight (4);
    hearButton.setBounds (bottom.removeFromRight (66)); bottom.removeFromRight (8);
    const int bw = (bottom.getWidth() - 8) / 3;
    for (auto* b : { &pluckButton, &keysButton, &swellButton }) { b->setBounds (bottom.removeFromLeft (bw)); bottom.removeFromLeft (4); }
    r.removeFromBottom (4);
    graph.setBounds (r);
    graph.refresh();
}
