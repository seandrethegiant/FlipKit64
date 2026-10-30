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

// a drum-ish loop: a decaying click on every beat, louder on the bar
static SamplePtr makeLoop (double bpm, int beats)
{
    auto s = std::make_shared<SampleData>();
    s->sampleRate = SR;
    const int beat = (int) std::round (SR * 60.0 / bpm);
    s->audio.setSize (2, beat * beats);
    s->audio.clear();
    for (int b = 0; b < beats; ++b)
        for (int i = 0; i < 3000 && b * beat + i < s->audio.getNumSamples(); ++i)
        {
            const float v = (b % 4 == 0 ? 0.9f : 0.5f) * std::exp (-i / 500.0f) * std::sin (i * 0.25f);
            s->audio.setSample (0, b * beat + i, v); s->audio.setSample (1, b * beat + i, v);
        }
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
        p.updatePad (A, [] (PadState& s) { s.end = 1.0f; s.decayMs = 100.0f; s.sustain = 0.0f; });
        silence (p); run (p, 1, 36); run (p, 200); const float afterDecay = run (p, 20);
        check (afterDecay < 0.01f, "decay: 100 ms decay to sustain 0 fades the hit");

        // ADSR (0.9.8)
        p.updatePad (A, [] (PadState& s) { s.resetEnvelope(); });
        silence (p); run (p, 1, 36); run (p, 40); const float plain = run (p, 20);
        p.updatePad (A, [] (PadState& s) { s.decayMs = 50.0f; s.sustain = 0.5f; });
        silence (p); run (p, 1, 36); run (p, 40); const float half = run (p, 20);
        check (std::abs (half / plain - 0.5f) < 0.05f, ("sustain: decays to the sustain level and stays there (" + String (half / plain, 2) + ")").toRawUTF8());
        p.updatePad (A, [] (PadState& s) { s.resetEnvelope(); s.attackMs = 300.0f; });
        silence (p); run (p, 1, 36); const float rising = run (p, 4); run (p, 40); const float risen = run (p, 20);
        check (rising < risen * 0.3f, "attack: fades in");
        p.updatePad (A, [] (PadState& s) { s.attackMs = 300.0f; s.curveA = 1.0f; });
        silence (p); run (p, 1, 36); const float fastRise = run (p, 4);
        p.updatePad (A, [] (PadState& s) { s.curveA = -1.0f; });
        silence (p); run (p, 1, 36); const float slowRise = run (p, 4);
        check (fastRise > slowRise * 3.0f, "curves: a bent attack rises faster or slower");
        p.updatePad (A, [] (PadState& s) { s.resetEnvelope(); });

        {   // kits from before 0.9.8 sound the same
            p.updatePad (A, [] (PadState& s) { s.decayMs = 250.0f; s.sustain = 0.0f; });
            p.updatePad (B, [] (PadState& s) { s.decayMs = 5000.0f; });
            auto x = p.kitToXml();
            for (auto* pe : x->getChildWithTagNameIterator ("Pad"))
                for (auto* a : { "sustain", "release", "curveA", "curveD", "curveR" }) pe->removeAttribute (a);
            PadRomplerProcessor o; o.kitFromXml (*x);
            check (o.getPad (A)->sustain == 0.0f && o.getPad (A)->curveD == 1.0f && o.getPad (A)->decayMs == 250.0f
                   && o.getPad (B)->sustain == 1.0f, "old kits: Decay becomes a fade to silence, Decay off plays the whole sound");
            PadRomplerProcessor n;
            p.updatePad (A, [] (PadState& s) { s.sustain = 0.4f; s.releaseMs = 700.0f; s.curveR = -0.3f; });
            n.kitFromXml (*p.kitToXml());
            check (n.getPad (A)->sustain == 0.4f && n.getPad (A)->releaseMs == 700.0f && std::abs (n.getPad (A)->curveR + 0.3f) < 1e-6f,
                   "kits keep the envelope");
            p.updatePad (A, [] (PadState& s) { s.resetEnvelope(); });
            p.updatePad (B, [] (PadState& s) { s.resetEnvelope(); });
        }
        p.updatePad (A, [] (PadState& s) { s.cutoff = 100.0f; s.reso = 0.0f; });
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

    {   // ---------------- pad play modes, Stop and Clear all
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (3.0, 440.0f), 0, 1, "long");
        auto send = [&] (const MidiMessage& m) { AudioBuffer<float> b (2, BS); MidiBuffer mb; mb.addEvent (m, 0); q.processBlock (b, mb); };
        auto level = [&] (double seconds) { return run (q, (int) (seconds * SR / BS)); };
        auto setPlay = [&] (PlayMode m) { q.updatePad (A, [m] (PadState& p) { p.play = m; }); };

        send (MidiMessage::noteOn (1, 36, 1.0f));
        level (0.1);
        send (MidiMessage::noteOff (1, 36));
        level (0.1);
        check (level (0.2) > 0.05, "one-shot: keeps playing after the pad is let go");
        q.stopAllSounds(); level (0.1);
        check (level (0.2) < 1e-4, "Stop sounds: silences a long one-shot even while stopped");

        setPlay (PlayMode::Hold);
        send (MidiMessage::noteOn (1, 36, 1.0f));
        const float holding = level (0.3);
        send (MidiMessage::noteOff (1, 36));
        level (0.1);
        check (holding > 0.05 && level (0.2) < 1e-4, "Hold: plays while held, stops when let go");

        q.updatePad (A, [] (PadState& s) { s.releaseMs = 600.0f; });  // the release rings out after letting go
        send (MidiMessage::noteOn (1, 36, 1.0f)); level (0.2);
        send (MidiMessage::noteOff (1, 36));
        const float tail = level (0.15); level (0.5);
        check (tail > holding * 0.1f && level (0.2) < 1e-4, "release: a Hold pad rings out for its release time, then stops");
        q.updatePad (A, [] (PadState& s) { s.releaseMs = 100.0f; });

        setPlay (PlayMode::Toggle);
        send (MidiMessage::noteOn (1, 36, 1.0f)); send (MidiMessage::noteOff (1, 36));
        const float on1 = level (0.3);
        send (MidiMessage::noteOn (1, 36, 1.0f)); send (MidiMessage::noteOff (1, 36));
        level (0.1);
        const float off1 = level (0.2);
        send (MidiMessage::noteOn (1, 36, 1.0f));
        check (on1 > 0.05 && off1 < 1e-4 && level (0.2) > 0.05, "Toggle: first press starts, second stops, third starts again");
        q.stopAllSounds(); level (0.1);

        setPlay (PlayMode::Hold);                                      // from the sequencer a Hold pad lasts one step
        q.patterns[0].setStep (A, 0, 110);
        FakePlayHead ph; q.setPlayHead (&ph);
        run (q, (int) (0.1 * SR / BS), -1, 1, &ph);
        const float stepOn = run (q, 1, -1, 1, &ph);
        while (ph.ppq < 0.45) run (q, 1, -1, 1, &ph);
        check (stepOn > 0.05 && run (q, (int) (0.1 * SR / BS), -1, 1, &ph) < 1e-4, "Hold: a sequenced step lasts one step");
        q.patterns[0].clear();

        setPlay (PlayMode::OneShot);
        send (MidiMessage::noteOn (1, 36, 1.0f));
        run (q, 4, -1, 1, &ph);
        ph.playing = false;                                            // LUNA (or Play) stops
        run (q, (int) (0.1 * SR / BS), -1, 1, &ph);
        check (run (q, (int) (0.2 * SR / BS), -1, 1, &ph) < 1e-4, "Stop: stopping the transport fades out what's still playing");
        q.setPlayHead (nullptr);

        send (MidiMessage::noteOn (1, 36, 1.0f)); level (0.05);
        const uint8_t d[] = { 0x7F, 0x7F, 0x06, 0x01 };
        send (MidiMessage::createSysExMessage (d, 4)); level (0.1);
        check (level (0.2) < 1e-4, "TempoPAD Stop (MMC) silences everything");

        setPlay (PlayMode::Toggle);
        MemoryBlock mb; q.getStateInformation (mb);
        PadRomplerProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        check (r.getPad (A)->play == PlayMode::Toggle, "state keeps each pad's play mode");

        q.patterns[1].setStep (B, 3, 100); q.keysTracks[0].sourcePad = A; q.patterns[0].lanes[0].set (q.makeNotes ({ { 60, 0, 2, 100 } }));
        q.clearAllPads();
        check (q.getPad (A)->layers.empty() && q.patterns[1].vel[B][3].load() == 100 && q.patterns[0].lanes[0].get() != nullptr,
               "Clear all pads: empties every pad, keeps the patterns");
        q.assignRegion (A, makeTone (0.1, 440.0f), 0, 1, "x");
        const auto loads = q.kitLoads.load();
        q.newKit();
        check (q.getPad (A)->layers.empty() && q.patterns[1].vel[B][3].load() == 0 && q.patterns[0].lanes[0].get() == nullptr
               && q.keysTracks[0].sourcePad.load() == -1 && q.kitLoads.load() == loads + 1, "New kit: clears pads, patterns and Keys tracks");
    }

    {   // ---------------- Keys tracks
        // render until the playhead reaches 'to', returning the left channel
        auto renderTo = [] (PadRomplerProcessor& p, FakePlayHead* ph, double to, MidiBuffer* first = nullptr)
        {
            std::vector<float> out; AudioBuffer<float> b (2, BS); MidiBuffer m;
            bool firstDone = false;
            while (ph == nullptr ? out.size() < (size_t) (to * SR) : ph->ppq < to)
            {
                if (first != nullptr && ! firstDone) { m = *first; firstDone = true; }
                p.processBlock (b, m); m.clear();
                if (ph) ph->ppq += BS * ph->bpm / 60.0 / SR;
                for (int s = 0; s < BS; ++s) out.push_back (b.getSample (0, s));
            }
            return out;
        };
        auto rms = [] (const std::vector<float>& x, size_t from, size_t to)
        {
            double sum = 0; to = jmin (to, x.size());
            for (size_t i = from; i < to; ++i) sum += x[i] * x[i];
            return to > from ? std::sqrt (sum / (double) (to - from)) : 0.0;
        };
        auto freq = [] (const std::vector<float>& x, size_t from, size_t to)    // rising zero crossings per second
        {
            int n = 0; to = jmin (to, x.size());
            for (size_t i = from + 1; i < to; ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
            return n / ((double) (to - from) / SR);
        };
        const size_t perBeat = (size_t) (SR * 60.0 / 120.0);

        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        q.assignRegion (A, makeTone (3.0, 440.0f), 0, 1, "keys");
        q.keysTracks[0].sourcePad = A; q.keysTracks[0].root = 60; q.keysTracks[0].mono = false;
        auto& lane = q.patterns[0].lanes[0];                           // 1/8 steps: step 2 = beat 1.0
        lane.set (q.makeNotes ({ { 72, 2, 2, 110 } }));                // C4, an octave up, 2 steps = 1 beat
        FakePlayHead ph; q.setPlayHead (&ph);
        auto x = renderTo (q, &ph, 2.6);
        check (rms (x, 0, (size_t) (0.95 * perBeat)) < 1e-4 && rms (x, (size_t) (1.05 * perBeat), (size_t) (1.9 * perBeat)) > 0.05,
               "Keys: a note plays on its step");
        const double f = freq (x, (size_t) (1.1 * perBeat), (size_t) (1.9 * perBeat));
        check (std::abs (f - 880.0) < 15.0, ("Keys: an octave above the root plays an octave up (" + String (f, 0) + " Hz)").toRawUTF8());
        check (rms (x, (size_t) (2.15 * perBeat), (size_t) (2.6 * perBeat)) < 1e-3, "Keys: the note stops after its length");

        ph.playing = false; renderTo (q, &ph, 0.0); renderTo (q, nullptr, 0.1);
        q.keysTracks[1].sourcePad = A; q.keysTracks[1].root = 60;     // Bass: mono with glide by default
        q.patterns[0].lanes[1].set (q.makeNotes ({ { 60, 0, 2, 110 }, { 67, 2, 2, 110 } }));
        lane.set (nullptr);
        ph.ppq = 0.0; ph.playing = true;
        auto y = renderTo (q, &ph, 1.95);
        const double f1 = freq (y, (size_t) (0.1 * perBeat), (size_t) (0.9 * perBeat));
        const double f2 = freq (y, (size_t) (1.3 * perBeat), (size_t) (1.9 * perBeat));
        const double mid = rms (y, (size_t) (0.98 * perBeat), (size_t) (1.02 * perBeat));
        check (std::abs (f1 - 440.0) < 10.0 && std::abs (f2 - 659.3) < 12.0 && mid > 0.05,
               ("Bass: glides from C to G without a gap (" + String (f1, 0) + " -> " + String (f2, 0) + " Hz)").toRawUTF8());

        ph.playing = false; renderTo (q, &ph, 0.0); renderTo (q, nullptr, 0.2);
        q.patterns[0].lanes[1].set (nullptr);
        q.keysInput = 0;                                               // pads play Keys 1
        MidiBuffer on; on.addEvent (MidiMessage::noteOn (1, 64, 1.0f), 0);
        auto z = renderTo (q, nullptr, 0.4, &on);
        const double fz = freq (z, (size_t) (0.05 * SR), (size_t) (0.35 * SR));
        MidiBuffer off; off.addEvent (MidiMessage::noteOff (1, 64), 0);
        auto z2 = renderTo (q, nullptr, 0.3, &off);
        check (rms (z, (size_t) (0.05 * SR), (size_t) (0.35 * SR)) > 0.05 && std::abs (fz - 554.4) < 12.0 && rms (z2, (size_t) (0.1 * SR), (size_t) (0.3 * SR)) < 1e-3,
               ("Keys from the pads: a held note sounds at its pitch and stops on release (" + String (fz, 0) + " Hz)").toRawUTF8());

        q.seqRecord = true; ph.ppq = 0.0; ph.playing = true;
        renderTo (q, &ph, 0.5);
        MidiBuffer on2; on2.addEvent (MidiMessage::noteOn (1, 67, 0.8f), 0);
        renderTo (q, &ph, 1.5, &on2);
        MidiBuffer off2; off2.addEvent (MidiMessage::noteOff (1, 67), 0);
        renderTo (q, &ph, 1.6, &off2);
        q.drainKeyRecordings();
        auto rec = lane.get();
        check (rec != nullptr && rec->size() == 1 && (*rec)[0].pitch == 67 && (*rec)[0].step == 1 && (*rec)[0].len == 2 && (*rec)[0].vel > 90 && (*rec)[0].vel < 110,
               "Keys recording: a held note lands on its step with its length");
        q.seqRecord = false; q.keysInput = -1;
        q.setPlayHead (nullptr);

        lane.length = 32; lane.stepsPer2Beats = 2;
        q.keysTracks[2].name = "Strings"; q.keysTracks[2].root = 48; q.scaleType = 2; q.scaleRoot = 9;
        MemoryBlock mb; q.getStateInformation (mb);
        PadRomplerProcessor r; r.setStateInformation (mb.getData(), (int) mb.getSize());
        auto rn = r.patterns[0].lanes[0].get();
        check (rn != nullptr && rn->size() == 1 && (*rn)[0].pitch == 67 && (*rn)[0].len == 2 && r.patterns[0].lanes[0].length.load() == 32
               && r.patterns[0].lanes[0].stepsPer2Beats.load() == 2 && r.keysTracks[0].sourcePad.load() == A && r.keysTracks[1].mono.load()
               && r.keysTracks[2].name == "Strings" && r.keysTracks[2].root.load() == 48 && r.scaleType.load() == 2 && r.scaleRoot.load() == 9,
               "state keeps Keys notes, lane lengths, track settings and the scale");

        PatternHistory hist (q);
        lane.set (q.makeNotes ({ { 60, 0, 4, 100 } }));
        for (int i = 0; i < 6; ++i) hist.tick();
        Thread::sleep (450);
        for (int i = 0; i < 6; ++i) hist.tick();
        hist.undo();
        auto un = lane.get();
        check (un != nullptr && un->size() == 1 && (*un)[0].pitch == 67, "undo covers Keys notes");
        check (Keys::inScale (2, 9, 69) && Keys::inScale (2, 9, 72) && ! Keys::inScale (2, 9, 70), "scales: A minor has A and C, not A#");
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
            if (auto* snap2 = std::getenv ("FLIPKIT_SNAP2"))
            {
                const int keysPad = padOfNote (44), bassPad = padOfNote (45);
                q.assignRegion (keysPad, makeTone (1.0, 262.0f), 0, 1, "Rhodes C3");
                q.assignRegion (bassPad, makeTone (1.0, 65.0f), 0, 1, "808 Sub C1");
                q.keysTracks[0].sourcePad = keysPad; q.keysTracks[1].sourcePad = bassPad; q.keysTracks[1].root = 36;
                q.scaleRoot = 0; q.scaleType = 2;
                q.patterns[0].lanes[0].set (q.makeNotes ({ { 60, 0, 6, 96 }, { 63, 0, 6, 88 }, { 67, 0, 6, 84 }, { 70, 0, 6, 80 },
                                                            { 56, 8, 6, 92 }, { 60, 8, 6, 86 }, { 63, 8, 6, 82 }, { 67, 8, 6, 78 } }));
                q.patterns[0].lanes[1].set (q.makeNotes ({ { 48, 0, 3, 120 }, { 48, 3, 1, 90 }, { 51, 6, 2, 104 }, { 56, 8, 4, 118 }, { 55, 13, 2, 100 } }));
                for (auto* c : ed->getChildren())
                    if (auto* seqPage = dynamic_cast<SequencerPage*> (c))
                    {
                        seqPage->padsChanged();
                        seqPage->repaint();
                        MessageManager::getInstance()->runDispatchLoopUntil (100);
                        auto img1 = ed->createComponentSnapshot (ed->getLocalBounds());
                        const File f1 { String (snap2) + "-drums.png" }; f1.deleteFile(); FileOutputStream o1 { f1 };
                        PNGImageFormat().writeImageToStream (img1, o1);
                        seqPage->selectTrack (2);
                        MessageManager::getInstance()->runDispatchLoopUntil (150);
                        auto img2 = ed->createComponentSnapshot (ed->getLocalBounds());
                        const File f2 { String (snap2) + "-bass.png" }; f2.deleteFile(); FileOutputStream o2 { f2 };
                        PNGImageFormat().writeImageToStream (img2, o2);
                        seqPage->selectTrack (0);
                    }
            }
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

    {   // ---------------- sampler slice tools (0.9.8)
        check (SamplerPage::bpmFromName ("Rainy Keys 88bpm") == 88.0 && SamplerPage::bpmFromName ("break_96_BPM_v2") == 96.0
               && SamplerPage::bpmFromName ("BPM120 loop") == 120.0 && SamplerPage::bpmFromName ("Kick 808") == 0.0,
               "BPM from file names (88bpm, 96_BPM, BPM120; none in 'Kick 808')");
        auto loop = makeLoop (100.0, 16);
        const double found = SamplerPage::detectBpm (*loop);
        check (std::abs (found - 100.0) < 0.6, ("BPM detection on a 16-beat loop at 100 (" + String (found, 1) + ")").toRawUTF8());
        auto g = SamplerPage::gridSlices (loop->audio.getNumSamples(), SR, 100.0, 1.0);
        check (g.size() == 15 && std::abs (g[0] - (int) (0.6 * SR)) <= 1, "beat grid: 16 beats = 16 slices, each 0.6 s at 100 BPM");
        check (SamplerPage::gridSlices (loop->audio.getNumSamples(), SR, 100.0, 4.0).size() == 3, "beat grid: 1 bar slices");
        auto tone = makeTone (0.1, 100.0f);
        const int zc = SamplerPage::nearestZeroCrossing (*tone, 200);
        check (std::abs (zc - (int) (SR / 200.0)) <= 1, ("zero-crossing snap finds the sine's crossing (" + String (zc) + ")").toRawUTF8());
        auto nrm = SamplerPage::normalized (*tone);
        check (std::abs (nrm->audio.getMagnitude (0, 0, nrm->audio.getNumSamples()) - 0.98f) < 0.01f, "normalize: peak to -0.2 dB");
        SampleData pad; pad.sampleRate = SR; pad.audio.setSize (2, 30000); pad.audio.clear();
        for (int i = 10000; i < 20000; ++i) { pad.audio.setSample (0, i, 0.5f); pad.audio.setSample (1, i, 0.5f); }
        int lead = 0; auto tr = SamplerPage::trimmed (pad, lead);
        check (tr != nullptr && lead == 10000 - 64 && tr->audio.getNumSamples() < 12000, "trim silence: cuts the quiet start and end");
        auto ps = SamplerPage::patternSteps ({ 0, (int) (0.6 * SR), (int) (1.2 * SR), (int) (1.5 * SR) }, SR, 100.0, 4.0, 64);
        check (ps.size() == 4 && ps[1].second == 4 && ps[2].second == 8 && ps[3].second == 10, "chop to pattern: slices land on their 16th-note steps");
        check (SamplerPage::patternSteps ({ 0, 100, 200 }, SR, 0.0, 4.0, 2).size() == 2, "chop to pattern: no BPM = one slice per step");

        PadRomplerProcessor q; q.prepareToPlay (SR, BS);           // Keys track in slice mode: one pad per key, unpitched
        const int first = padOfNote (36);
        q.assignRegion (first, makeTone (1.0, 220.0f), 0, 1, "s1");
        q.assignRegion (padOfNote (37), makeTone (1.0, 440.0f), 0, 1, "s2");
        q.keysTracks[2].sourcePad = first; q.keysTracks[2].root = 60; q.keysTracks[2].sliceCount = 2;
        auto freqOf = [&] (int pitch)
        {
            q.stopAllSounds(); run (q, 200);
            q.triggerKey (2, pitch, 1.0f);
            AudioBuffer<float> b (2, 8192); b.clear(); MidiBuffer mb;
            AudioBuffer<float> warm (2, BS); q.processBlock (warm, mb);
            q.processBlock (b, mb);
            if (b.getMagnitude (0, 0, 8192) < 1.0e-3f) return 0.0;
            int zx = 0; for (int i = 1; i < 8192; ++i) if (b.getSample (0, i - 1) <= 0 && b.getSample (0, i) > 0) ++zx;
            return zx * SR / 8192.0;
        };
        const double f0 = freqOf (60), f1 = freqOf (61), f2 = freqOf (62);
        check (std::abs (f0 - 220) < 15 && std::abs (f1 - 440) < 25 && f2 < 1.0,
               ("slice keys: C3 plays slice 1, C#3 slice 2, past the slices nothing (" + String (f0, 0) + ", " + String (f1, 0) + ", " + String (f2, 0) + ")").toRawUTF8());
        MemoryBlock mb2; q.getStateInformation (mb2);
        PadRomplerProcessor r2; r2.setStateInformation (mb2.getData(), (int) mb2.getSize());
        check (r2.keysTracks[2].sliceCount.load() == 2, "state keeps a Keys track's slice mode");

        q.updatePad (first, [] (PadState& x) { x.fadeOutMs = 400.0f; x.end = 0.5f; });   // fade out: quiet near the region end
        q.stopAllSounds(); run (q, 30);
        run (q, 1, 36); run (q, (int) (0.1 * SR / BS)); const float body = run (q, 4);
        run (q, (int) (0.28 * SR / BS)); const float nearEnd = run (q, 4);
        check (nearEnd < body * 0.35f, "slice fade out: the end of the region fades");
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
    if (std::getenv ("DISPLAY") != nullptr)
    {   // ---------------- Sampler page: beat-grid chop, chop to pattern, send to Keys, undo (0.9.8)
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        std::unique_ptr<AudioProcessorEditor> base (q.createEditor());
        auto* ed = dynamic_cast<PadRomplerEditor*> (base.get());
        ed->setSize (1280, 780);
        SamplerPage* sp = nullptr;
        for (auto* c : ed->getChildren()) if (auto* x = dynamic_cast<SamplerPage*> (c)) sp = x;
        auto loop = makeLoop (96.0, 8);
        const File wavFile = File::getSpecialLocation (File::tempDirectory).getChildFile ("Dusty Loop 96bpm.wav");
        wavFile.deleteFile();
        {
            WavAudioFormat wav; auto* os = wavFile.createOutputStream().release();
            std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os, SR, 2, 24, {}, 0));
            w->writeFromAudioSampleBuffer (loop->audio, 0, loop->audio.getNumSamples());
        }
        ed->selectPad (padOfNote (36));
        sp->loadFile (wavFile);
        check (sp->bpm == 96.0, "sampler: tempo from the file name");
        sp->setChopMode (3); sp->gridBeats = 1.0;
        sp->doChop();
        check (sp->getWave().slices().size() == 8, "sampler: beat-grid chop makes one slice per beat");
        sp->sliceSet[2].gainDb = -6.0f; sp->sliceSet[3].reverse = true;
        q.currentPattern = 2; q.patterns[2].stepsPer2Beats = 4;     // 1/8 steps
        sp->chopToPattern();
        auto& pat = q.patterns[2];
        bool inOrder = pat.length.load() == 16;
        for (int i = 0; i < 8; ++i) inOrder = inOrder && pat.vel[padOfNote (36 + i)][i * 2].load() == 100 && pat.vel[padOfNote (36 + i)][i * 2 + 1].load() == 0;
        check (inOrder, "chop to pattern: slice n plays on beat n (1/8 steps), on pads 36-43");
        check (std::abs (q.getPad (padOfNote (38))->gain - 0.501f) < 0.01f && q.getPad (padOfNote (39))->reverse,
               "slices take their gain and reverse to the pads");
        sp->sliceSet[2].gainDb = 0.0f;
        sp->sendToKeys (1);
        check (q.keysTracks[1].sliceCount.load() == 8 && q.keysTracks[1].sourcePad.load() == padOfNote (36), "send to Keys: a track plays the 8 slices");
        const size_t before = sp->getWave().markers.size();
        sp->setChopMode (2); sp->doChop();
        sp->undo();
        check (sp->getWave().markers.size() == before && sp->sliceSet.size() == 8 && sp->sliceSet[3].reverse, "sampler undo: brings back the slices and their settings");
        sp->redo();
        check (sp->getWave().markers.size() != before, "sampler redo");
        sp->undo();
        if (auto* snap3 = std::getenv ("FLIPKIT_SNAP3"))
        {
            sp->setChopMode (3);
            ed->showTab (1);
            sp->getWave().selectedSlice = 2;
            sp->getWave().onSliceClicked (2);
            MessageManager::getInstance()->runDispatchLoopUntil (300);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds());
            const File f { String (snap3) + "-sampler-loop.png" }; f.deleteFile(); FileOutputStream o { f };
            PNGImageFormat().writeImageToStream (img, o);
        }
        wavFile.deleteFile();
    }
    if (auto* snap3 = std::getenv ("FLIPKIT_SNAP3"))
    {   // ---------------- 0.9.8 screenshots: envelope editor, sampler
        PadRomplerProcessor q; q.prepareToPlay (SR, BS);
        std::unique_ptr<AudioProcessorEditor> base (q.createEditor());
        auto* ed = dynamic_cast<PadRomplerEditor*> (base.get());
        ed->setSize (1280, 780);
        const int pad = padOfNote (44);
        q.assignRegion (padOfNote (36), makeTone (0.2, 60.0f, 10.0f), 0, 1, "Kick");
        q.assignRegion (padOfNote (37), makeTone (0.2, 200.0f, 10.0f), 0, 1, "Snare");
        q.assignRegion (pad, makeTone (2.0, 262.0f, 1.2f), 0, 1, "Rhodes C3");
        q.updatePad (pad, [] (PadState& s) { s.play = PlayMode::Hold; s.attackMs = 40; s.decayMs = 420; s.sustain = 0.55f; s.releaseMs = 700; s.curveD = 0.6f; s.curveR = 0.6f; });
        ed->showTab (0);
        ed->selectPad (pad);
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        auto shot = [&] (const String& name)
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds());
            const File f { String (snap3) + "-" + name + ".png" }; f.deleteFile(); FileOutputStream o { f };
            PNGImageFormat().writeImageToStream (img, o);
        };
        shot ("pads");
        for (auto* c : ed->getChildren())
            if (auto* pp = dynamic_cast<PadsPage*> (c))
            {
                pp->envelope.hearIt();
                MessageManager::getInstance()->runDispatchLoopUntil (700);
                shot ("hear");
            }
        ed->showTab (1);
        MessageManager::getInstance()->runDispatchLoopUntil (200);
        shot ("sampler");
    }
    std::printf ("%d failures\n", fails);
    return fails;
}
