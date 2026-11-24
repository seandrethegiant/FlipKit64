#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <functional>
#include <vector>

// Sends Keys-track notes (and optionally MIDI clock) straight to a hardware MIDI output, e.g. an
// M-Vave FM-1 on USB. The audio thread only stamps events and pushes them into a lock-free queue;
// a sender thread sends each one at its due time, so nothing on the audio thread ever blocks on a
// MIDI driver. Works the same in the VST3 and the standalone app, with no host MIDI routing needed.
//
// Note-offs carry the "generation" of the note-on they belong to, so a sequenced note's scheduled
// note-off can't cut a newer note on the same key. Every sounding note is tracked, so Stop, a
// device change or closing always sends a note-off for each one (no stuck notes on the synth).
class ExternalMidi : private juce::Thread
{
public:
    ExternalMidi();
    ~ExternalMidi() override;

    // ---- message thread
    static juce::StringArray deviceNames();
    bool open (const juce::String& deviceName);       // "" closes; false = the device couldn't be opened
    void close() { open ({}); }
    juce::String deviceName() const;                   // the one that's open ("" = none)
    bool isOpen() const { return openFlag.load(); }
    bool active() const { return openFlag.load (std::memory_order_relaxed) || testSink != nullptr; }   // worth sending to

    // ---- audio thread (one producer)
    // Call once per block before the other calls; positions below are sample offsets in that block.
    void beginBlock (double sampleRate, int numSamples);
    void noteOn (int channel, int pitch, int velocity, int pos, juce::int64 gateSamples);   // gate <= 0: held until noteOff
    void noteOff (int channel, int pitch, int pos);
    void allNotesOff (int pos);                        // every note FlipKit64 has sounding on the device
    void clockTick (int pos)            { push (pos, 0xF8); }
    void start (int pos)                { push (pos, 0xFA); }
    void continuePlayback (int pos)     { push (pos, 0xFB); }
    void stop (int pos)                 { push (pos, 0xFC); }
    void songPosition (int pos, int sixteenths);

    std::atomic<float> offsetMs { 0.0f };              // nudge: + = later, - = earlier (vs FlipKit64's own sound)

    // ---- tests: no device, no thread. Messages go to the sink; deliver() sends what's due by `now`.
    std::function<void (const juce::MidiMessage&, double dueMs)> testSink;
    std::function<double()> testClock;
    void deliver (double nowMs) { pump (nowMs); }

private:
    struct Event
    {
        double due = 0.0;
        juce::uint8 b[3] {};
        juce::uint8 len = 0;
        juce::uint8 kind = 0;        // 0 = plain bytes, 1 = note-on, 2 = note-off, 3 = all notes off
        juce::uint16 gen = 0;
    };
    void push (int pos, juce::uint8 b0, juce::uint8 b1 = 0, juce::uint8 b2 = 0, juce::uint8 len = 1,
               juce::uint8 kind = 0, juce::uint16 gen = 0, double extraMs = 0.0);
    double now() const;
    double stamp (int pos) const;

    // audio thread state
    double blockStart = 0.0, blockMs = 0.0, rate = 48000.0;
    double lastStart = -1.0, lastBlockMs = 0.0;
    juce::uint16 gens[16][128] {};

    juce::AbstractFifo fifo { 4096 };
    Event fifoBuf[4096];

    // sender side (sender thread, or the caller of deliver() in tests)
    void run() override;
    void pump (double nowMs);
    void sendRaw (const juce::MidiMessage&, double due);
    void silenceAll();                                 // note-offs for everything sounding
    std::vector<Event> pending;
    juce::uint16 sounding[16][128] {};                 // generation of the sounding note, 0 = silent

    juce::CriticalSection deviceLock;                  // never taken on the audio thread
    std::unique_ptr<juce::MidiOutput> device;
    juce::String name;
    std::atomic<bool> openFlag { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExternalMidi)
};
