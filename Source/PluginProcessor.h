#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <memory>
#include <vector>
#include <functional>

// ---------------------------------------------------------------------------------------------
// TempoPAD Z-1 layout (learned from the controller): 8x8, row 0 = top.
// Left 4 columns: notes 36..67 counting up from the bottom row; right 4 columns: 68..99.
// ---------------------------------------------------------------------------------------------
namespace PadLayout
{
    constexpr int rows = 8, cols = 8, numPads = rows * cols;
    inline int noteFor (int row, int col)
    {
        return col < 4 ? 36 + (7 - row) * 4 + col
                       : 68 + (7 - row) * 4 + (col - 4);
    }
    inline int padIndex (int row, int col) { return row * cols + col; }
    inline int noteForPad (int pad) { return noteFor (pad / cols, pad % cols); }
    inline int padForNote (int note) { for (int p = 0; p < numPads; ++p) if (noteForPad (p) == note) return p; return -1; }
}

// One loaded sample.
struct SampleData
{
    juce::File file;
    juce::AudioBuffer<float> audio;   // always 2 channels
    double sampleRate = 44100.0;
};
using SamplePtr = std::shared_ptr<const SampleData>;

enum class LayerMode { RoundRobin = 0, Velocity, Random };
// One-shot: plays to the end. Hold: plays while the pad is held (one step from the sequencer).
// Toggle: one press starts it, the next press stops it.
enum class PlayMode { OneShot = 0, Hold, Toggle };

// Immutable snapshot of a pad. The editor builds a new one and swaps it in; the audio thread
// only ever reads snapshots, so no locks are needed while playing.
// Envelope segment shape, 0..1 over t = 0..1. Curve +1 rises fast then levels off (like a natural decay),
// -1 starts slow, 0 is a straight line. Curve +1 on a decay to 0 matches the old exponential Decay knob.
inline float envCurveK (float curve) { return -6.9f * curve; }
inline float envShape (float t, float curve)
{
    const float k = envCurveK (curve);
    if (std::abs (k) < 1.0e-3f) return t;
    return (std::exp (k * t) - 1.0f) / (std::exp (k) - 1.0f);
}

struct PadState
{
    juce::String name;
    float gain = 1.0f;                 // linear
    LayerMode mode = LayerMode::RoundRobin;
    std::vector<SamplePtr> layers;     // velocity mode: first = softest

    // sound shaping (MPC-style "program edit")
    float pan = 0.0f;                  // -1 .. 1
    float tune = 0.0f;                 // semitones -24 .. 24
    float fine = 0.0f;                 // cents -100 .. 100
    float start = 0.0f, end = 1.0f;    // region within each sample, 0..1
    // ADSR envelope. Sustain 100% = plays the whole region (the old "decay off"). The release runs when a
    // Hold pad is let go, a Toggle pad is stopped or a Keys note ends; one-shot hits just play out.
    float attackMs = 0.0f;             // 0 .. 2000
    float decayMs = 300.0f;            // 10 .. 5000
    float sustain = 1.0f;              // 0 .. 1
    float releaseMs = 100.0f;          // 5 .. 5000
    float curveA = 0.0f, curveD = 0.7f, curveR = 0.8f;   // -1 .. 1: + = fast start, - = slow start, 0 = straight
    float fadeOutMs = 0.0f;            // fade at the end of the region (0 = just a click-free edge)
    float cutoff = 20000.0f;           // Hz, 20000 = filter off
    float reso = 0.0f;                 // 0 .. 1
    bool reverse = false;
    bool mono = false;                 // retrigger cuts the previous hit of this pad
    int choke = 0;                     // 0 = none, 1..8: pads in the same group cut each other
    PlayMode play = PlayMode::OneShot;

    void resetEnvelope() { const PadState d; attackMs = d.attackMs; decayMs = d.decayMs; sustain = d.sustain;
                           releaseMs = d.releaseMs; curveA = d.curveA; curveD = d.curveD; curveR = d.curveR; }
    bool filterOff() const { return cutoff >= 19999.0f; }
};
using PadPtr = std::shared_ptr<const PadState>;

// ---------------------------------------------------------------------------------------------
// Sequencer data. Plain atomics so the editor can edit while the audio thread plays.
// ---------------------------------------------------------------------------------------------
namespace Seq
{
    constexpr int numPatterns = 8, maxSteps = 64;

    // When a step plays, counted in loops of its pattern since the transport's start (Elektron-style trig conditions).
    enum Cond : uint8_t { always = 0, of2a, of2b, of4a, of4b, of4c, of4d, fillOnly, notFill, numConds };
    inline bool condPasses (int c, long long loop, bool fill)
    {
        switch (c)
        {
            case of2a: return loop % 2 == 0;   case of2b: return loop % 2 == 1;
            case of4a: return loop % 4 == 0;   case of4b: return loop % 4 == 1;
            case of4c: return loop % 4 == 2;   case of4d: return loop % 4 == 3;
            case fillOnly: return fill;        case notFill: return ! fill;
            default: return true;
        }
    }
    inline const char* condName (int c)
    {
        static const char* names[] = { "Every loop", "1st of 2", "2nd of 2", "1st of 4", "2nd of 4", "3rd of 4", "4th of 4", "Only in fills", "Not in fills" };
        return names[juce::jlimit (0, (int) numConds - 1, c)];
    }
    inline const char* condShort (int c)
    {
        static const char* names[] = { "", "1:2", "2:2", "1:4", "2:4", "3:4", "4:4", "F", "!F" };
        return names[juce::jlimit (0, (int) numConds - 1, c)];
    }
}

// Keys tracks: melodic lanes that play one pad's sound pitched across the keyboard.
namespace Keys
{
    constexpr int numTracks = 4;
    constexpr int lowNote = 24, highNote = 107;          // piano roll range (C0..B6 with middle C = C3)
    struct Note { uint8_t pitch, step, len, vel; };       // step and len in the lane's steps
    using NoteList = std::vector<Note>;
    using NotesPtr = std::shared_ptr<const NoteList>;     // immutable once made; swapped whole
    inline bool sameNotes (const NotesPtr& a, const NotesPtr& b)
    {
        const size_t na = a ? a->size() : 0, nb = b ? b->size() : 0;
        if (na != nb) return false;
        for (size_t i = 0; i < na; ++i)
        {
            const auto& x = (*a)[i]; const auto& y = (*b)[i];
            if (x.pitch != y.pitch || x.step != y.step || x.len != y.len || x.vel != y.vel) return false;
        }
        return true;
    }

    // Scales for highlighting and snapping (semitone masks from the root).
    constexpr int numScales = 8;
    inline const char* scaleName (int s)
    {
        static const char* n[] = { "Chromatic", "Major", "Minor", "Dorian", "Mixolydian", "Minor pentatonic", "Major pentatonic", "Blues" };
        return n[juce::jlimit (0, numScales - 1, s)];
    }
    inline bool inScale (int s, int root, int pitch)
    {
        static const int masks[] = { 0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x6B5, 0x4A9, 0x295, 0x4E9 };
        const int pc = ((pitch - root) % 12 + 12) % 12;
        return (masks[juce::jlimit (0, numScales - 1, s)] >> pc) & 1;
    }
}

// One Keys track's notes inside a pattern, with its own length and step size.
struct KeysLane
{
    std::atomic<int> length { 16 };
    std::atomic<int> stepsPer2Beats { 4 };                // 1/8 by default (16 steps = 2 bars)
    Keys::NotesPtr notes;                                 // nullptr = empty; use get / set
    double stepLength() const { return 2.0 / juce::jmax (1, stepsPer2Beats.load()); }
    Keys::NotesPtr get() const { return std::atomic_load (&notes); }
    void set (Keys::NotesPtr n) { std::atomic_store (&notes, std::move (n)); }
    void reset() { length = 16; stepsPer2Beats = 4; set (nullptr); }
};

struct PatternData
{
    std::atomic<int> length { 16 };          // steps (16/32/48/64)
    std::atomic<int> stepsPer2Beats { 8 };   // steps in two beats: 2 = 1/4, 3 = 1/4T, 4 = 1/8, 6 = 1/8T, 8 = 1/16, 12 = 1/16T, 16 = 1/32
    double stepLength() const { return 2.0 / juce::jmax (1, stepsPer2Beats.load()); }   // in beats
    std::atomic<uint8_t> vel[PadLayout::numPads][Seq::maxSteps];      // 0 = off
    std::atomic<uint8_t> ratchet[PadLayout::numPads][Seq::maxSteps];  // 1..4 hits per step
    std::atomic<uint8_t> chance[PadLayout::numPads][Seq::maxSteps];   // 1..100 percent
    std::atomic<uint8_t> cond[PadLayout::numPads][Seq::maxSteps];     // Seq::Cond
    std::atomic<uint8_t> nudge[PadLayout::numPads][Seq::maxSteps];    // 128 = on the grid; 78..178 = -50..+50 % of a step

    KeysLane lanes[Keys::numTracks];

    PatternData() { clear(); }
    void clear() { clearDrums(); for (auto& l : lanes) l.reset(); }
    void clearDrums()
    {
        for (int p = 0; p < PadLayout::numPads; ++p)
            for (int s = 0; s < Seq::maxSteps; ++s) { vel[p][s].store (0); resetStep (p, s); }
    }
    void resetStep (int p, int s)               // everything but velocity back to plain
    {
        ratchet[p][s].store (1); chance[p][s].store (100); cond[p][s].store (0); nudge[p][s].store (128);
    }
    void setStep (int p, int s, uint8_t v) { vel[p][s].store (v); resetStep (p, s); }
    void copyStep (int p, int s, const PatternData& o, int op, int os)
    {
        vel[p][s].store (o.vel[op][os].load()); ratchet[p][s].store (o.ratchet[op][os].load());
        chance[p][s].store (o.chance[op][os].load()); cond[p][s].store (o.cond[op][os].load());
        nudge[p][s].store (o.nudge[op][os].load());
    }
    void copyFrom (const PatternData& o)
    {
        length.store (o.length.load()); stepsPer2Beats.store (o.stepsPer2Beats.load());
        for (int p = 0; p < PadLayout::numPads; ++p)
            for (int s = 0; s < Seq::maxSteps; ++s) copyStep (p, s, o, p, s);
        for (int t = 0; t < Keys::numTracks; ++t)
        {
            lanes[t].length.store (o.lanes[t].length.load());
            lanes[t].stepsPer2Beats.store (o.lanes[t].stepsPer2Beats.load());
            lanes[t].set (o.lanes[t].get());
        }
    }
    bool sameAs (const PatternData& o) const
    {
        if (length.load() != o.length.load() || stepsPer2Beats.load() != o.stepsPer2Beats.load()) return false;
        for (int t = 0; t < Keys::numTracks; ++t)
            if (lanes[t].length.load() != o.lanes[t].length.load() || lanes[t].stepsPer2Beats.load() != o.lanes[t].stepsPer2Beats.load()
                 || ! Keys::sameNotes (lanes[t].get(), o.lanes[t].get()))
                return false;
        for (int p = 0; p < PadLayout::numPads; ++p)
            for (int s = 0; s < Seq::maxSteps; ++s)
                if (vel[p][s].load() != o.vel[p][s].load() || (vel[p][s].load() > 0
                     && (ratchet[p][s].load() != o.ratchet[p][s].load() || chance[p][s].load() != o.chance[p][s].load()
                         || cond[p][s].load() != o.cond[p][s].load() || nudge[p][s].load() != o.nudge[p][s].load())))
                    return false;
        return true;
    }
    bool padHasSteps (int pad) const
    {
        for (int s = 0; s < Seq::maxSteps; ++s) if (vel[pad][s].load() > 0) return true;
        return false;
    }
};

class PadRomplerProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    PadRomplerProcessor();
    ~PadRomplerProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "FlipKit64"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- Pads (message thread)
    PadPtr getPad (int index) const;
    void setPad (int index, PadPtr newState);
    void updatePad (int index, const std::function<void (PadState&)>& change);
    void assignFiles (int index, const juce::Array<juce::File>& files, bool append);
    void assignRegion (int index, const SamplePtr& sample, float start, float end, const juce::String& name);
    void removeLayer (int index, int layer);
    void moveLayer (int index, int layer, int delta);
    void clearPad (int index);
    SamplePtr loadSample (const juce::File& file);

    // Auditioning from the editor
    void triggerPad (int index, float velocity, bool held = false);   // held: a Hold pad plays until releasePad
    void releasePad (int index);
    void auditionRelease (int index);       // let go of a pad's sounds whatever its play mode (envelope "Hear it")
    void stopAllSounds() { stopSoundsReq = true; }   // fade out every pad and Keys sound (Stop, Esc)
    void clearAllPads();
    void newKit();                                   // pads, patterns and Keys tracks back to empty
    void previewFile (const juce::File& file);                 // load into the browser player and play
    void previewRegion (const SamplePtr& sample, float start, float end, const PadState* shape = nullptr);
    SamplePtr saveEdited (const SampleData& audio, const juce::String& name);   // writes a WAV beside the recordings, loads it

    // Browser player: one file at a time, with play / pause / stop / seek / loop.
    void playerLoad (const SamplePtr& sample, bool autoPlay);
    void playerPlay();
    void playerPause();
    void playerStop();                                         // fades out, back to the start
    void playerSeek (double fraction);
    void playerSetLoop (bool shouldLoop) { playerLoop = shouldLoop; }
    bool playerLooping() const { return playerLoop.load(); }
    bool playerIsPlaying() const { return playerPlaying.load(); }
    SamplePtr playerSampleRef() const { return playerHold; }
    double playerPositionFraction() const;

    // Kits
    bool saveKit (const juce::File& file);
    bool loadKit (const juce::File& file);

    // ---- Sequencer (editor reads/writes these directly)
    PatternData patterns[Seq::numPatterns];
    std::atomic<int> currentPattern { 0 };
    std::atomic<int> queuedPattern { -1 };       // switches at the next pattern start while playing
    std::atomic<float> swing { 50.0f };          // 50 (straight) .. 75 percent
    std::atomic<bool> seqRecord { false };       // live-record incoming pad hits into the pattern
    std::atomic<bool> fillOn { false };          // "Fill" held/latched: fill-only steps play, not-in-fill steps rest
    std::atomic<int> rollDiv { 0 };              // Roll: 0 = off, else repeats per beat (2 = 1/8, 4 = 1/16, 8 = 1/32)
    int captureToPattern();                      // message thread: last loop of pad playing -> current pattern; -1 if stopped
    std::atomic<uint32_t> kitLoads { 0 };        // bumped when a kit or project replaces the patterns (editor resets undo)
    std::atomic<bool> internalPlaying { false }; // own transport when LUNA isn't playing
    std::atomic<double> internalBpm { 120.0 };
    std::atomic<int> playStep { -1 };            // for the editor's playhead (-1 = stopped)
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostBpm { 120.0 };
    void selectPattern (int p);

    // ---- Keys tracks (settings shared by all patterns; the notes live in each pattern's lanes)
    struct KeysTrack
    {
        std::atomic<int> sourcePad { -1 };       // the pad whose sound (and shaping) this track plays
        std::atomic<int> root { 60 };            // the note the sample sounds at unpitched (60 = C3)
        std::atomic<bool> mono { false };
        std::atomic<float> glideMs { 0.0f };     // mono only: slide into the next note
        std::atomic<float> level { 1.0f };
        std::atomic<int> sliceCount { 0 };       // >0: slice mode - key root+i plays the pad i notes above sourcePad, unpitched
        juce::String name;                       // message thread
    };
    KeysTrack keysTracks[Keys::numTracks];
    std::atomic<int> keysInput { -1 };           // track that pad notes play (-1 = pads play drums)
    std::atomic<int> scaleRoot { 0 }, scaleType { 0 };
    std::atomic<int> keysStep[Keys::numTracks];  // playhead per track for the editor (-1 = stopped)
    Keys::NotesPtr makeNotes (Keys::NoteList);   // message thread: every note list is made here
    void triggerKey (int track, int pitch, float velocity);   // audition from the editor
    void resetKeysTracks();
    void drainKeyRecordings();                   // message thread: live-recorded notes -> lanes
    std::atomic<uint32_t> keysRecorded { 0 };    // bumped when recorded notes land (editor repaints)

    // ---- TempoPAD modes: the controller's own mode buttons switch what its pads do in FlipKit64.
    // Drum/Key (channel 1 notes) = play pads, User (channel 3 notes) = toggle steps for the selected
    // pad, Easy (Synido's short SysEx per pad) = top row picks patterns, the rest play.
    // Play / Stop / Record arrive as MMC when the controller's MMC button is on.
    enum CtrlMode { ctrlNone = 0, ctrlPads, ctrlSteps, ctrlPatterns };
    std::atomic<bool> tempoPadModes { true };
    std::atomic<int> ctrlMode { ctrlNone };
    std::atomic<int> stepTarget { 0 };          // pad that Steps-mode presses write to
    // Easy mode is the only mode where the TempoPAD takes live pad colours, so FlipKit64 decides
    // what its pads do there: 0 = top row picks patterns, rest play; 1 = all play; 2 = steps.
    std::atomic<int> easyLayout { 0 };
    std::atomic<int> velocityMode { 0 };         // pads played live: 0 as played, 1 lighter touch, 2 full level

    // TempoPAD Play / Stop / Record when they don't send MMC (MMC off, or a mode that sends them as CCs).
    // Each: (type << 16) | (channel << 8) | number; type 1 = CC, 2 = note; channel 0 = any; 0 = not set.
    enum TransportButton { tbStop = 0, tbPlay, tbRecord, numTransportButtons };
    static int transportCode (int type, int channel, int number) { return (type << 16) | (channel << 8) | number; }
    static constexpr int defaultStopCode = (1 << 16) | 127;    // Synido's default: Stop = CC 127
    std::atomic<int> transportMsg[numTransportButtons] { { defaultStopCode }, { 0 }, { 0 } };
    std::atomic<int> transportLearn { -1 };      // a button waiting to be taught: the next CC or note becomes it
    std::atomic<int> lastMidiCode { 0 };         // the last CC / note received (shown while teaching)
    void transportAction (int button, bool hostIsPlaying);

    // Troubleshooting log (TempoPAD... > Write a MIDI log): incoming MIDI and how long each sound lasts.
    struct LogEvent
    {
        enum Kind : uint8_t { midi = 1, soundStart, soundRelease, soundEnd, soundStolen };
        juce::int64 clock = 0;     // samples since start
        uint8_t kind = 0, bytes[3] {};
        int size = 0, track = -1, pad = -1;
        float a = 0.0f, b = 0.0f;  // start: gain, semis   end/release: seconds since start, level
        juce::int64 gate = 0;
        int how = 0;               // end: 0 end of sample, 1 faded out after release, 2 cut short (choke / steal / stop)
    };
    std::atomic<bool> midiLog { false };
    juce::AbstractFifo logFifo { 2048 };
    LogEvent logBuf[2048];
    double logRate() const { return hostRate; }
    void logEvent (const LogEvent& e) noexcept
    {
        if (! midiLog.load (std::memory_order_relaxed)) return;
        int s1, n1, s2, n2;
        logFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) { logBuf[s1] = e; logFifo.finishedWrite (1); }
    }
    std::atomic<bool> padLights { true };       // send pad colours to the TempoPAD (Easy mode)
    std::atomic<bool> samplerView { false };    // Sampler tab open: Easy-mode presses pick slices (editor reads samplerPress)
    std::atomic<int> samplerPressPad { -1 };
    std::atomic<uint32_t> samplerPresses { 0 };
    std::unique_ptr<class PadLights> lights;
    std::atomic<int> lastStepEdit { -1 };
    std::atomic<uint32_t> stepEdits { 0 };      // bumped on each Steps-mode edit (editor repaints)

    // ---- Recording / resampling
    enum RecState { recIdle = 0, recArmed, recRecording, recFinished };
    std::atomic<int> recState { recIdle };
    std::atomic<int> recSource { 0 };            // 0 = FlipKit64's own output, 1 = audio input
    std::atomic<float> recThreshold { 0.0f };    // linear; 0 = start immediately
    std::atomic<int> recPos { 0 };
    std::atomic<int> recMax { 1 };               // recording buffer length in samples (pad lights' time bar)
    std::atomic<float> recInputLevel { 0.0f };   // meter for the editor
    bool armRecording (double maxSeconds = 60.0);
    void stopRecording();
    juce::File saveRecording();                  // call after recFinished; returns the WAV written
    bool inputBusAvailable() const;
    double getHostRate() const { return hostRate; }

    // ---- Tap chop: play a sample and turn pad hits into chop points
    enum TapMode { tapOff = 0, tapMarkers, tapPads };
    struct TapEvent { int pad; double pos; };    // pos = sample position in the source sample
    std::atomic<int> tapMode { tapOff };
    std::atomic<bool> tapPlaying { false };
    std::atomic<double> tapPos { 0.0 };          // playhead (source samples) for the editor
    std::atomic<bool> tapReachedEnd { false };
    void startTap (const SamplePtr& s, double startSample);
    void stopTap() { tapPlaying = false; }
    int popTapEvents (TapEvent* out, int max);   // message thread

    juce::File browserRoot;
    juce::AudioFormatManager formats;
    std::atomic<float> masterGain { 0.8f };

    // Hit indicators for the editor (set by audio thread, cleared by editor)
    std::atomic<uint32_t> hitCounter[PadLayout::numPads] {};
    std::function<void()> onPadsChanged;   // editor refresh hook

    static juce::File recordingsFolder();

    std::unique_ptr<juce::XmlElement> kitToXml() const;
    void kitFromXml (const juce::XmlElement& xml);

private:
    void timerCallback() override;          // frees retired snapshots off the audio thread

    // A hit to start this block: a pad, or (track >= 0) a Keys note; velocity < 0 = that key's note-off.
    // gate = samples the note sounds (-1 = held until its note-off).
    struct Hit { int pos; int pad; float velocity; int track = -1; int pitch = 0; juce::int64 gate = 0; };
    void handleHit (int padIndex, float velocity, juce::int64 gate = 0);
    void padRelease (int padIndex);
    std::atomic<bool> stopSoundsReq { false };
    bool wasRunning = false;
    void handleKeyHit (int track, int pitch, float velocity, juce::int64 gate);
    void releaseKey (int track, int pitch);
    int chooseLayer (int padIndex, const PadState& pad, float velocity);
    struct Voice;
    Voice* startVoice (const SamplePtr& s, const PadState* pad, float gain, int padIndex, float start = 0.0f, float end = 1.0f,
                       int keysTrack = -1, double semis = 0.0, juce::int64 gate = 0);
    void scheduleKeys (double ppq0, double ppqPerSample, int numSamples, Hit* hits, int& numHits, int maxHits);
    std::vector<Keys::NotesPtr> noteRegistry;    // message thread: keeps every list alive until nothing uses it

    // Live-recorded keys (audio thread -> message thread)
    struct KeyDown { int track = -1; double ppq = -1.0; float vel = 0.0f; int pattern = 0; };
    KeyDown keyDown[128];
    struct KeyRec { int track, pattern, pitch; double on, off; float vel; };
    juce::AbstractFifo keyRecFifo { 256 };
    KeyRec keyRecBuf[256];
    void renderVoices (juce::AudioBuffer<float>& buffer, int start, int count);
    void scheduleSequencer (double ppq0, double ppqPerSample, int numSamples, Hit* hits, int& numHits, int maxHits);

    // 0..99, fixed for a given step occurrence (so a ratchet's repeats agree), different every loop.
    uint64_t rollSeed = (uint64_t) juce::Random::getSystemRandom().nextInt64();
    int stepRoll (long long k, int pad) const
    {
        uint64_t h = (uint64_t) k * 0x9E3779B97F4A7C15ull ^ ((uint64_t) pad + 1) * 0xC2B2AE3D27D4EB4Full ^ rollSeed;
        h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
        return (int) (h % 100);
    }

    std::shared_ptr<const PadState> pads[PadLayout::numPads];
    int noteToPad[128];

    // Roll (audio thread only): pads currently held down
    struct Held { bool on = false; float vel = 0.0f; double pressPpq = -1.0; double nextSample = 0.0; };
    Held held[PadLayout::numPads];
    juce::int64 sampleClock = 0;

    // Capture: the most recent pad hits played while the sequencer runs (audio thread writes, editor reads)
    struct CapHit { std::atomic<double> ppq { 0.0 }; std::atomic<int> pad { 0 }; std::atomic<float> vel { 0.0f }; };
    static constexpr int capSize = 1024;
    CapHit capLog[capSize];
    std::atomic<uint32_t> capCount { 0 };
    std::atomic<double> lastPpqEnd { -1.0 };     // transport position at the end of the last block, -1 = stopped

    // voices
    struct Voice
    {
        SamplePtr sample;          // keeps sample alive while playing
        double pos = 0.0, step = 1.0;
        double regionStart = 0.0, regionEnd = 0.0;
        bool reverse = false;
        double fadeOut = 64.0;     // source samples faded at the region's end
        float gainL = 0.0f, gainR = 0.0f;
        int pad = -1, choke = 0;
        // envelope: ADSR segments (stage) plus a separate quick fade (releasing) for choke, steal and stop
        enum Stage : uint8_t { stAttack, stDecay, stSustain, stRelease };
        float env = 0.0f;
        uint8_t stage = stSustain;
        double segT = 0.0, segInc = 0.0, decInc = 0.0, relInc = 0.0;
        float segFrom = 0.0f, segTo = 0.0f, segK = 0.0f, segDen = 1.0f;
        float sustainLvl = 1.0f, kD = 0.0f, kR = 0.0f;
        bool releasing = false;
        float releaseCoef = 0.0f;
        // filter (TPT state-variable lowpass)
        bool filterOn = false;
        float fg = 0, fk = 0, fa1 = 0, fa2 = 0, fa3 = 0;
        float ic1[2] {}, ic2[2] {};
        bool active = false;
        // Keys notes
        int keysTrack = -1, keysPitch = 0;
        juce::int64 startedAt = 0;             // troubleshooting log: when it started, whether its release was logged
        bool loggedRelease = false;
        juce::int64 gateLeft = 0;              // >0 counts down to the note-off, -1 = held, 0 = none
        double baseStep = 1.0, stepTarget = 1.0;
        float glideCoef = 0.0f;
        void release (double sr, double ms = 4.0) { releasing = true; releaseCoef = (float) std::exp (-1.0 / (ms * 0.001 * sr)); }
        void beginSegment (uint8_t st, float from, float to, float k, double inc)
        {
            stage = st; segT = 0.0; segInc = inc; segFrom = from; segTo = to; segK = k;
            segDen = std::abs (k) < 1.0e-3f ? 1.0f : std::exp (k) - 1.0f;
        }
        void noteOff() { if (! releasing && stage != stRelease) beginSegment (stRelease, env, 0.0f, kR, relInc); }
    };
    static constexpr int maxVoices = 128;
    Voice voices[maxVoices];
    double hostRate = 44100.0;

    // per-pad round robin counters (audio thread only)
    int rrIndex[PadLayout::numPads] {};
    juce::Random rng;

    // sequencer runtime (audio thread only)
    double internalPpq = 0.0;
    bool wasInternalPlaying = false;
    uint8_t skipOnce[PadLayout::numPads][Seq::maxSteps] {};

    // editor -> audio requests (lock-free single-producer single-consumer)
    struct Request { int pad; float velocity; int track = -1; int pitch = 0; bool held = false; };
    juce::AbstractFifo reqFifo { 64 };
    Request reqBuf[64];
    struct Preview { SamplePtr sample; float start = 0, end = 1; bool shaped = false; PadState shape; };
    std::shared_ptr<const Preview> pendingPreview;
    std::atomic<bool> previewWaiting { false };

    // tap chop playback
    std::shared_ptr<const SampleData> tapSample;
    std::atomic<double> tapStartRequest { 0.0 };
    std::atomic<bool> tapRestart { false };
    double tapPlayPos = 0.0;
    juce::AbstractFifo tapFifo { 256 };
    TapEvent tapBuf[256];

    // recording
    juce::AudioBuffer<float> recBuffer;
    juce::AudioBuffer<float> inputCopy;
    double recRate = 44100.0;

    // The message thread holds an extra reference to every sample and every replaced pad snapshot,
    // so the audio thread never drops the last reference (never frees memory). A timer releases
    // them once nothing else is using them.
    std::shared_ptr<int> aliveToken = std::make_shared<int> (0);
    std::vector<SamplePtr> sampleCache;
    std::vector<PadPtr> retiredPads;
    std::vector<std::shared_ptr<const Preview>> retiredPreviews;

    // browser player
    SamplePtr playerSample;                        // read by the audio thread with atomic_load
    SamplePtr playerHold;                          // message thread's reference to the current one
    std::vector<SamplePtr> playerRetired;
    std::atomic<bool> playerPlaying { false }, playerLoop { false }, playerStopReq { false };
    std::atomic<double> playerSeekReq { -1.0 };    // in source frames; -1 = none
    std::atomic<double> playerPosOut { 0.0 };      // current position, source frames
    double playerPos = 0.0;                        // audio thread
    float playerAmp = 0.0f;                        // audio thread: fade in/out gain

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadRomplerProcessor)
};
