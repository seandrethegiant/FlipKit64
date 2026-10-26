#include "../Source/PluginProcessor.h"
#include "../Source/SamplerPage.h"
#include <cstdio>
using namespace juce;

static int fails = 0;
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++fails; }

struct FakePlayHead : AudioPlayHead
{
    double ppq = 0, bpm = 120; bool playing = true;
    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying (playing); p.setBpm (bpm); p.setPpqPosition (ppq); return p;
    }
};

static constexpr double SR = 48000.0;
static constexpr int BS = 64;

// render n blocks, return rms of left channel; optional note at first block
static float run (PadRomplerProcessor& p, int blocks, int note = -1, float vel = 1.0f, FakePlayHead* ph = nullptr)
{
    AudioBuffer<float> b (2, BS); MidiBuffer m;
    if (note >= 0) m.addEvent (MidiMessage::noteOn (1, note, vel), 0);
    double sum = 0; int n = 0;
    for (int i = 0; i < blocks; ++i)
    {
        p.processBlock (b, m); m.clear();
        if (ph) ph->ppq += BS * ph->bpm / 60.0 / SR;
        for (int s = 0; s < BS; ++s) { sum += b.getSample (0, s) * b.getSample (0, s); ++n; }
    }
    return (float) std::sqrt (sum / jmax (1, n));
}
static float runDouble (PadRomplerProcessor& p, int note, int blocks)   // two hits at the same instant
{
    AudioBuffer<float> b (2, BS); MidiBuffer m;
    m.addEvent (MidiMessage::noteOn (1, note, 1.0f), 0); m.addEvent (MidiMessage::noteOn (1, note, 1.0f), 0);
    double sum = 0; int n = 0;
    for (int i = 0; i < blocks; ++i) { p.processBlock (b, m); m.clear(); for (int s = 0; s < BS; ++s) { sum += b.getSample (0, s) * b.getSample (0, s); ++n; } }
    return (float) std::sqrt (sum / n);
}
static void silence (PadRomplerProcessor& p)
{
    AudioBuffer<float> b (2, BS); MidiBuffer m; m.addEvent (MidiMessage::allSoundOff (1), 0);
    p.processBlock (b, m); m.clear();
    for (int i = 0; i < 40; ++i) p.processBlock (b, m);
}
static SamplePtr makeTone (double seconds, float freq, float decayPerSec = 0.0f)
{
    auto s = std::make_shared<SampleData>();
    s->sampleRate = SR; const int n = (int) (seconds * SR);
    s->audio.setSize (2, n);
    for (int i = 0; i < n; ++i)
    {
        const float v = 0.5f * std::sin (6.2831853f * freq * i / (float) SR) * std::exp (-decayPerSec * i / (float) SR);
        s->audio.setSample (0, i, v); s->audio.setSample (1, i, v);
    }
    s->file = File ("/tmp/claude-0/tone.wav");
    return s;
}
static int padOfNote (int note) { for (int i = 0; i < 64; ++i) if (PadLayout::noteForPad (i) == note) return i; return -1; }
static uint32_t hits (PadRomplerProcessor& p, int note) { return p.hitCounter[padOfNote (note)].load(); }

int main (int, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    const int A = padOfNote (36), B = padOfNote (37), C = padOfNote (38);

    {   // ---------------- basics with the test kit
        PadRomplerProcessor p; p.prepareToPlay (SR, BS);
        check (p.loadKit (File (argv[1])), "kit loads");
        check (run (p, 300, 36) > 0.01f, "note 36 plays kick");
        silence (p);
        check (run (p, 300, 70) < 1e-6f, "empty pad silent");
        check (run (p, 300, 20) < 1e-6f, "off-grid note silent");
        silence (p);
        const float soft = run (p, 600, 40, 0.2f); silence (p);
        const float hard = run (p, 600, 40, 1.0f); silence (p);
        check (hard > soft && soft > 0, "velocity layers: hard louder than soft");
    }

    PadRomplerProcessor p; p.prepareToPlay (SR, BS);
    auto tone = makeTone (3.0, 220.0f);
    p.assignRegion (A, tone, 0, 1, "toneA");
    p.assignRegion (B, makeTone (3.0, 330.0f), 0, 1, "toneB");
    p.assignRegion (C, makeTone (1.0, 100.0f, 6.0f), 0, 1, "decaying");

    {   // ---------------- choke groups
        run (p, 20, 36); const float aloneA = run (p, 20);
        run (p, 1, 37); run (p, 30);                     // B without choke: A keeps going
        const float both = run (p, 20); silence (p);
        p.updatePad (A, [] (PadState& s) { s.choke = 1; });
        p.updatePad (B, [] (PadState& s) { s.choke = 1; s.gain = 0.0f; });   // B silent so only A's level shows
        run (p, 20, 36); run (p, 1, 37); run (p, 30);
        const float choked = run (p, 20); silence (p);
        check (aloneA > 0.1f && both > 0.1f && choked < 0.01f, "choke group: B cuts A");
        p.updatePad (A, [] (PadState& s) { s.choke = 0; }); p.updatePad (B, [] (PadState& s) { s.choke = 0; s.gain = 1.0f; });
    }
    {   // ---------------- mono
        runDouble (p, 36, 1); const float poly2 = run (p, 20); silence (p);
        p.updatePad (A, [] (PadState& s) { s.mono = true; });
        runDouble (p, 36, 1); run (p, 10); const float mono2 = run (p, 20); silence (p);
        check (poly2 > mono2 * 1.5f, "mono: retrigger cuts the previous hit (no stacking)");
        p.updatePad (A, [] (PadState& s) { s.mono = false; });
    }
    {   // ---------------- reverse + region + decay + filter
        const float fwdStart = run (p, 10, 38); silence (p);
        p.updatePad (C, [] (PadState& s) { s.reverse = true; });
        const float revStart = run (p, 10, 38); silence (p);
        check (fwdStart > revStart * 3.0f, "reverse: decaying sample starts quiet when reversed");
        p.updatePad (C, [] (PadState& s) { s.reverse = false; });

        p.updatePad (A, [] (PadState& s) { s.end = 0.1f; });           // 0.3 s of a 3 s tone
        run (p, 1, 36); const float early = run (p, 100); run (p, 200); const float late = run (p, 100);
        check (early > 0.1f && late < 0.01f, "region end: stops at 10% of the sample");
        p.updatePad (A, [] (PadState& s) { s.end = 1.0f; s.decayMs = 100.0f; });
        silence (p); run (p, 1, 36); run (p, 200); const float afterDecay = run (p, 20);
        check (afterDecay < 0.01f, "decay: 100 ms decay fades the hit");
        p.updatePad (A, [] (PadState& s) { s.decayMs = 5000.0f; s.cutoff = 100.0f; s.reso = 0.0f; });
        silence (p); run (p, 1, 36); run (p, 30); const float filtered = run (p, 30);
        p.updatePad (A, [] (PadState& s) { s.cutoff = 20000.0f; });
        silence (p); run (p, 1, 36); run (p, 30); const float open = run (p, 30);
        check (filtered < open * 0.6f, "filter: 100 Hz lowpass attenuates a 220 Hz tone");
        silence (p);
    }

    {   // ---------------- sequencer timing (host transport)
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.05, 1000.0f), 0, 1, "click");
        FakePlayHead ph; q.setPlayHead (&ph);
        auto& pat = q.patterns[0];
        pat.vel[A][0] = 100; pat.vel[A][4] = 100;
        const uint32_t h0 = hits (q, 36);
        while (ph.ppq < 1.05) run (q, 1, -1, 1, &ph);
        check (hits (q, 36) - h0 == 2, "sequencer: steps 1 and 5 fire within the first beat");

        pat.ratchet[A][8] = 4; pat.vel[A][8] = 100;
        const uint32_t h1 = hits (q, 36);
        while (ph.ppq < 2.24) run (q, 1, -1, 1, &ph);
        check (hits (q, 36) - h1 == 4, "ratchet x4 = 4 hits in one step");

        q.patterns[1].vel[B][1] = 100;
        q.patterns[1].length = 16;
        q.assignRegion (B, makeTone (0.05, 1000.0f), 0, 1, "click2");
        q.swing = 75.0f; ph.playing = false; run (q, 2, -1, 1, &ph);
        q.selectPattern (1);
        ph.ppq = 0.0; ph.playing = true;
        const uint32_t hb = hits (q, 37);
        while (ph.ppq < 0.36) run (q, 1, -1, 1, &ph);
        const bool notYet = hits (q, 37) == hb;
        while (ph.ppq < 0.39) run (q, 1, -1, 1, &ph);
        check (notYet && hits (q, 37) == hb + 1, "swing 75%: off-beat step lands at ppq 0.375");

        q.swing = 50.0f;
        q.patterns[0].clear(); q.patterns[0].vel[C][0] = 100;
        q.assignRegion (C, makeTone (0.05, 500.0f), 0, 1, "click3");
        q.selectPattern (0);
        check (q.queuedPattern.load() == 0 && q.currentPattern.load() == 1, "switch is queued while playing");
        while (ph.ppq < 3.9) run (q, 1, -1, 1, &ph);
        const bool stillOld = q.currentPattern.load() == 1;
        while (ph.ppq < 4.05) run (q, 1, -1, 1, &ph);
        check (stillOld && q.currentPattern.load() == 0, "queued pattern starts at the bar line");

        q.seqRecord = true;
        while (ph.ppq < 4.24) run (q, 1, -1, 1, &ph);
        const uint32_t hr = hits (q, 36);
        run (q, 1, 36, 0.8f, &ph);
        while (ph.ppq < 4.4) run (q, 1, -1, 1, &ph);
        const int rv = q.patterns[0].vel[A][1].load();
        check (rv > 90 && rv < 110, "live record: hit lands on step 2 with its velocity");
        check (hits (q, 36) == hr + 1, "live record: no double trigger right after recording");
        while (ph.ppq < 8.4) run (q, 1, -1, 1, &ph);
        check (hits (q, 36) == hr + 2, "live record: recorded step plays on the next loop");
        q.seqRecord = false;

        ph.playing = false; q.internalPlaying = true; q.internalBpm = 120.0;
        q.patterns[0].clear(); q.patterns[0].vel[A][0] = 100;
        const uint32_t hi = hits (q, 36);
        run (q, 375, -1, 1, &ph);
        check (hits (q, 36) == hi + 1, "internal transport plays the pattern on its own");
        q.internalPlaying = false;
        q.setPlayHead (nullptr);
    }

    {   // ---------------- state round trip (pad params + patterns)
        p.updatePad (A, [] (PadState& s) { s.tune = 5; s.choke = 3; s.reverse = true; s.start = 0.25f; });
        p.patterns[2].vel[B][7] = 77; p.patterns[2].ratchet[B][7] = 3; p.patterns[2].length = 32;
        MemoryBlock mb; p.getStateInformation (mb);
        PadRomplerProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        auto pa = r.getPad (A);
        check (pa->tune == 5 && pa->choke == 3 && pa->reverse && std::abs (pa->start - 0.25f) < 1e-4f, "state keeps pad shaping");
        check (r.patterns[2].vel[B][7].load() == 77 && r.patterns[2].ratchet[B][7].load() == 3 && r.patterns[2].length.load() == 32,
               "state keeps sequencer patterns");
    }

    {   // ---------------- resampling
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.5, 440.0f), 0, 1, "t");
        q.recSource = 0; q.recThreshold = 0.0f;
        check (q.armRecording (5.0), "arm recording");
        run (q, 10); run (q, 400, 36);
        q.stopRecording();
        auto f = q.saveRecording();
        bool ok = f.existsAsFile();
        float peak = 0;
        if (ok) { auto s = q.loadSample (f); ok = s != nullptr; if (ok) peak = s->audio.getMagnitude (0, s->audio.getNumSamples()); }
        check (ok && peak > 0.1f, "resample: recording saved as WAV with the pad's audio");
        if (ok) f.deleteFile();

        q.recThreshold = 0.2f;
        q.armRecording (5.0);
        run (q, 50);
        check (q.recState.load() == PadRomplerProcessor::recArmed && q.recPos.load() == 0, "threshold: waits while silent");
        run (q, 50, 36);
        check (q.recState.load() == PadRomplerProcessor::recRecording && q.recPos.load() > 0, "threshold: starts on sound");
        q.stopRecording(); auto f2 = q.saveRecording(); f2.deleteFile();
    }

    {   // ---------------- tap chop
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (B, makeTone (1.0, 880.0f), 0, 1, "padB");
        auto song = makeTone (2.0, 220.0f);
        q.tapMode = PadRomplerProcessor::tapPads;
        q.startTap (song, 0.0);
        const float playing = run (q, 100);                       // 6400 samples in
        check (playing > 0.1f && q.tapPlaying.load(), "tap: sample plays back");
        AudioBuffer<float> b (2, BS); MidiBuffer m;
        m.addEvent (MidiMessage::noteOn (1, 37, 1.0f), 32);
        q.processBlock (b, m);
        PadRomplerProcessor::TapEvent ev[8];
        const int n = q.popTapEvents (ev, 8);
        check (n == 1 && ev[0].pad == B && std::abs (ev[0].pos - (6400 + 32)) < 2.0, "tap: pad hit captured at the exact sample position");
        // the pad itself must not sound: compare against the song alone
        q.stopTap(); run (q, 2); silence (q);
        check (run (q, 50) < 1e-6f, "tap: pad hit did not trigger the pad's sound");
        q.startTap (song, song->audio.getNumSamples() - 1000.0);
        run (q, 40);
        check (! q.tapPlaying.load() && q.tapReachedEnd.load(), "tap: stops at the end of the sample");
        q.tapMode = PadRomplerProcessor::tapOff;
    }

    {   // ---------------- chopping
        SampleData s; s.sampleRate = SR; const int n = (int) (2.0 * SR); s.audio.setSize (2, n); s.audio.clear();
        const int at[] = { 0, 24000, 48000, 72000 };
        for (int h : at) for (int i = 0; i < 4000; ++i)
        { const float v = 0.8f * std::exp (-i / 600.0f) * std::sin (i * 0.3f); s.audio.setSample (0, h + i, v); s.audio.setSample (1, h + i, v); }
        auto t = SamplerPage::detectTransients (s, 0.6f);
        bool good = t.size() == 3;
        for (size_t i = 0; good && i < t.size(); ++i) good = std::abs (t[i] - at[i + 1]) < 300;
        check (good, "transient chop finds the 3 hits after the first (within 6 ms)");
        auto e = SamplerPage::equalSlices (n, 4);
        check (e.size() == 3 && e[0] == n / 4 && e[2] == 3 * n / 4, "equal slices: 4 slices = 3 markers");
    }

    std::printf ("%d failures\n", fails);
    return fails;
}
