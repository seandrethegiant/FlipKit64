#include "PreviewStrip.h"

using namespace juce;
using namespace UI;

void PreviewStrip::RoundButton::paintButton (Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const auto c = r.getCentre();
    const bool enabled = isEnabled();
    if (kind == playPause)
    {
        g.setColour ((enabled ? accent : line).brighter (down ? -0.1f : over ? 0.12f : 0.0f));
        g.fillEllipse (r);
        g.setColour (bg);
        if (showPause)
        {
            g.fillRoundedRectangle (c.x - 6.0f, c.y - 7.0f, 4.0f, 14.0f, 1.0f);
            g.fillRoundedRectangle (c.x + 2.0f, c.y - 7.0f, 4.0f, 14.0f, 1.0f);
        }
        else
        {
            Path p;
            p.addTriangle (c.x - 4.5f, c.y - 7.5f, c.x - 4.5f, c.y + 7.5f, c.x + 7.5f, c.y);
            g.fillPath (p);
        }
    }
    else
    {
        g.setColour (panel.brighter (down ? 0.02f : over ? 0.14f : 0.08f));
        g.fillEllipse (r);
        g.setColour (line.brighter (0.2f));
        g.drawEllipse (r, 1.0f);
        g.setColour (enabled ? UI::text : UI::muted);
        g.fillRoundedRectangle (c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f, 1.5f);
    }
}

PreviewStrip::PreviewStrip (EditorContext& c) : ctx (c), proc (c.getProc())
{
    playButton.setTooltip ("Play / pause the preview (Space)");
    stopButton.setTooltip ("Stop and go back to the start (Esc)");
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { stop(); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);

    for (auto* b : { &loopButton, &chopButton }) { styleButton (*b); addAndMakeVisible (b); }
    loopButton.setClickingTogglesState (true);
    loopButton.setTooltip ("Loop the preview");
    loopButton.onClick = [this] { proc.playerSetLoop (loopButton.getToggleState()); };
    chopButton.setTooltip ("Open this sample in the Sampler to chop it");
    chopButton.onClick = [this]
    {
        if (shown == nullptr) return;
        proc.playerStop();
        ctx.openInSampler (shown->file);
    };

    styleLabel (nameLabel, 12.0f, text);
    nameLabel.setFont (Font (12.0f, Font::bold));
    nameLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (nameLabel);
    styleLabel (timeLabel, 11.0f);
    timeLabel.setJustificationType (Justification::centredRight);
    addAndMakeVisible (timeLabel);
    nameLabel.setText ("Click a sample to hear it", dontSendNotification);
    nameLabel.setColour (Label::textColourId, muted);
    tick();
}

void PreviewStrip::rebuildPeaks()
{
    peaks.assign (200, 0.0f);
    if (shown == nullptr) return;
    const auto& a = shown->audio;
    const int len = a.getNumSamples();
    if (len <= 0) return;
    const float* l = a.getReadPointer (0);
    const float* r = a.getReadPointer (1);
    const int n = (int) peaks.size();
    for (int b = 0; b < n; ++b)
    {
        const int s0 = (int) ((int64) len * b / n), s1 = jmax (s0 + 1, (int) ((int64) len * (b + 1) / n));
        const int stride = jmax (1, (s1 - s0) / 256);            // fast enough for long records
        float m = 0.0f;
        for (int i = s0; i < s1; i += stride) m = jmax (m, std::abs (l[i]), std::abs (r[i]));
        peaks[(size_t) b] = m;
    }
    const float top = jmax (0.05f, *std::max_element (peaks.begin(), peaks.end()));
    for (auto& p : peaks) p = std::sqrt (p / top);               // easier to read for quiet tracks
}

String PreviewStrip::timeText() const
{
    if (shown == nullptr) return {};
    const double len = shown->audio.getNumSamples() / shown->sampleRate;
    const double now = proc.playerPositionFraction() * len;
    if (len < 60.0)
        return String (now, 1) + " / " + String (len, 1) + " s";
    auto fmt = [] (double s) { const int m = (int) s / 60, r = (int) s % 60; return String (m) + ":" + String (r).paddedLeft ('0', 2); };
    return fmt (now) + " / " + fmt (len);
}

void PreviewStrip::tick()
{
    auto s = proc.playerSampleRef();
    if (s != shown)
    {
        shown = s;
        rebuildPeaks();
        nameLabel.setText (shown != nullptr ? shown->file.getFileName() : String ("Click a sample to hear it"), dontSendNotification);
        nameLabel.setColour (Label::textColourId, shown != nullptr ? text : muted);
        repaint();
    }
    const bool playing = proc.playerIsPlaying();
    const double pos = proc.playerPositionFraction();
    playButton.setEnabled (shown != nullptr);
    stopButton.setEnabled (shown != nullptr);
    chopButton.setEnabled (shown != nullptr);
    if (loopButton.getToggleState() != proc.playerLooping()) loopButton.setToggleState (proc.playerLooping(), dontSendNotification);
    if (playing != lastPlaying)
    {
        lastPlaying = playing;
        playButton.showPause = playing;
        playButton.setTooltip (playing ? "Pause the preview (Space)" : "Play the preview (Space)");
        playButton.repaint();
    }
    if (std::abs (pos - lastPos) > 1.0e-5)
    {
        lastPos = pos;
        timeLabel.setText (timeText(), dontSendNotification);
        repaint (waveArea);
    }
}

void PreviewStrip::togglePlay()
{
    if (shown == nullptr) return;
    if (proc.playerIsPlaying()) proc.playerPause(); else proc.playerPlay();
    tick();
}

void PreviewStrip::stop()
{
    proc.playerStop();
    tick();
}

void PreviewStrip::seekTo (int x)
{
    if (shown == nullptr || waveArea.getWidth() <= 0) return;
    proc.playerSeek ((double) (x - waveArea.getX()) / (double) waveArea.getWidth());
    if (! proc.playerIsPlaying()) proc.playerPlay();          // click = play from here
    tick();
}

void PreviewStrip::mouseDown (const MouseEvent& e) { if (waveArea.contains (e.getPosition())) seekTo (e.x); }
void PreviewStrip::mouseDrag (const MouseEvent& e) { if (waveArea.expanded (0, 20).contains (e.getMouseDownPosition())) seekTo (jlimit (waveArea.getX(), waveArea.getRight(), e.x)); }

void PreviewStrip::paint (Graphics& g)
{
    g.setColour (line);
    g.drawHorizontalLine (0, 2.0f, (float) getWidth() - 2.0f);

    auto w = waveArea.toFloat();
    g.setColour (panel.brighter (0.04f));
    g.fillRoundedRectangle (w, 4.0f);
    if (shown == nullptr || peaks.empty()) return;

    const int n = (int) peaks.size();
    const float bw = w.getWidth() / (float) n;
    const float played = (float) proc.playerPositionFraction();
    const float mid = w.getCentreY(), half = w.getHeight() * 0.46f;
    for (int i = 0; i < n; ++i)
    {
        const float h = jmax (1.0f, peaks[(size_t) i] * half);
        g.setColour (((float) i + 0.5f) / (float) n <= played ? accent : right.withAlpha (0.45f));
        g.fillRect (w.getX() + (float) i * bw, mid - h, jmax (1.0f, bw - 0.6f), h * 2.0f);
    }
    const float px = w.getX() + played * w.getWidth();
    g.setColour (text);
    g.fillRect (px - 0.75f, w.getY(), 1.5f, w.getHeight());
}

void PreviewStrip::resized()
{
    auto r = getLocalBounds().reduced (2, 0);
    r.removeFromTop (8);
    auto top = r.removeFromTop (18);
    timeLabel.setBounds (top.removeFromRight (96));
    nameLabel.setBounds (top);
    r.removeFromTop (4);
    waveArea = r.removeFromTop (34);
    r.removeFromTop (8);
    auto row = r.removeFromTop (40);
    playButton.setBounds (row.removeFromLeft (40));
    row.removeFromLeft (8);
    stopButton.setBounds (row.removeFromLeft (40).reduced (3));
    row.removeFromLeft (8);
    loopButton.setBounds (row.removeFromLeft (58).reduced (0, 5));
    chopButton.setBounds (row.removeFromRight (70).reduced (0, 5));
}
