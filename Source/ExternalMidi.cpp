#include "ExternalMidi.h"
#include <algorithm>
#include <cmath>

using namespace juce;

ExternalMidi::ExternalMidi() : Thread ("FlipKit64 MIDI out")
{
    pending.reserve (8192);
}

ExternalMidi::~ExternalMidi()
{
    close();
    stopThread (500);
}

StringArray ExternalMidi::deviceNames()
{
    StringArray names;
    for (const auto& d : MidiOutput::getAvailableDevices()) names.add (d.name);
    return names;
}

String ExternalMidi::deviceName() const
{
    const ScopedLock sl (deviceLock);
    return name;
}

bool ExternalMidi::open (const String& wanted)
{
    std::unique_ptr<MidiOutput> next;
    String nextName;
    if (wanted.isNotEmpty())
    {
        const auto devices = MidiOutput::getAvailableDevices();
        const MidiDeviceInfo* pick = nullptr;
        for (const auto& d : devices) if (d.name == wanted) { pick = &d; break; }
        if (pick == nullptr)                                   // Windows can add " 2" etc. to a re-plugged device
            for (const auto& d : devices) if (d.name.startsWithIgnoreCase (wanted) || wanted.startsWithIgnoreCase (d.name)) { pick = &d; break; }
        if (pick == nullptr) return false;
        {
            const ScopedLock sl (deviceLock);
            if (device != nullptr && name == pick->name) return true;    // already open
        }
        next = MidiOutput::openDevice (pick->identifier);
        if (next == nullptr) return false;                    // usually: another app (LUNA) has it open
        nextName = pick->name;
    }

    {
        const ScopedLock sl (deviceLock);
        silenceAll();                                         // nothing left hanging on the old device
        pending.clear();
        device = std::move (next);
        name = nextName;
        openFlag = device != nullptr;
    }
    if (openFlag.load() && ! isThreadRunning()) startThread (Thread::Priority::high);
    return wanted.isEmpty() || openFlag.load();
}

// ---------------------------------------------------------------- audio thread

double ExternalMidi::now() const
{
    return testClock ? testClock() : Time::getMillisecondCounterHiRes();
}

void ExternalMidi::beginBlock (double sampleRate, int numSamples)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    blockMs = numSamples * 1000.0 / rate;
    const double t = now();
    // Audio callbacks arrive with jitter; follow the steady block clock and only drift toward
    // the real time, so evenly spaced notes leave evenly spaced. A big jump (a dropout, a pause) resyncs.
    if (lastStart >= 0.0)
    {
        const double expected = lastStart + lastBlockMs;
        blockStart = std::abs (t - expected) < 20.0 ? expected + (t - expected) * 0.05 : t;
    }
    else blockStart = t;
    lastStart = blockStart;
    lastBlockMs = blockMs;
}

double ExternalMidi::stamp (int pos) const
{
    // One block later than the block's own start: that's when FlipKit64's audio for this block is heard.
    return blockStart + blockMs + pos * 1000.0 / rate + offsetMs.load (std::memory_order_relaxed);
}

void ExternalMidi::push (int pos, uint8 b0, uint8 b1, uint8 b2, uint8 len, uint8 kind, uint16 gen, double extraMs)
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 < 1) return;                                       // full (no device draining?): drop
    auto& e = fifoBuf[s1];
    e.due = stamp (pos) + extraMs;
    e.b[0] = b0; e.b[1] = b1; e.b[2] = b2; e.len = len; e.kind = kind; e.gen = gen;
    fifo.finishedWrite (1);
}

void ExternalMidi::noteOn (int channel, int pitch, int velocity, int pos, int64 gateSamples)
{
    const int ch = jlimit (1, 16, channel) - 1, p = jlimit (0, 127, pitch);
    auto& g = gens[ch][p];
    if (++g == 0) g = 1;                                      // 0 means "silent"
    push (pos, (uint8) (0x90 | ch), (uint8) p, (uint8) jlimit (1, 127, velocity), 3, 1, g);
    if (gateSamples > 0)
        push (pos, (uint8) (0x80 | ch), (uint8) p, 0, 3, 2, g, gateSamples * 1000.0 / rate);
}

void ExternalMidi::noteOff (int channel, int pitch, int pos)
{
    const int ch = jlimit (1, 16, channel) - 1, p = jlimit (0, 127, pitch);
    push (pos, (uint8) (0x80 | ch), (uint8) p, 0, 3, 2, gens[ch][p]);
}

void ExternalMidi::allNotesOff (int pos) { push (pos, 0, 0, 0, 0, 3); }

void ExternalMidi::songPosition (int pos, int sixteenths)
{
    const int v = jlimit (0, 16383, sixteenths);
    push (pos, 0xF2, (uint8) (v & 0x7F), (uint8) (v >> 7), 3);
}

// ---------------------------------------------------------------- sender

void ExternalMidi::run()
{
    while (! threadShouldExit())
    {
        pump (now());
        wait (1);
    }
}

void ExternalMidi::sendRaw (const MidiMessage& m, double due)
{
    if (testSink) { testSink (m, due); return; }
    if (device != nullptr) device->sendMessageNow (m);
}

void ExternalMidi::silenceAll()
{
    for (int ch = 0; ch < 16; ++ch)
        for (int p = 0; p < 128; ++p)
            if (sounding[ch][p] != 0)
            {
                sounding[ch][p] = 0;
                sendRaw (MidiMessage::noteOff (ch + 1, p), 0.0);
            }
}

void ExternalMidi::pump (double nowMs)
{
    const ScopedLock sl (deviceLock);
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (fifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) pending.push_back (fifoBuf[s1 + i]);
        for (int i = 0; i < n2; ++i) pending.push_back (fifoBuf[s2 + i]);
        fifo.finishedRead (n1 + n2);
    }
    if (pending.empty()) return;
    // Stable: things stamped at the same moment keep the order they were made in (a note's
    // note-off before the next note-on at the same tick, Start before the first clock).
    std::stable_sort (pending.begin(), pending.end(), [] (const Event& a, const Event& b) { return a.due < b.due; });
    size_t done = 0;
    for (; done < pending.size() && pending[done].due <= nowMs; ++done)
    {
        const auto& e = pending[done];
        if (e.kind == 3) { silenceAll(); continue; }
        if (e.kind == 1)
        {
            const int ch = e.b[0] & 0x0F, p = e.b[1];
            if (sounding[ch][p] != 0) sendRaw (MidiMessage::noteOff (ch + 1, p), e.due);   // retrigger: end the old one first
            sounding[ch][p] = e.gen;
        }
        else if (e.kind == 2)
        {
            const int ch = e.b[0] & 0x0F, p = e.b[1];
            if (sounding[ch][p] == 0 || sounding[ch][p] != e.gen) continue;   // already ended, or a newer note took the key
            sounding[ch][p] = 0;
        }
        sendRaw (MidiMessage (e.b, (int) e.len), e.due);
    }
    pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) done);
}
