#include "PluginProcessor.h"
#include "Network.h"
#include "PluginEditor.h"
#include <cstdlib>
#include <cmath>

using namespace juce;

PadRomplerProcessor::PadRomplerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Sampling input", AudioChannelSet::stereo(), false)
                          .withOutput ("Output", AudioChannelSet::stereo(), true))
{
    formats.registerBasicFormats();

    std::fill (std::begin (noteToPad), std::end (noteToPad), -1);
    for (int r = 0; r < PadLayout::rows; ++r)
        for (int c = 0; c < PadLayout::cols; ++c)
            noteToPad[PadLayout::noteFor (r, c)] = PadLayout::padIndex (r, c);

    for (auto& p : pads)
        p = std::make_shared<const PadState>();

    browserRoot = File::getSpecialLocation (File::userMusicDirectory);
    startTimer (500);

   #if FLIPKIT_DEV
    // Developer hook: load a kit at startup (used for automated UI checks only).
    if (auto* testKit = std::getenv ("PADROMPLER_TESTKIT"))
        if (MessageManager::getInstanceWithoutCreating() != nullptr
             && MessageManager::getInstance()->isThisTheMessageThread())
            loadKit (File (String (testKit)));
   #endif
}

PadRomplerProcessor::~PadRomplerProcessor() { stopTimer(); }

bool PadRomplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != AudioChannelSet::stereo()) return false;
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == AudioChannelSet::stereo();
}

bool PadRomplerProcessor::inputBusAvailable() const
{
    auto* bus = getBus (true, 0);
    return bus != nullptr && bus->isEnabled() && getTotalNumInputChannels() >= 2;
}

void PadRomplerProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    hostRate = sampleRate > 0 ? sampleRate : 44100.0;
    inputCopy.setSize (2, jmax (blockSize, 8192));
    for (auto& v : voices) { v.active = false; v.sample.reset(); }
}

File PadRomplerProcessor::recordingsFolder()
{
    return File::getSpecialLocation (File::userDocumentsDirectory).getChildFile ("FlipKit64").getChildFile ("Recordings");
}

// ------------------------------------------------------------------------------- pad access

PadPtr PadRomplerProcessor::getPad (int index) const
{
    return std::atomic_load (&pads[index]);
}

void PadRomplerProcessor::setPad (int index, PadPtr newState)
{
    auto old = std::atomic_exchange (&pads[index], std::move (newState));
    retiredPads.push_back (std::move (old));   // freed later by the timer
    if (onPadsChanged) onPadsChanged();
}

void PadRomplerProcessor::updatePad (int index, const std::function<void (PadState&)>& change)
{
    auto next = std::make_shared<PadState> (*getPad (index));
    change (*next);
    setPad (index, next);
}

SamplePtr PadRomplerProcessor::loadSample (const File& file)
{
    for (auto& s : sampleCache)                 // reuse if already loaded
        if (s->file == file) return s;

    if (! file.existsAsFile())                  // a network sample whose cached copy is gone: fetch it again
    {
        juce::String server, share, remotePath;
        if (Net::fromCacheFile (file, server, share, remotePath))
        {
            Net::SharedLibrary lib;
            Net::Location loc;
            juce::String err;
            if (lib->findLocation (server + "/" + share, loc))
                lib->fetchNow (loc, remotePath, err, 30000);
        }
    }

    std::unique_ptr<AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr) return {};

    const auto maxLen = (int64) (reader->sampleRate * 720.0);           // 12 min cap (whole songs)
    const int len = (int) jmin (reader->lengthInSamples, maxLen);
    if (len <= 0) return {};

    auto data = std::make_shared<SampleData>();
    data->file = file;
    data->sampleRate = reader->sampleRate;
    data->audio.setSize (2, len);
    reader->read (&data->audio, 0, len, 0, true, true);
    if (reader->numChannels == 1)
        data->audio.copyFrom (1, 0, data->audio, 0, 0, len);

    SamplePtr s = data;
    sampleCache.push_back (s);
    return s;
}

static String nameFromFile (const File& f) { return f.getFileNameWithoutExtension(); }

void PadRomplerProcessor::assignFiles (int index, const Array<File>& files, bool append)
{
    auto cur = getPad (index);
    auto next = std::make_shared<PadState> (append ? *cur : PadState());
    if (! append) next->mode = cur->mode;

    for (auto& f : files)
        if (auto s = loadSample (f))
            next->layers.push_back (s);

    if (next->layers.empty() && ! append) return;   // nothing loadable: keep pad as it was
    if (! append || cur->name.isEmpty())
        next->name = next->layers.empty() ? String() : nameFromFile (next->layers.front()->file);
    if (! append && files.size() > 1)
        next->mode = LayerMode::RoundRobin;
    setPad (index, next);
}

void PadRomplerProcessor::assignRegion (int index, const SamplePtr& sample, float start, float end, const String& name)
{
    if (sample == nullptr) return;
    auto next = std::make_shared<PadState>();
    next->layers.push_back (sample);
    next->start = jlimit (0.0f, 1.0f, start);
    next->end = jlimit (next->start, 1.0f, end);
    next->name = name;
    setPad (index, next);
}

void PadRomplerProcessor::removeLayer (int index, int layer)
{
    auto next = std::make_shared<PadState> (*getPad (index));
    if (! isPositiveAndBelow (layer, (int) next->layers.size())) return;
    next->layers.erase (next->layers.begin() + layer);
    if (next->layers.empty()) next->name = {};
    setPad (index, next);
}

void PadRomplerProcessor::moveLayer (int index, int layer, int delta)
{
    auto next = std::make_shared<PadState> (*getPad (index));
    const int to = layer + delta, n = (int) next->layers.size();
    if (! isPositiveAndBelow (layer, n) || ! isPositiveAndBelow (to, n)) return;
    std::swap (next->layers[(size_t) layer], next->layers[(size_t) to]);
    setPad (index, next);
}

void PadRomplerProcessor::clearPad (int index) { setPad (index, std::make_shared<const PadState>()); }

void PadRomplerProcessor::triggerPad (int index, float velocity)
{
    int s1, n1, s2, n2;
    reqFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { reqBuf[s1] = { index, velocity }; reqFifo.finishedWrite (1); }
}

void PadRomplerProcessor::previewRegion (const SamplePtr& s, float start, float end)
{
    if (s == nullptr) return;
    auto p = std::make_shared<Preview>();
    p->sample = s; p->start = start; p->end = end;
    std::shared_ptr<const Preview> cp = p;
    retiredPreviews.push_back (cp);            // message thread keeps a reference
    std::atomic_store (&pendingPreview, cp);
    previewWaiting = true;
}

void PadRomplerProcessor::previewFile (const File& file)
{
    previewRegion (loadSample (file), 0.0f, 1.0f);
}

void PadRomplerProcessor::startTap (const SamplePtr& s, double startSample)
{
    if (s == nullptr) return;
    std::atomic_store (&tapSample, std::shared_ptr<const SampleData> (s));
    tapStartRequest = jmax (0.0, startSample);
    tapPos = startSample;
    tapReachedEnd = false;
    tapRestart = true;
    tapPlaying = true;
}

int PadRomplerProcessor::popTapEvents (TapEvent* out, int max)
{
    int s1, n1, s2, n2;
    tapFifo.prepareToRead (jmin (max, tapFifo.getNumReady()), s1, n1, s2, n2);
    int n = 0;
    for (int i = 0; i < n1; ++i) out[n++] = tapBuf[s1 + i];
    for (int i = 0; i < n2; ++i) out[n++] = tapBuf[s2 + i];
    tapFifo.finishedRead (n1 + n2);
    return n;
}

void PadRomplerProcessor::selectPattern (int p)
{
    p = jlimit (0, Seq::numPatterns - 1, p);
    if (hostPlaying.load() || internalPlaying.load()) queuedPattern = p;
    else { currentPattern = p; queuedPattern = -1; }
}

void PadRomplerProcessor::timerCallback()
{
    retiredPads.erase (std::remove_if (retiredPads.begin(), retiredPads.end(),
                                       [] (const PadPtr& p) { return p.use_count() == 1; }),
                       retiredPads.end());

    auto current = std::atomic_load (&pendingPreview);
    retiredPreviews.erase (std::remove_if (retiredPreviews.begin(), retiredPreviews.end(),
                                           [&] (const std::shared_ptr<const Preview>& p)
                                           { return p != current && p.use_count() == 1; }),
                           retiredPreviews.end());

    // drop cached samples not used by any pad, voice or preview
    sampleCache.erase (std::remove_if (sampleCache.begin(), sampleCache.end(),
                                       [] (const SamplePtr& s) { return s.use_count() == 1; }),
                       sampleCache.end());
}

// ------------------------------------------------------------------------------- recording

bool PadRomplerProcessor::armRecording (double maxSeconds)
{
    const int st = recState.load();
    if (st == recArmed || st == recRecording) return false;
    recRate = hostRate;
    recBuffer.setSize (2, (int) (jlimit (1.0, 600.0, maxSeconds) * recRate), false, true, false);
    recBuffer.clear();
    recPos = 0;
    recState = recArmed;
    return true;
}

void PadRomplerProcessor::stopRecording()
{
    int expected = recArmed;
    if (recState.compare_exchange_strong (expected, recIdle)) return;   // nothing captured yet
    expected = recRecording;
    recState.compare_exchange_strong (expected, recFinished);
}

File PadRomplerProcessor::saveRecording()
{
    if (recState.load() != recFinished) return {};
    const int len = recPos.load();
    recState = recIdle;
    if (len <= 0) return {};

    auto folder = recordingsFolder();
    folder.createDirectory();
    auto file = folder.getNonexistentChildFile ("Rec " + Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), ".wav", false);

    WavAudioFormat wav;
    std::unique_ptr<OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) return {};
    std::unique_ptr<AudioFormatWriter> writer (wav.createWriterFor (stream.get(), recRate, 2, 24, {}, 0));
    if (writer == nullptr) return {};
    stream.release();                                   // writer owns it now
    writer->writeFromAudioSampleBuffer (recBuffer, 0, len);
    writer.reset();
    return file;
}

// ------------------------------------------------------------------------------- audio

static const PadState& defaultPad() { static const PadState p; return p; }

void PadRomplerProcessor::startVoice (const SamplePtr& s, const PadState* padState, float gain, int padIndex,
                                      float start, float end)
{
    if (s == nullptr) return;
    const PadState& ps = padState != nullptr ? *padState : defaultPad();

    // choke groups / mono: fade out the voices this hit should cut
    for (auto& v : voices)
    {
        if (! v.active || v.releasing) continue;
        if ((ps.mono && v.pad == padIndex && padIndex >= 0)
             || (ps.choke > 0 && v.choke == ps.choke && v.pad != padIndex))
            v.release (hostRate);
    }

    const int len = s->audio.getNumSamples();
    const double rs = jlimit (0.0, 1.0, (double) start) * len;
    const double re = jlimit (0.0, 1.0, (double) end) * len;
    if (re - rs < 4.0) return;

    Voice* v = nullptr;
    for (auto& cand : voices) if (! cand.active) { v = &cand; break; }
    if (v == nullptr)                      // steal: prefer a releasing voice, else the oldest-sounding
    {
        float best = 1.0e9f;
        for (auto& cand : voices)
        {
            const float score = cand.releasing ? -1.0f : cand.env;
            if (score < best) { best = score; v = &cand; }
        }
    }

    v->sample = s;           // copy (refcount++) — never the last owner, cache holds one too
    v->regionStart = rs;
    v->regionEnd = re;
    v->reverse = ps.reverse;
    v->pos = ps.reverse ? re - 1.0 : rs;
    v->step = s->sampleRate / hostRate * std::pow (2.0, (ps.tune + ps.fine / 100.0) / 12.0);
    v->gainL = gain * jmin (1.0f, 1.0f - ps.pan);
    v->gainR = gain * jmin (1.0f, 1.0f + ps.pan);
    v->pad = padIndex;
    v->choke = ps.choke;

    v->releasing = false;
    if (ps.attackMs > 0.5f) { v->env = 0.0f; v->inAttack = true; v->attackInc = (float) (1.0 / (ps.attackMs * 0.001 * hostRate)); }
    else                    { v->env = 1.0f; v->inAttack = false; }
    v->decaying = ! ps.decayOff();
    if (v->decaying) v->decayCoef = (float) std::exp (std::log (0.001) / (jmax (10.0f, ps.decayMs) * 0.001 * hostRate));

    v->filterOn = ! ps.filterOff();
    if (v->filterOn)
    {
        const double fc = jlimit (20.0, hostRate * 0.45, (double) ps.cutoff);
        const double g = std::tan (MathConstants<double>::pi * fc / hostRate);
        const double k = 2.0 - 1.94 * jlimit (0.0f, 1.0f, ps.reso);
        v->fg = (float) g; v->fk = (float) k;
        v->fa1 = (float) (1.0 / (1.0 + g * (g + k)));
        v->fa2 = (float) g * v->fa1;
        v->fa3 = (float) g * v->fa2;
        v->ic1[0] = v->ic1[1] = v->ic2[0] = v->ic2[1] = 0.0f;
    }
    v->active = true;
}

void PadRomplerProcessor::handleHit (int padIndex, float velocity)
{
    auto pad = std::atomic_load (&pads[padIndex]);
    if (pad == nullptr || pad->layers.empty()) return;
    const int n = (int) pad->layers.size();
    int which = 0;
    switch (pad->mode)
    {
        case LayerMode::RoundRobin: which = rrIndex[padIndex]++ % n; break;
        case LayerMode::Velocity:   which = jlimit (0, n - 1, (int) (velocity * (float) n * 0.9999f)); break;
        case LayerMode::Random:     which = rng.nextInt (n); break;
    }
    const float vcurve = velocity * velocity * 0.7f + velocity * 0.3f;   // gentle curve
    startVoice (pad->layers[(size_t) which], pad.get(), pad->gain * vcurve, padIndex, pad->start, pad->end);
    hitCounter[padIndex].fetch_add (1, std::memory_order_relaxed);
}

void PadRomplerProcessor::renderVoices (AudioBuffer<float>& buffer, int start, int count)
{
    if (count <= 0) return;
    auto* L = buffer.getWritePointer (0, start);
    auto* R = buffer.getWritePointer (1, start);

    for (auto& v : voices)
    {
        if (! v.active) continue;
        const auto& a = v.sample->audio;
        const float* sl = a.getReadPointer (0);
        const float* sr = a.getReadPointer (1);
        const double fadeLen = 64.0;     // source samples: short fade at the region edge (no clicks on chops)

        for (int i = 0; i < count; ++i)
        {
            const double remaining = v.reverse ? v.pos - v.regionStart : v.regionEnd - 1.0 - v.pos;
            if (remaining <= 0.0) { v.active = false; break; }

            const int idx = (int) v.pos;
            const float frac = (float) (v.pos - idx);
            const int nxt = jmin (idx + 1, a.getNumSamples() - 1);
            float xl = sl[idx] + frac * (sl[nxt] - sl[idx]);
            float xr = sr[idx] + frac * (sr[nxt] - sr[idx]);

            if (v.filterOn)
            {
                float* in[2] = { &xl, &xr };
                for (int c = 0; c < 2; ++c)
                {
                    const float v3 = *in[c] - v.ic2[c];
                    const float v1 = v.fa1 * v.ic1[c] + v.fa2 * v3;
                    const float v2 = v.ic2[c] + v.fa2 * v.ic1[c] + v.fa3 * v3;
                    v.ic1[c] = 2.0f * v1 - v.ic1[c];
                    v.ic2[c] = 2.0f * v2 - v.ic2[c];
                    *in[c] = v2;
                }
            }

            if (v.releasing)        { v.env *= v.releaseCoef; if (v.env < 1.0e-4f) { v.active = false; break; } }
            else if (v.inAttack)    { v.env += v.attackInc; if (v.env >= 1.0f) { v.env = 1.0f; v.inAttack = false; } }
            else if (v.decaying)    { v.env *= v.decayCoef; if (v.env < 1.0e-4f) { v.active = false; break; } }

            const float fade = remaining < fadeLen ? (float) (remaining / fadeLen) : 1.0f;
            const float g = v.env * fade;
            L[i] += xl * v.gainL * g;
            R[i] += xr * v.gainR * g;
            v.pos += v.reverse ? -v.step : v.step;
        }
    }
}

void PadRomplerProcessor::scheduleSequencer (double ppq0, double ppqPerSample, int numSamples,
                                             Hit* hits, int& numHits, int maxHits)
{
    int patIndex = jlimit (0, Seq::numPatterns - 1, currentPattern.load());
    const PatternData* pat = &patterns[patIndex];
    int len = jlimit (1, Seq::maxSteps, pat->length.load());
    int spb = jmax (1, pat->stepsPerBeat.load());
    double stepLen = 1.0 / spb;
    const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);
    const double ppq1 = ppq0 + ppqPerSample * numSamples;

    const auto kFirst = (long long) std::floor (ppq0 / stepLen) - 1;
    const auto kLast  = (long long) std::floor (ppq1 / stepLen);
    for (long long k = jmax (0LL, kFirst); k <= kLast; ++k)
    {
        const double base = k * stepLen + ((k & 1) ? swingOff * stepLen : 0.0);
        int stepIdx = (int) (k % len);

        if (stepIdx == 0 && base >= ppq0 && base < ppq1)
        {
            const int q = queuedPattern.exchange (-1);
            if (q >= 0 && q != patIndex)
            {
                patIndex = q; currentPattern = q;
                pat = &patterns[patIndex];
                len = jlimit (1, Seq::maxSteps, pat->length.load());
                spb = jmax (1, pat->stepsPerBeat.load());
                if (spb != (int) std::lround (1.0 / stepLen)) return;   // resolution changed: resume next block
                std::memset (skipOnce, 0, sizeof (skipOnce));
            }
        }
        if (base >= ppq0 && base < ppq1) playStep = stepIdx;

        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            const int vel = pat->vel[pad][stepIdx].load (std::memory_order_relaxed);
            if (vel == 0) continue;
            const int rat = jlimit (1, 4, (int) pat->ratchet[pad][stepIdx].load (std::memory_order_relaxed));
            for (int j = 0; j < rat; ++j)
            {
                const double t = base + j * stepLen / rat;
                if (t < ppq0 || t >= ppq1) continue;
                if (j == 0 && skipOnce[pad][stepIdx]) { skipOnce[pad][stepIdx] = 0; continue; }
                if (numHits >= maxHits) return;
                const int pos = jlimit (0, numSamples - 1, (int) ((t - ppq0) / ppqPerSample));
                const float v = (float) vel / 127.0f * (j == 0 ? 1.0f : 0.85f);   // ratchet repeats slightly softer
                hits[numHits++] = { pos, pad, v };
            }
        }
    }
}

void PadRomplerProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // keep a copy of the sampling input before the buffer becomes our output
    const bool haveInput = getTotalNumInputChannels() >= 2 && numSamples <= inputCopy.getNumSamples();
    if (haveInput)
        for (int c = 0; c < 2; ++c) inputCopy.copyFrom (c, 0, buffer, c, 0, numSamples);
    buffer.clear();

    // ---- transport: follow LUNA when it plays, otherwise our own transport
    double ppq0 = 0.0, ppqPerSample = 0.0;
    bool running = false;
    bool hostIsPlaying = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) hostBpm = *b;
            hostIsPlaying = pos->getIsPlaying();
            if (hostIsPlaying)
                if (auto p = pos->getPpqPosition())
                {
                    ppq0 = *p;
                    ppqPerSample = hostBpm.load() / 60.0 / hostRate;
                    running = true;
                }
        }
    hostPlaying = hostIsPlaying;
    const bool intPlay = internalPlaying.load();
    if (! running && intPlay)
    {
        if (! wasInternalPlaying) internalPpq = 0.0;
        ppq0 = internalPpq;
        ppqPerSample = internalBpm.load() / 60.0 / hostRate;
        internalPpq += ppqPerSample * numSamples;
        running = true;
    }
    wasInternalPlaying = intPlay && ! hostIsPlaying;
    if (! running)
    {
        if (playStep.load() != -1) std::memset (skipOnce, 0, sizeof (skipOnce));
        playStep = -1;
        const int q = queuedPattern.exchange (-1);
        if (q >= 0) currentPattern = q;
    }

    // ---- gather hits for this block
    constexpr int maxHits = 1024;
    Hit hits[maxHits];
    int numHits = 0;

    {   // auditions from the editor
        int s1, n1, s2, n2;
        reqFifo.prepareToRead (reqFifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1 && numHits < maxHits; ++i) hits[numHits++] = { 0, reqBuf[s1 + i].pad, reqBuf[s1 + i].velocity };
        for (int i = 0; i < n2 && numHits < maxHits; ++i) hits[numHits++] = { 0, reqBuf[s2 + i].pad, reqBuf[s2 + i].velocity };
        reqFifo.finishedRead (n1 + n2);
    }
    if (previewWaiting.exchange (false))
        if (auto pv = std::atomic_load (&pendingPreview))
            startVoice (pv->sample, nullptr, 0.8f, -1, pv->start, pv->end);

    // ---- tap chop playback state for this block
    std::shared_ptr<const SampleData> tapS;
    double tapStep = 1.0, tapBlockStart = 0.0;
    const bool tapRunning = tapPlaying.load();
    if (tapRunning)
    {
        tapS = std::atomic_load (&tapSample);
        if (tapRestart.exchange (false)) tapPlayPos = tapStartRequest.load();
        if (tapS != nullptr) tapStep = tapS->sampleRate / hostRate;
        tapBlockStart = tapPlayPos;
    }
    const bool tapActive = tapRunning && tapS != nullptr && tapMode.load() != tapOff;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        const int pos = jlimit (0, numSamples - 1, meta.samplePosition);
        if (msg.isNoteOn())
        {
            const int pad = noteToPad[msg.getNoteNumber()];
            if (pad < 0) continue;
            if (tapActive)                        // tap chop: the hit marks a chop point instead of playing
            {
                int s1, n1, s2, n2;
                tapFifo.prepareToWrite (1, s1, n1, s2, n2);
                if (n1 > 0) { tapBuf[s1] = { pad, tapBlockStart + pos * tapStep }; tapFifo.finishedWrite (1); }
                hitCounter[pad].fetch_add (1, std::memory_order_relaxed);   // still flash the pad
                continue;
            }
            const float vel = msg.getFloatVelocity();
            if (numHits < maxHits) hits[numHits++] = { pos, pad, vel };

            if (running && seqRecord.load())      // live record, quantized to the nearest step
            {
                auto& pat = patterns[jlimit (0, Seq::numPatterns - 1, currentPattern.load())];
                const int len = jlimit (1, Seq::maxSteps, pat.length.load());
                const double stepLen = 1.0 / jmax (1, pat.stepsPerBeat.load());
                const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);
                const double t = ppq0 + pos * ppqPerSample;
                auto timeOf = [&] (long long k) { return k * stepLen + ((k & 1) ? swingOff * stepLen : 0.0); };
                long long best = std::llround (t / stepLen);
                for (long long k = best - 1; k <= best + 1; ++k)
                    if (k >= 0 && std::abs (timeOf (k) - t) < std::abs (timeOf (best) - t)) best = k;
                best = jmax (0LL, best);
                const int idx = (int) (best % len);
                pat.vel[pad][idx].store ((uint8_t) jlimit (1, 127, (int) std::lround (vel * 127.0f)));
                pat.ratchet[pad][idx].store (1);
                if (timeOf (best) > t) skipOnce[pad][idx] = 1;   // already heard it; don't double-trigger
            }
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            for (auto& v : voices) if (v.active) v.release (hostRate);
        }
    }

    if (running)
        scheduleSequencer (ppq0, ppqPerSample, numSamples, hits, numHits, maxHits);

    // sort by position (small lists: insertion sort)
    for (int i = 1; i < numHits; ++i)
        for (int j = i; j > 0 && hits[j].pos < hits[j - 1].pos; --j)
            std::swap (hits[j], hits[j - 1]);

    // ---- render, starting hits sample-accurately
    int rendered = 0;
    for (int i = 0; i < numHits; ++i)
    {
        renderVoices (buffer, rendered, hits[i].pos - rendered);
        rendered = jmax (rendered, hits[i].pos);
        handleHit (hits[i].pad, hits[i].velocity);
    }
    renderVoices (buffer, rendered, numSamples - rendered);

    // tap chop: play the sample being chopped
    if (tapRunning && tapS != nullptr)
    {
        const auto& a = tapS->audio;
        const int len = a.getNumSamples();
        const float* sl = a.getReadPointer (0);
        const float* sr = a.getReadPointer (1);
        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            const int idx = (int) tapPlayPos;
            if (idx >= len - 1) { tapPlaying = false; tapReachedEnd = true; break; }
            const float frac = (float) (tapPlayPos - idx);
            L[i] += 0.8f * (sl[idx] + frac * (sl[idx + 1] - sl[idx]));
            R[i] += 0.8f * (sr[idx] + frac * (sr[idx + 1] - sr[idx]));
            tapPlayPos += tapStep;
        }
        tapPos = tapPlayPos;
    }

    // Clearing a finished voice's shared_ptr only decrements (the cache always holds another
    // reference), so this never frees memory on the audio thread.
    for (auto& v : voices) if (! v.active && v.sample != nullptr) v.sample.reset();

    buffer.applyGain (masterGain.load());

    // ---- recording / resampling ("what you hear", or the sampling input)
    {
        const float* srcL = nullptr; const float* srcR = nullptr;
        if (recSource.load() == 1) { if (haveInput) { srcL = inputCopy.getReadPointer (0); srcR = inputCopy.getReadPointer (1); } }
        else { srcL = buffer.getReadPointer (0); srcR = buffer.getReadPointer (1); }

        float peak = 0.0f;
        if (srcL != nullptr)
            for (int i = 0; i < numSamples; ++i) peak = jmax (peak, std::abs (srcL[i]), std::abs (srcR[i]));
        recInputLevel = jmax (peak, recInputLevel.load() * 0.8f);

        int st = recState.load();
        if (srcL != nullptr && (st == recArmed || st == recRecording))
        {
            int startAt = 0;
            if (st == recArmed)
            {
                const float thr = recThreshold.load();
                startAt = -1;
                if (thr <= 0.0f) startAt = 0;
                else for (int i = 0; i < numSamples; ++i)
                    if (std::abs (srcL[i]) > thr || std::abs (srcR[i]) > thr) { startAt = i; break; }
                if (startAt >= 0)
                {
                    int expected = recArmed;
                    if (recState.compare_exchange_strong (expected, recRecording)) st = recRecording;
                }
            }
            if (st == recRecording && startAt >= 0)
            {
                const int cap = recBuffer.getNumSamples();
                const int pos = recPos.load();
                const int n = jmin (numSamples - startAt, cap - pos);
                if (n > 0)
                {
                    recBuffer.copyFrom (0, pos, srcL + startAt, n);
                    recBuffer.copyFrom (1, pos, srcR + startAt, n);
                    recPos = pos + n;
                }
                if (pos + n >= cap)
                {
                    int expected = recRecording;
                    recState.compare_exchange_strong (expected, recFinished);
                }
            }
        }
    }

    midi.clear();
}

// ------------------------------------------------------------------------------- state / kits

static String toHex (const std::atomic<uint8_t>* data, int n)
{
    String s;
    for (int i = 0; i < n; ++i) s << String::toHexString ((int) data[i].load()).paddedLeft ('0', 2);
    return s;
}

static void fromHex (const String& s, std::atomic<uint8_t>* data, int n, uint8_t fallback)
{
    for (int i = 0; i < n; ++i)
        data[i].store (i * 2 + 1 < s.length() ? (uint8_t) s.substring (i * 2, i * 2 + 2).getHexValue32() : fallback);
}

std::unique_ptr<XmlElement> PadRomplerProcessor::kitToXml() const
{
    auto xml = std::make_unique<XmlElement> ("FlipKit64Kit");
    xml->setAttribute ("version", 2);
    xml->setAttribute ("browserRoot", browserRoot.getFullPathName());
    xml->setAttribute ("master", (double) masterGain.load());
    for (int i = 0; i < PadLayout::numPads; ++i)
    {
        auto p = getPad (i);
        if (p->layers.empty() && p->name.isEmpty()) continue;
        auto* pe = xml->createNewChildElement ("Pad");
        pe->setAttribute ("index", i);
        pe->setAttribute ("name", p->name);
        pe->setAttribute ("gain", (double) p->gain);
        pe->setAttribute ("mode", (int) p->mode);
        pe->setAttribute ("pan", (double) p->pan);
        pe->setAttribute ("tune", (double) p->tune);
        pe->setAttribute ("fine", (double) p->fine);
        pe->setAttribute ("start", (double) p->start);
        pe->setAttribute ("end", (double) p->end);
        pe->setAttribute ("attack", (double) p->attackMs);
        pe->setAttribute ("decay", (double) p->decayMs);
        pe->setAttribute ("cutoff", (double) p->cutoff);
        pe->setAttribute ("reso", (double) p->reso);
        pe->setAttribute ("reverse", p->reverse);
        pe->setAttribute ("mono", p->mono);
        pe->setAttribute ("choke", p->choke);
        for (auto& s : p->layers)
            pe->createNewChildElement ("Sample")->setAttribute ("file", s->file.getFullPathName());
    }

    auto* seq = xml->createNewChildElement ("Sequencer");
    seq->setAttribute ("pattern", currentPattern.load());
    seq->setAttribute ("swing", (double) swing.load());
    seq->setAttribute ("bpm", internalBpm.load());
    for (int pi = 0; pi < Seq::numPatterns; ++pi)
    {
        auto& pat = patterns[pi];
        auto* pe = seq->createNewChildElement ("Pattern");
        pe->setAttribute ("index", pi);
        pe->setAttribute ("length", pat.length.load());
        pe->setAttribute ("stepsPerBeat", pat.stepsPerBeat.load());
        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            if (! pat.padHasSteps (pad)) continue;
            auto* re = pe->createNewChildElement ("Row");
            re->setAttribute ("pad", pad);
            re->setAttribute ("vel", toHex (pat.vel[pad], Seq::maxSteps));
            re->setAttribute ("rat", toHex (pat.ratchet[pad], Seq::maxSteps));
        }
    }
    return xml;
}

void PadRomplerProcessor::kitFromXml (const XmlElement& xml)
{
    if (! xml.hasTagName ("FlipKit64Kit") && ! xml.hasTagName ("PadRomplerKit")) return;   // also reads Pad Rompler kits
    if (xml.hasAttribute ("browserRoot")) browserRoot = File (xml.getStringAttribute ("browserRoot"));
    masterGain = (float) xml.getDoubleAttribute ("master", 0.8);

    for (int i = 0; i < PadLayout::numPads; ++i)
        setPad (i, std::make_shared<const PadState>());

    for (auto* pe : xml.getChildWithTagNameIterator ("Pad"))
    {
        const int i = pe->getIntAttribute ("index", -1);
        if (! isPositiveAndBelow (i, PadLayout::numPads)) continue;
        auto st = std::make_shared<PadState>();
        st->name = pe->getStringAttribute ("name");
        st->gain = (float) pe->getDoubleAttribute ("gain", 1.0);
        st->mode = (LayerMode) jlimit (0, 2, pe->getIntAttribute ("mode", 0));
        st->pan = (float) pe->getDoubleAttribute ("pan", 0.0);
        st->tune = (float) pe->getDoubleAttribute ("tune", 0.0);
        st->fine = (float) pe->getDoubleAttribute ("fine", 0.0);
        st->start = (float) pe->getDoubleAttribute ("start", 0.0);
        st->end = (float) pe->getDoubleAttribute ("end", 1.0);
        st->attackMs = (float) pe->getDoubleAttribute ("attack", 0.0);
        st->decayMs = (float) pe->getDoubleAttribute ("decay", 5000.0);
        st->cutoff = (float) pe->getDoubleAttribute ("cutoff", 20000.0);
        st->reso = (float) pe->getDoubleAttribute ("reso", 0.0);
        st->reverse = pe->getBoolAttribute ("reverse", false);
        st->mono = pe->getBoolAttribute ("mono", false);
        st->choke = jlimit (0, 8, pe->getIntAttribute ("choke", 0));
        for (auto* se : pe->getChildWithTagNameIterator ("Sample"))
            if (auto s = loadSample (File (se->getStringAttribute ("file"))))
                st->layers.push_back (s);
        setPad (i, st);
    }

    for (auto& pat : patterns) { pat.clear(); pat.length = 16; pat.stepsPerBeat = 4; }
    if (auto* seq = xml.getChildByName ("Sequencer"))
    {
        swing = (float) seq->getDoubleAttribute ("swing", 50.0);
        internalBpm = seq->getDoubleAttribute ("bpm", 120.0);
        for (auto* pe : seq->getChildWithTagNameIterator ("Pattern"))
        {
            const int pi = pe->getIntAttribute ("index", -1);
            if (! isPositiveAndBelow (pi, Seq::numPatterns)) continue;
            auto& pat = patterns[pi];
            pat.length = jlimit (1, Seq::maxSteps, pe->getIntAttribute ("length", 16));
            pat.stepsPerBeat = jlimit (1, 8, pe->getIntAttribute ("stepsPerBeat", 4));
            for (auto* re : pe->getChildWithTagNameIterator ("Row"))
            {
                const int pad = re->getIntAttribute ("pad", -1);
                if (! isPositiveAndBelow (pad, PadLayout::numPads)) continue;
                fromHex (re->getStringAttribute ("vel"), pat.vel[pad], Seq::maxSteps, 0);
                fromHex (re->getStringAttribute ("rat"), pat.ratchet[pad], Seq::maxSteps, 1);
            }
        }
        currentPattern = jlimit (0, Seq::numPatterns - 1, seq->getIntAttribute ("pattern", 0));
    }
    if (onPadsChanged) onPadsChanged();
}

void PadRomplerProcessor::getStateInformation (MemoryBlock& dest)
{
    copyXmlToBinary (*kitToXml(), dest);
}

void PadRomplerProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        // hosts may call this off the message thread; do the loading on the message thread
        if (MessageManager::getInstance()->isThisTheMessageThread()) { kitFromXml (*xml); return; }
        auto copy = std::make_shared<XmlElement> (*xml);
        std::weak_ptr<int> alive = aliveToken;
        MessageManager::callAsync ([this, copy, alive] { if (alive.lock()) kitFromXml (*copy); });
    }
}

bool PadRomplerProcessor::saveKit (const File& file) { return kitToXml()->writeTo (file); }

bool PadRomplerProcessor::loadKit (const File& file)
{
    if (auto xml = parseXML (file)) { kitFromXml (*xml); return true; }
    return false;
}

AudioProcessorEditor* PadRomplerProcessor::createEditor() { return new PadRomplerEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PadRomplerProcessor(); }
