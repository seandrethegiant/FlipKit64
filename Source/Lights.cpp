#include "Lights.h"

using namespace juce;

namespace Lights
{
    static RGB rgb (int r, int g, int b) { return { (uint8_t) jlimit (0, 127, r), (uint8_t) jlimit (0, 127, g), (uint8_t) jlimit (0, 127, b) }; }
    static RGB scale (RGB c, float k) { return rgb ((int) std::lround (c.r * k), (int) std::lround (c.g * k), (int) std::lround (c.b * k)); }
    static RGB mix (RGB a, RGB b, float t)
    {
        return rgb ((int) std::lround (a.r + (b.r - a.r) * t), (int) std::lround (a.g + (b.g - a.g) * t), (int) std::lround (a.b + (b.b - a.b) * t));
    }

    // The same four quadrant colours as the screen (and the TempoPAD's own Drum mode), in 0-127.
    RGB zoneColour (int pad)
    {
        const bool leftHalf = (pad % PadLayout::cols) < 4;
        const bool topHalf  = (pad / PadLayout::cols) < 4;
        if (leftHalf) return topHalf ? rgb (100, 38, 127) : rgb (13, 108, 126);   // purple / cyan
        return topHalf ? rgb (57, 68, 125) : rgb (127, 112, 40);                   // blue / yellow
    }

    static const RGB white = rgb (85, 127, 105);       // the TempoPAD's red LEDs run hot: equal RGB looks pink
    static const RGB orange = rgb (127, 56, 0);

    static const RGB red = rgb (127, 0, 0);

    int meterRows (float level)
    {
        if (level <= 0.0f) return 0;
        const float db = 20.0f * std::log10 (level);
        return jlimit (0, PadLayout::rows - 1, (int) std::ceil ((db + 48.0f) / 48.0f * (PadLayout::rows - 1)));   // -48 dB .. 0 dB
    }

    static Frame renderRecording (const Inputs& in)
    {
        Frame f {};
        const int meterH = PadLayout::rows - 1;                 // rows 1-7 (the top row is the time bar)
        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            const int row = pad / PadLayout::cols, col = pad % PadLayout::cols;
            RGB c;
            if (row == 0)
            {
                if (in.recording == 2)                           // recording time used so far
                {
                    const float x = in.recProgress * PadLayout::cols;
                    c = col < (int) x ? scale (red, 0.5f) : (col == (int) x ? (in.blinkOn ? red : scale (red, 0.3f)) : RGB {});
                }
                else c = in.blinkOn ? scale (red, 0.35f) : scale (red, 0.1f);   // armed: the top row pulses
            }
            else
            {
                const int fromBottom = PadLayout::rows - 1 - row;    // 0 = bottom row
                const int lit = meterRows (in.recLevel[col]);
                if (fromBottom < lit)
                {
                    const float t = (float) fromBottom / (float) (meterH - 1);
                    c = t < 0.55f ? rgb (13, 108, 126) : t < 0.85f ? rgb (127, 112, 40) : red;   // cyan, yellow, red near the top
                    if (in.recording == 1) c = scale (c, 0.5f);       // waiting: the meter is dimmer
                }
                else c = scale (red, in.recording == 1 ? 0.06f : 0.03f);   // dim red background
            }
            f[(size_t) ledForPad (pad)] = c;
        }
        return f;
    }

    Frame render (const Inputs& in)
    {
        if (in.recording > 0) return renderRecording (in);
        Frame f {};
        auto padColour = [&] (int pad)
        {
            const RGB base = in.loaded[pad] ? scale (zoneColour (pad), 0.3f) : RGB {};
            // a hit lights the pad up in its own colour (white turned busy patterns into a white blur)
            return in.flash[pad] > 0.01f ? mix (base, zoneColour (pad), jlimit (0.0f, 1.0f, in.flash[pad])) : base;
        };
        if (in.sampler)
        {
            for (int pad = 0; pad < PadLayout::numPads; ++pad)
            {
                const int row = pad / PadLayout::cols, col = pad % PadLayout::cols;
                RGB c;
                if (in.tapping && row == 0)                       // progress bar across the top row
                {
                    const float x = in.tapProgress * PadLayout::cols;
                    c = col < (int) x ? scale (orange, 0.35f) : (col == (int) x ? white : RGB {});
                }
                else if (in.tapping)
                    c = in.tapped[pad] ? zoneColour (pad) : (in.loaded[pad] ? scale (zoneColour (pad), 0.06f) : RGB {});
                else if (in.slice[pad] >= 0)                      // the slice map
                    c = in.slice[pad] == in.selectedSlice ? zoneColour (pad)
                                                          : scale (zoneColour (pad), in.assigned[pad] ? 0.3f : 0.08f);
                if (in.flash[pad] > 0.01f) c = mix (c, zoneColour (pad), jlimit (0.0f, 1.0f, in.flash[pad]));
                f[(size_t) ledForPad (pad)] = c;
            }
            return f;
        }
        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            RGB c;
            const int row = pad / PadLayout::cols, col = pad % PadLayout::cols;
            switch (in.layout)
            {
                case layoutPatterns:
                    if (row == 0)                                  // top row = patterns 1-8
                    {
                        const int p = col;
                        if (p == in.currentPattern)      c = orange;
                        else if (p == in.queuedPattern)  c = in.blinkOn ? orange : scale (orange, 0.15f);
                        else if (in.patternUsed[p])      c = scale (orange, 0.12f);
                        else                             c = {};                       // empty pattern: off
                    }
                    else c = padColour (pad);
                    break;

                case layoutSteps:                                  // the pads read like a page: top-left = step 1
                {
                    // Patterns up to 32 steps fit in the top four rows; then the bottom four rows pick the sound.
                    const bool picker = in.stepLength <= stepsWithPicker && row >= PadLayout::rows / 2;
                    if (picker)
                    {
                        c = padColour (pad);
                        if (pad == in.stepPad && in.flash[pad] < 0.05f) c = zoneColour (pad);    // the sound being programmed: full colour
                        break;
                    }
                    const int step = row * PadLayout::cols + col;
                    const RGB hue = zoneColour (in.stepPad);
                    if (step >= in.stepLength)        c = {};
                    else if (step == in.playStep)     c = white;
                    else if (in.stepOn[step])         c = hue;
                    else                              c = scale (hue, step % 4 == 0 ? 0.06f : 0.025f);
                    break;
                }

                default: c = padColour (pad); break;
            }
            f[(size_t) ledForPad (pad)] = c;
        }
        return f;
    }

    std::vector<uint8_t> encode (const Frame& next, const Frame& last, bool full)
    {
        std::vector<uint8_t> out { 0xF0, 0x65 };
        for (int i = 0; i < PadLayout::numPads; ++i)
            if (full || ! (next[(size_t) i] == last[(size_t) i]))
            {
                const auto& c = next[(size_t) i];
                out.insert (out.end(), { (uint8_t) i, (uint8_t) (c.r & 0x7F), (uint8_t) (c.g & 0x7F), (uint8_t) (c.b & 0x7F) });
            }
        if (out.size() == 2) return {};
        out.push_back (0xF7);
        return out;
    }
}

// ===================================================================================== PadLights

PadLights::PadLights (PadRomplerProcessor& p) : proc (p)
{
    for (int i = 0; i < PadLayout::numPads; ++i) lastHits[i] = proc.hitCounter[i].load();
    startTimerHz (30);
}

PadLights::~PadLights()
{
    stopTimer();
    if (out != nullptr && wasEnabled) blackout();
}

void PadLights::findDevice()
{
    const auto devices = MidiOutput::getAvailableDevices();
    if (out != nullptr)
    {
        for (const auto& d : devices) if (d.name == openName) return;      // still there
        out.reset(); openName = {};
    }
    const MidiDeviceInfo* pick = nullptr;
    for (const auto& d : devices) if (d.name.containsIgnoreCase ("FlipKit64 lights")) { pick = &d; break; }   // Chromebook bridge
    if (pick == nullptr)
        for (const auto& d : devices)
            if (d.name.containsIgnoreCase ("TempoPAD") || d.name.containsIgnoreCase ("Synido")) { pick = &d; break; }
    if (pick == nullptr) return;
    out = MidiOutput::openDevice (pick->identifier);
    if (out != nullptr) { openName = pick->name; last = {}; ticks = 0; }
}

void PadLights::blackout()
{
    const Lights::Frame dark {};
    auto bytes = Lights::encode (dark, dark, true);
    if (out != nullptr && bytes.size() > 2) out->sendMessageNow (MidiMessage::createSysExMessage (bytes.data() + 1, (int) bytes.size() - 2));
    last = dark;
}

Lights::Inputs PadLights::gather()
{
    Lights::Inputs in;
    in.layout = jlimit (0, Lights::numLayouts - 1, proc.easyLayout.load());
    for (int i = 0; i < PadLayout::numPads; ++i)
    {
        auto p = proc.getPad (i);
        in.loaded[i] = p != nullptr && ! p->layers.empty();
        const auto h = proc.hitCounter[i].load (std::memory_order_relaxed);
        if (h != lastHits[i]) { lastHits[i] = h; flash[i] = 1.0f; }
        in.flash[i] = flash[i];
    }
    in.currentPattern = jlimit (0, Seq::numPatterns - 1, proc.currentPattern.load());
    in.queuedPattern = proc.queuedPattern.load();
    for (int p = 0; p < Seq::numPatterns; ++p)
    {
        const auto& pat = proc.patterns[p];
        bool used = false;
        for (int pad = 0; pad < PadLayout::numPads && ! used; ++pad)
            for (int s = 0; s < Seq::maxSteps; ++s)
                if (pat.vel[pad][s].load (std::memory_order_relaxed) > 0) { used = true; break; }
        for (int t = 0; t < Keys::numTracks && ! used; ++t) used = pat.lanes[t].get() != nullptr;
        in.patternUsed[p] = used;
    }
    const auto& cur = proc.patterns[in.currentPattern];
    in.stepPad = jlimit (0, PadLayout::numPads - 1, proc.stepTarget.load());
    in.stepLength = jlimit (1, Seq::maxSteps, cur.length.load());
    in.playStep = proc.playStep.load();
    for (int s = 0; s < Seq::maxSteps; ++s) in.stepOn[s] = cur.vel[in.stepPad][s].load (std::memory_order_relaxed) > 0;
    in.blinkOn = (Time::getMillisecondCounter() / 250) % 2 == 0;
    const int rs = proc.recState.load();
    if (rs == PadRomplerProcessor::recArmed || rs == PadRomplerProcessor::recRecording)
    {
        in.recording = rs == PadRomplerProcessor::recArmed ? 1 : 2;
        recPeak = jmax (recPeak, proc.recInputLevel.load());
        if (ticks % 3 == 0)                                    // scroll the meter ~10 times a second
        {
            std::copy (std::begin (recHistory) + 1, std::end (recHistory), std::begin (recHistory));
            recHistory[PadLayout::cols - 1] = recPeak;
            recPeak = 0.0f;
        }
        std::copy (std::begin (recHistory), std::end (recHistory), std::begin (in.recLevel));
        in.recLevel[PadLayout::cols - 1] = jmax (in.recLevel[PadLayout::cols - 1], proc.recInputLevel.load());
        in.recProgress = jlimit (0.0f, 1.0f, (float) proc.recPos.load() / (float) jmax (1, proc.recMax.load()));
    }
    else std::fill (std::begin (recHistory), std::end (recHistory), 0.0f);
    if (samplerInfo)
    {
        const auto si = samplerInfo();
        if (si.active)
        {
            in.sampler = true;
            std::copy (std::begin (si.slice), std::end (si.slice), std::begin (in.slice));
            std::copy (std::begin (si.assigned), std::end (si.assigned), std::begin (in.assigned));
            std::copy (std::begin (si.tapped), std::end (si.tapped), std::begin (in.tapped));
            in.selectedSlice = si.selectedSlice;
            in.tapping = si.tapping;
            in.tapProgress = si.tapProgress;
        }
    }
    return in;
}

void PadLights::timerCallback()
{
    const bool on = proc.padLights.load();
    if (! on)
    {
        if (wasEnabled) blackout();
        wasEnabled = false;
        return;
    }
    if (ticks % 60 == 0) findDevice();                    // every 2 s: (re)connect, notice unplugging
    const auto in = gather();
    for (auto& f : flash) f *= 0.72f;                     // hits fade over ~100 ms
    const auto frame = Lights::render (in);
    const bool full = ! wasEnabled || ticks % 30 == 0;    // full picture once a second
    wasEnabled = true;
    ++ticks;
    if (out == nullptr) { last = frame; return; }
    // The TempoPAD treats a colour message as the whole picture: pads left out of it go dark.
    // So whenever anything changed, send all 64 pads (259 bytes, ~30 a second at most).
    const bool changed = ! (frame == last);
    const auto bytes = (changed || full) ? Lights::encode (frame, last, true) : std::vector<uint8_t>();
    if (! bytes.empty()) out->sendMessageNow (MidiMessage::createSysExMessage (bytes.data() + 1, (int) bytes.size() - 2));
    last = frame;
}
