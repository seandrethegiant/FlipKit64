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

// Immutable snapshot of a pad. The editor builds a new one and swaps it in; the audio thread
// only ever reads snapshots, so no locks are needed while playing.
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
    float attackMs = 0.0f;             // 0 .. 2000
    float decayMs = 5000.0f;           // 10 .. 5000, 5000 = off (play whole region)
    float cutoff = 20000.0f;           // Hz, 20000 = filter off
    float reso = 0.0f;                 // 0 .. 1
    bool reverse = false;
    bool mono = false;                 // retrigger cuts the previous hit of this pad
    int choke = 0;                     // 0 = none, 1..8: pads in the same group cut each other

    bool decayOff() const  { return decayMs >= 4999.0f; }
    bool filterOff() const { return cutoff >= 19999.0f; }
};
using PadPtr = std::shared_ptr<const PadState>;

// ---------------------------------------------------------------------------------------------
// Sequencer data. Plain atomics so the editor can edit while the audio thread plays.
// ---------------------------------------------------------------------------------------------
namespace Seq
{
    constexpr int numPatterns = 8, maxSteps = 64;
}

struct PatternData
{
    std::atomic<int> length { 16 };          // steps (16/32/48/64)
    std::atomic<int> stepsPerBeat { 4 };     // 2 = 1/8, 3 = 1/8T, 4 = 1/16, 6 = 1/16T, 8 = 1/32
    std::atomic<uint8_t> vel[PadLayout::numPads][Seq::maxSteps];      // 0 = off
    std::atomic<uint8_t> ratchet[PadLayout::numPads][Seq::maxSteps];  // 1..4 hits per step

    PatternData() { clear(); }
    void clear()
    {
        for (auto& row : vel) for (auto& v : row) v.store (0);
        for (auto& row : ratchet) for (auto& r : row) r.store (1);
    }
    void copyFrom (const PatternData& o)
    {
        length.store (o.length.load()); stepsPerBeat.store (o.stepsPerBeat.load());
        for (int p = 0; p < PadLayout::numPads; ++p)
            for (int s = 0; s < Seq::maxSteps; ++s)
            { vel[p][s].store (o.vel[p][s].load()); ratchet[p][s].store (o.ratchet[p][s].load()); }
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
    void triggerPad (int index, float velocity);
    void previewFile (const juce::File& file);
    void previewRegion (const SamplePtr& sample, float start, float end);

    // Kits
    bool saveKit (const juce::File& file);
    bool loadKit (const juce::File& file);

    // ---- Sequencer (editor reads/writes these directly)
    PatternData patterns[Seq::numPatterns];
    std::atomic<int> currentPattern { 0 };
    std::atomic<int> queuedPattern { -1 };       // switches at the next pattern start while playing
    std::atomic<float> swing { 50.0f };          // 50 (straight) .. 75 percent
    std::atomic<bool> seqRecord { false };       // live-record incoming pad hits into the pattern
    std::atomic<bool> internalPlaying { false }; // own transport when LUNA isn't playing
    std::atomic<double> internalBpm { 120.0 };
    std::atomic<int> playStep { -1 };            // for the editor's playhead (-1 = stopped)
    std::atomic<bool> hostPlaying { false };
    std::atomic<double> hostBpm { 120.0 };
    void selectPattern (int p);

    // ---- Recording / resampling
    enum RecState { recIdle = 0, recArmed, recRecording, recFinished };
    std::atomic<int> recState { recIdle };
    std::atomic<int> recSource { 0 };            // 0 = FlipKit64's own output, 1 = audio input
    std::atomic<float> recThreshold { 0.0f };    // linear; 0 = start immediately
    std::atomic<int> recPos { 0 };
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

private:
    std::unique_ptr<juce::XmlElement> kitToXml() const;
    void kitFromXml (const juce::XmlElement& xml);
    void timerCallback() override;          // frees retired snapshots off the audio thread

    struct Hit { int pos; int pad; float velocity; };
    void handleHit (int padIndex, float velocity);
    void startVoice (const SamplePtr& s, const PadState* pad, float gain, int padIndex, float start = 0.0f, float end = 1.0f);
    void renderVoices (juce::AudioBuffer<float>& buffer, int start, int count);
    void scheduleSequencer (double ppq0, double ppqPerSample, int numSamples, Hit* hits, int& numHits, int maxHits);

    std::shared_ptr<const PadState> pads[PadLayout::numPads];
    int noteToPad[128];

    // voices
    struct Voice
    {
        SamplePtr sample;          // keeps sample alive while playing
        double pos = 0.0, step = 1.0;
        double regionStart = 0.0, regionEnd = 0.0;
        bool reverse = false;
        float gainL = 0.0f, gainR = 0.0f;
        int pad = -1, choke = 0;
        // envelope
        float env = 0.0f, attackInc = 1.0f, decayCoef = 1.0f;
        bool inAttack = false, decaying = false, releasing = false;
        float releaseCoef = 0.0f;
        // filter (TPT state-variable lowpass)
        bool filterOn = false;
        float fg = 0, fk = 0, fa1 = 0, fa2 = 0, fa3 = 0;
        float ic1[2] {}, ic2[2] {};
        bool active = false;
        void release (double sr) { releasing = true; releaseCoef = (float) std::exp (-1.0 / (0.004 * sr)); }
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
    struct Request { int pad; float velocity; };
    juce::AbstractFifo reqFifo { 64 };
    Request reqBuf[64];
    struct Preview { SamplePtr sample; float start = 0, end = 1; };
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadRomplerProcessor)
};
