#include "../Source/PluginProcessor.h"
#include "../Source/SamplerPage.h"
#include "../Source/Network.h"
#include "../Source/PluginEditor.h"
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

    {   // ---------------- chance, conditions, fills, micro-timing
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.01, 1000.0f), 0, 1, "a");
        q.assignRegion (B, makeTone (0.01, 900.0f), 0, 1, "b");
        q.assignRegion (C, makeTone (0.01, 800.0f), 0, 1, "c");
        FakePlayHead ph; q.setPlayHead (&ph);
        auto& pat = q.patterns[0];
        pat.length = 1; pat.stepsPer2Beats = 16;                      // one 1/32 step, looping
        pat.setStep (A, 0, 100); pat.chance[A][0] = 50;
        const uint32_t h0 = hits (q, 36);
        while (ph.ppq < 25.0) run (q, 1, -1, 1, &ph);                // 200 loops
        const int played = (int) (hits (q, 36) - h0);
        check (played > 70 && played < 130, ("chance 50%: plays about half the time (" + String (played) + " of 200)").toRawUTF8());

        ph.playing = false; run (q, 2, -1, 1, &ph);
        pat.clear(); pat.length = 4; pat.stepsPer2Beats = 8;          // 4 x 1/16 = 1 beat per loop
        pat.setStep (A, 0, 100); pat.cond[A][0] = Seq::of4b;           // 2nd of every 4 loops
        pat.setStep (B, 0, 100); pat.cond[B][0] = Seq::fillOnly;
        pat.setStep (C, 0, 100); pat.cond[C][0] = Seq::notFill;
        ph.ppq = 0.0; ph.playing = true;
        const uint32_t a0 = hits (q, 36), b0 = hits (q, 37), c0 = hits (q, 38);
        while (ph.ppq < 0.99) run (q, 1, -1, 1, &ph);
        const bool notLoop1 = hits (q, 36) == a0;
        while (ph.ppq < 1.99) run (q, 1, -1, 1, &ph);
        const bool loop2 = hits (q, 36) == a0 + 1;
        while (ph.ppq < 7.99) run (q, 1, -1, 1, &ph);
        check (notLoop1 && loop2 && hits (q, 36) == a0 + 2, "condition 2nd-of-4: plays on loops 2 and 6 only");
        check (hits (q, 37) == b0 && hits (q, 38) == c0 + 8, "fill off: fill steps rest, not-fill steps play");
        q.fillOn = true;
        const uint32_t b1 = hits (q, 37), c1 = hits (q, 38);
        while (ph.ppq < 9.99) run (q, 1, -1, 1, &ph);
        check (hits (q, 37) == b1 + 2 && hits (q, 38) == c1, "fill on: fill steps play, not-fill steps rest");
        q.fillOn = false;

        ph.playing = false; run (q, 2, -1, 1, &ph);
        pat.clear(); pat.length = 16;
        pat.setStep (A, 1, 100); pat.nudge[A][1] = 128 + 50;           // step 2 (ppq 0.25), half a step late -> 0.375
        pat.setStep (B, 3, 100); pat.nudge[B][3] = 128 - 50;           // step 4 (ppq 0.75), half a step early -> 0.625
        ph.ppq = 0.0; ph.playing = true;
        const uint32_t n0 = hits (q, 36), m0 = hits (q, 37);
        while (ph.ppq < 0.36) run (q, 1, -1, 1, &ph);
        const bool lateNotYet = hits (q, 36) == n0;
        while (ph.ppq < 0.39) run (q, 1, -1, 1, &ph);
        check (lateNotYet && hits (q, 36) == n0 + 1, "timing: a late step lands half a step later");
        while (ph.ppq < 0.61) run (q, 1, -1, 1, &ph);
        const bool earlyNotYet = hits (q, 37) == m0;
        while (ph.ppq < 0.64) run (q, 1, -1, 1, &ph);
        check (earlyNotYet && hits (q, 37) == m0 + 1, "timing: an early step lands half a step sooner");

        MemoryBlock mb; pat.chance[A][1] = 33; pat.cond[A][1] = Seq::fillOnly;
        q.getStateInformation (mb);
        PadRomplerProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        check (r.patterns[0].chance[A][1].load() == 33 && r.patterns[0].cond[A][1].load() == Seq::fillOnly
               && r.patterns[0].nudge[A][1].load() == 178 && r.patterns[0].nudge[B][3].load() == 78, "state keeps chance, conditions and timing");
        q.setPlayHead (nullptr);
    }

    {   // ---------------- Capture
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.01, 1000.0f), 0, 1, "a");
        q.assignRegion (B, makeTone (0.01, 900.0f), 0, 1, "b");
        check (q.captureToPattern() == -1, "Capture: says so when the pattern isn't playing");
        FakePlayHead ph; q.setPlayHead (&ph);
        while (ph.ppq < 0.25) run (q, 1, -1, 1, &ph);
        run (q, 1, 36, 0.9f, &ph);                                     // just after step 2
        while (ph.ppq < 0.49) run (q, 1, -1, 1, &ph);
        run (q, 1, 37, 0.6f, &ph);                                     // just before step 3
        while (ph.ppq < 4.1) run (q, 1, -1, 1, &ph);
        const int n = q.captureToPattern();
        auto& pat = q.patterns[0];
        int on = 0; for (int p = 0; p < 64; ++p) for (int st = 0; st < 64; ++st) on += pat.vel[p][st].load() > 0 ? 1 : 0;
        check (n == 2 && pat.vel[A][1].load() > 100 && pat.vel[B][2].load() > 60 && pat.vel[B][2].load() < 90 && on == 2,
               "Capture: the last loop of playing lands on the right steps with its velocity");
        q.setPlayHead (nullptr);
    }

    {   // ---------------- Roll
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.01, 1000.0f), 0, 1, "a");
        q.internalBpm = 120.0; q.rollDiv = 4;                          // 1/16 at 120 BPM = 125 ms
        const uint32_t h0 = hits (q, 36);
        run (q, 1, 36, 1.0f);
        run (q, (int) (0.51 * SR / BS));
        const uint32_t held = hits (q, 36) - h0;
        AudioBuffer<float> b (2, BS); MidiBuffer m; m.addEvent (MidiMessage::noteOff (1, 36), 0); q.processBlock (b, m);
        const uint32_t h1 = hits (q, 36);
        run (q, (int) (0.5 * SR / BS));
        check (held == 5 && hits (q, 36) == h1, ("Roll while stopped: holding for half a second repeats every 1/16, letting go stops (" + String ((int) held) + ")").toRawUTF8());

        FakePlayHead ph; q.setPlayHead (&ph);
        const uint32_t h2 = hits (q, 36);
        run (q, 1, 36, 1.0f, &ph);
        while (ph.ppq < 0.99) run (q, 1, -1, 1, &ph);
        check (hits (q, 36) - h2 == 4, "Roll while playing: repeats land on the 1/16 grid");
        MidiBuffer m2; m2.addEvent (MidiMessage::noteOff (1, 36), 0); q.processBlock (b, m2);
        q.rollDiv = 0;
        q.setPlayHead (nullptr);
    }

    {   // ---------------- undo / redo
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        PatternHistory hist (q);
        auto settle = [&] { for (int i = 0; i < 6; ++i) hist.tick(); Thread::sleep (450); for (int i = 0; i < 6; ++i) hist.tick(); };
        check (! hist.canUndo(), "undo: nothing to undo at first");
        q.patterns[0].setStep (A, 0, 100);
        settle();
        q.patterns[0].setStep (A, 4, 100); q.patterns[0].chance[A][4] = 50;
        settle();
        check (hist.canUndo(), "undo: edits are recorded");
        hist.undo();
        check (q.patterns[0].vel[A][4].load() == 0 && q.patterns[0].vel[A][0].load() == 100, "undo: takes back the last change only");
        hist.redo();
        check (q.patterns[0].vel[A][4].load() == 100 && q.patterns[0].chance[A][4].load() == 50, "redo: puts it back, settings included");
        q.patterns[0].setStep (A, 8, 90);                               // not settled yet
        hist.tick(); hist.tick(); hist.tick();
        hist.undo();
        check (q.patterns[0].vel[A][8].load() == 0 && q.patterns[0].vel[A][4].load() == 100, "undo: also takes back a change made a moment ago");
        q.patterns[2].setStep (B, 1, 100);
        settle();
        hist.undo();
        check (q.patterns[2].vel[B][1].load() == 0 && q.currentPattern.load() == 2, "undo: a change in another pattern switches to it");
        MemoryBlock mb; q.getStateInformation (mb); q.setStateInformation (mb.getData(), (int) mb.getSize());
        hist.tick();
        check (! hist.canUndo() && ! hist.canRedo(), "undo: loading a kit or project starts fresh");
    }

    {   // ---------------- 1/4 and 1/4-triplet steps
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.02, 1000.0f), 0, 1, "click");
        FakePlayHead ph; q.setPlayHead (&ph);
        auto& pat = q.patterns[0];
        pat.stepsPer2Beats = 2;                                       // 1/4
        pat.vel[A][1] = 100;
        const uint32_t h0 = hits (q, 36);
        while (ph.ppq < 0.98) run (q, 1, -1, 1, &ph);
        const bool early = hits (q, 36) == h0;
        while (ph.ppq < 1.02) run (q, 1, -1, 1, &ph);
        check (early && hits (q, 36) == h0 + 1, "1/4 steps: step 2 lands on beat 2");

        ph.playing = false; run (q, 2, -1, 1, &ph);
        pat.clear(); pat.stepsPer2Beats = 3;                          // 1/4 triplet
        pat.vel[A][1] = 100;
        ph.ppq = 0.0; ph.playing = true;
        const uint32_t h1 = hits (q, 36);
        while (ph.ppq < 0.65) run (q, 1, -1, 1, &ph);
        const bool early3 = hits (q, 36) == h1;
        while (ph.ppq < 0.69) run (q, 1, -1, 1, &ph);
        check (early3 && hits (q, 36) == h1 + 1, "1/4 triplets: step 2 lands at 2/3 of a beat");
        q.setPlayHead (nullptr);

        MemoryBlock mb; q.getStateInformation (mb);
        PadRomplerProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        check (r.patterns[0].stepsPer2Beats.load() == 3, "state keeps 1/4-triplet steps");

        auto xml = AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        bool edited = false;
        if (xml != nullptr)
            for (auto* e = xml->getFirstChildElement(); e != nullptr; e = e->getNextElement())
                for (auto* pe : e->getChildWithTagNameIterator ("Pattern"))
                    if (pe->getIntAttribute ("index", -1) == 0) { pe->removeAttribute ("stepsPer2Beats"); pe->setAttribute ("stepsPerBeat", 2); edited = true; }
        MemoryBlock old; if (xml != nullptr) AudioProcessor::copyXmlToBinary (*xml, old);
        PadRomplerProcessor o; o.setStateInformation (old.getData(), (int) old.getSize());
        check (edited && o.patterns[0].stepsPer2Beats.load() == 4, "older projects: 1/8 steps load as 1/8");
    }

    {   // ---------------- TempoPAD controller modes (MMC transport, User = steps, Easy = patterns)
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (0.05, 1000.0f), 0, 1, "click");
        auto send = [&] (const MidiMessage& m) { AudioBuffer<float> b (2, BS); MidiBuffer mb; mb.addEvent (m, 0); q.processBlock (b, mb); };
        auto mmc = [&] (uint8_t cmd) { const uint8_t d[] = { 0x7F, 0x7F, 0x06, cmd }; send (MidiMessage::createSysExMessage (d, 4)); };
        auto easy = [&] (uint8_t v) { send (MidiMessage::createSysExMessage (&v, 1)); };

        mmc (0x02);
        check (q.internalPlaying.load(), "MMC Play starts FlipKit64's own transport");
        mmc (0x06);
        check (q.seqRecord.load(), "MMC Record arms live record");
        mmc (0x01);
        check (! q.internalPlaying.load() && ! q.seqRecord.load(), "MMC Stop stops and disarms record");
        mmc (0x06);
        check (q.seqRecord.load() && q.internalPlaying.load(), "MMC Record while stopped starts playing and recording");
        mmc (0x01);

        send (MidiMessage::noteOn (1, 40, 1.0f));
        check (q.ctrlMode.load() == PadRomplerProcessor::ctrlPads && q.stepTarget.load() == padOfNote (40),
               "Drum-mode hit picks the pad that User mode will program");
        q.stepTarget = A;
        const int topLeft = PadLayout::noteForPad (0);                  // first pad of the grid = step 1
        const uint32_t h0 = hits (q, 36);
        send (MidiMessage::noteOn (3, topLeft, 1.0f));
        check (q.ctrlMode.load() == PadRomplerProcessor::ctrlSteps && q.patterns[0].vel[A][0].load() > 0,
               "User mode (channel 3): a pad press turns step 1 on for the selected pad");
        check (hits (q, 36) == h0 + 1, "User mode: turning a step on plays the sound once");
        send (MidiMessage::noteOn (3, PadLayout::noteForPad (5), 1.0f));
        check (q.patterns[0].vel[A][5].load() > 0, "User mode: sixth pad = step 6");
        send (MidiMessage::noteOn (3, topLeft, 1.0f));
        check (q.patterns[0].vel[A][0].load() == 0, "User mode: pressing it again turns the step off");
        send (MidiMessage::noteOn (3, PadLayout::noteForPad (40), 1.0f));
        check (q.patterns[0].vel[A][40].load() == 0, "User mode: steps past the pattern length are ignored");

        easy (0x3F);                                                    // top-right pad
        check (q.ctrlMode.load() == PadRomplerProcessor::ctrlPatterns && q.currentPattern.load() == 7,
               "Easy mode: top-right pad selects pattern 8");
        easy (0x7F);
        check (q.currentPattern.load() == 7, "Easy mode: pad release does nothing");
        const uint32_t h1 = hits (q, 36);
        easy (0x00);                                                    // bottom-left pad
        check (hits (q, 36) == h1 + 1 && q.currentPattern.load() == 7, "Easy mode: other pads still play");

        q.tempoPadModes = false;
        const uint32_t h2 = hits (q, 36);
        send (MidiMessage::noteOn (3, 36, 1.0f));
        check (hits (q, 36) == h2 + 1 && q.patterns[7].vel[A][0].load() == 0, "TempoPAD modes off: channel 3 just plays");
        easy (0x3C);
        check (q.currentPattern.load() == 7, "TempoPAD modes off: Easy-mode messages are ignored");
    }

    if (std::getenv ("DISPLAY") != nullptr)
    {   // ---------------- editor follows the TempoPAD's mode buttons
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        std::unique_ptr<AudioProcessorEditor> ed (q.createEditor());
        auto visiblePage = [&]() -> String {
            for (auto* c : ed->getChildren())
                if (c->isVisible()) {
                    if (dynamic_cast<SequencerPage*> (c)) return "seq";
                    if (dynamic_cast<PadsPage*> (c)) return "pads";
                    if (dynamic_cast<SamplerPage*> (c)) return "sampler";
                }
            return "?";
        };
        auto send = [&] (const MidiMessage& m) { AudioBuffer<float> b (2, BS); MidiBuffer mb; mb.addEvent (m, 0); q.processBlock (b, mb); };
        const bool was = q.tempoPadModes.load();
        q.tempoPadModes = true;
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        const String start = visiblePage();
        send (MidiMessage::noteOn (1, 45, 1.0f));
        MessageManager::getInstance()->runDispatchLoopUntil (100);
        send (MidiMessage::noteOn (3, PadLayout::noteForPad (2), 1.0f));
        MessageManager::getInstance()->runDispatchLoopUntil (150);
        const String inSteps = visiblePage();
        if (auto* snap = std::getenv ("FLIPKIT_SNAP"))
        {
            ed->setSize (1280, 780);
            auto& sp = q.patterns[0];
            const int k = padOfNote (36), s2 = padOfNote (37), hh = padOfNote (38);
            q.assignRegion (k, makeTone (0.05, 60.0f), 0, 1, "Kick");
            q.assignRegion (s2, makeTone (0.05, 200.0f), 0, 1, "Snare");
            q.assignRegion (hh, makeTone (0.05, 4000.0f), 0, 1, "Closed hat");
            for (int st : { 0, 7, 10 }) sp.setStep (k, st, 118);
            sp.nudge[k][7] = 138;
            sp.setStep (k, 15, 90); sp.cond[k][15] = Seq::of4a;
            for (int st : { 4, 12 }) sp.setStep (s2, st, 120);
            sp.setStep (s2, 14, 70); sp.chance[s2][14] = 50;
            for (int st = 0; st < 16; st += 2) sp.setStep (hh, st, st % 4 == 0 ? 100 : 72);
            sp.ratchet[hh][14] = 3;
            for (auto* c : ed->getChildren())
                if (auto* seqPage = dynamic_cast<SequencerPage*> (c))
                {
                    seqPage->getGrid().selPad = s2; seqPage->getGrid().selStep = 14;
                    seqPage->padsChanged(); seqPage->stepChanged();
                }
            MessageManager::getInstance()->runDispatchLoopUntil (200);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds());
            const File snapFile { String (snap) }; FileOutputStream fo { snapFile }; fo.setPosition (0); fo.truncate();
            PNGImageFormat().writeImageToStream (img, fo);
        }
        auto* pe = dynamic_cast<PadRomplerEditor*> (ed.get());
        check (start == "pads" && inSteps == "seq" && pe != nullptr && pe->getSelectedPad() == padOfNote (45)
               && q.patterns[0].vel[padOfNote (45)][2].load() > 0,
               "editor: User mode opens the sequencer on the pad you last hit");
        send (MidiMessage::noteOn (1, 45, 1.0f));
        MessageManager::getInstance()->runDispatchLoopUntil (150);
        check (visiblePage() == "pads", "editor: back to Drum mode returns to the Pads tab");
        q.tempoPadModes = was;
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



    {   // ---------------- browser player
        PadRomplerProcessor p; p.prepareToPlay (SR, BS);
        auto tone = makeTone (1.0, 220.0f);                       // 1 s
        p.playerLoad (tone, true);
        check (run (p, 50) > 0.05f && p.playerIsPlaying(), "player: plays when loaded");
        const double at = p.playerPositionFraction();
        check (at > 0.05 && at < 0.1, "player: position advances (about 67 ms in)");
        p.playerPause(); run (p, 20);
        const double paused = p.playerPositionFraction();
        check (run (p, 50) < 1e-4f && std::abs (p.playerPositionFraction() - paused) < 1e-9, "player: pause is silent and holds position");
        p.playerPlay();
        check (run (p, 20) > 0.05f && p.playerPositionFraction() > paused, "player: resumes from the paused spot");
        p.playerSeek (0.5); run (p, 1);
        check (std::abs (p.playerPositionFraction() - 0.5) < 0.01, "player: seek jumps to the middle");
        p.playerStop(); run (p, 20);
        check (! p.playerIsPlaying() && p.playerPositionFraction() == 0.0 && run (p, 20) < 1e-4f, "player: stop is silent and rewinds");
        p.playerPlay(); run (p, 900);                             // 0.92 s past a 1 s sample
        check (! p.playerIsPlaying() && p.playerPositionFraction() == 0.0, "player: stops at the end without loop");
        p.playerSetLoop (true); p.playerPlay(); run (p, 900);
        check (p.playerIsPlaying() && run (p, 20) > 0.05f, "player: keeps going with loop on");
        AudioBuffer<float> b (2, BS); MidiBuffer m;
        p.playerSetLoop (false); p.playerPause();
        float firstAbs = 0; for (int i = 0; i < 20; ++i) { p.processBlock (b, m); }
        p.playerPlay(); p.processBlock (b, m); firstAbs = std::abs (b.getSample (0, 0));
        check (firstAbs < 0.01f, "player: fades in (no click when starting)");
    }
   #if FLIPKIT_NETWORK
    if (auto* srv = std::getenv ("FLIPKIT_SMB_TEST"))   // network tests against a test file share
    {
        auto waitList = [] (Net::Library& lib, const Net::Location& l, const String& path, bool& ok, Array<Net::Entry>& out, String& err)
        {
            bool done = false;
            lib.list (l, path, [&] (bool o, Array<Net::Entry> e, String er) { ok = o; out = e; err = er; done = true; });
            for (int i = 0; i < 300 && ! done; ++i) MessageManager::getInstance()->runDispatchLoopUntil (50);
            return done;
        };
        Net::cacheRoot().deleteRecursively();
        Net::SharedLibrary lib;
        Net::Location loc; loc.name = "PC Music"; loc.server = srv; loc.share = "Music"; loc.user = "sean"; loc.password = "Beat$Maker1";
        lib->saveLocation (loc);

        bool ok = false; Array<Net::Entry> e; String err;
        check (waitList (*lib, loc, "", ok, e, err) && ok, "network: list share root");
        StringArray names; for (auto& x : e) names.add (x.name + (x.isDir ? "/" : ""));
        check (names.contains ("Demo Kit/") && names.contains ("Sample Packs/") && ! names.contains ("readme.txt"),
               "network: folders listed, non-audio files hidden");
        check (waitList (*lib, loc, String::fromUTF8 ("Sample Packs/Lo-Fi \xc3\x9cn\xc3\xaf" "code Kit"), ok, e, err) && ok
               && e.size() == 1 && e[0].name == String::fromUTF8 ("Sn\xc3\xa4re #2.wav"), "network: accented folder and file names");

        String ferr;
        auto f = lib->fetchNow (loc, "Demo Kit/Kick.wav", ferr);
        const File src ("/srv/smbtest/Music/Demo Kit/Kick.wav");
        MemoryBlock a, b; f.loadFileAsData (a); src.loadFileAsData (b);
        check (f.existsAsFile() && a == b, "network: downloaded file is byte-identical");
        check (f == Net::cacheFileFor (loc, "Demo Kit/Kick.wav"), "network: download lands in the cache path");
        const auto t1 = f.getLastModificationTime();
        Thread::sleep (1100);
        lib->fetchNow (loc, "Demo Kit/Kick.wav", ferr);
        check (f.getLastModificationTime() == t1, "network: unchanged file is not downloaded twice");

        auto big = lib->fetchNow (loc, "Records/Rainy Keys 88bpm.wav", ferr);
        check (big.getSize() == File ("/srv/smbtest/Music/Records/Rainy Keys 88bpm.wav").getSize(), "network: multi-chunk download (1.9 MB)");
        auto uni = lib->fetchNow (loc, String::fromUTF8 ("Sample Packs/Lo-Fi \xc3\x9cn\xc3\xaf" "code Kit/Sn\xc3\xa4re #2.wav"), ferr);
        check (uni.existsAsFile(), "network: download with accented path");

        String sv, sh, rp;
        check (Net::fromCacheFile (f, sv, sh, rp) && sv == String (srv).toLowerCase() && sh == "music" && rp == "Demo Kit/Kick.wav",
               "network: cache path maps back to the share");

        {   // a kit referencing a network sample whose cached copy was deleted
            PadRomplerProcessor p; p.prepareToPlay (SR, BS);
            f.deleteFile();
            p.assignFiles (A, { f }, false);
            check (f.existsAsFile() && ! p.getPad (A)->layers.empty(), "network: missing cached sample is re-downloaded on load");
        }

        auto bad = loc; bad.password = "wrong";
        lib->saveLocation (bad);
        bool done = waitList (*lib, bad, "", ok, e, err);
        check (done && ! ok && err.containsIgnoreCase ("username or password"), ("network: wrong password -> " + err).toRawUTF8());
        lib->saveLocation (loc);

        auto noShare = loc; noShare.share = "Nope";
        done = waitList (*lib, noShare, "", ok, e, err);
        check (done && ! ok && err.containsIgnoreCase ("no shared folder"), ("network: wrong share -> " + err).toRawUTF8());

        auto noHost = loc; noHost.server = "DESKTOP-DOESNOTEXIST";
        done = waitList (*lib, noHost, "", ok, e, err);
        check (done && ! ok && err.containsIgnoreCase ("IP address"), ("network: unknown PC -> " + err).toRawUTF8());

        auto deadHost = loc; deadHost.server = "127.0.0.1:1";
        done = waitList (*lib, deadHost, "", ok, e, err);
        check (done && ! ok && err.contains ("Couldn't reach"), ("network: PC not answering -> " + err).toRawUTF8());

        check (waitList (*lib, loc, "No Such Folder", ok, e, err) && ! ok && err.contains ("isn't there"), ("network: missing folder -> " + err).toRawUTF8());

        check (waitList (*lib, loc, "Demo Kit", ok, e, err) && ok && e.size() == 9, "network: reconnects fine after errors");
    }
   #endif
    std::printf ("%d failures\n", fails);
    return fails;
}
