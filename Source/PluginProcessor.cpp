#include "PluginProcessor.h"
#include "Network.h"
#include "PluginEditor.h"
#include <cstdlib>
#include <cmath>

using namespace juce;

// Nearest step to time t (in beats), allowing for swing on the off-beat steps.
static long long nearestStep (double t, double stepLen, double swingOff)
{
    auto timeOf = [&] (long long k) { return k * stepLen + ((k & 1) ? swingOff * stepLen : 0.0); };
    long long best = std::llround (t / stepLen);
    for (long long k = best - 1; k <= best + 1; ++k)
        if (k >= 0 && std::abs (timeOf (k) - t) < std::abs (timeOf (best) - t)) best = k;
    return jmax (0LL, best);
}


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
    for (auto& k : keysStep) k = -1;
    resetKeysTracks();

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

void PadRomplerProcessor::clearAllPads()
{
    stopAllSounds();
    for (int i = 0; i < PadLayout::numPads; ++i) setPad (i, std::make_shared<const PadState>());
    if (onPadsChanged) onPadsChanged();
}

void PadRomplerProcessor::newKit()
{
    clearAllPads();
    for (auto& pat : patterns) { pat.clear(); pat.length = 16; pat.stepsPer2Beats = 8; }
    resetKeysTracks();
    scaleRoot = 0; scaleType = 0;
    queuedPattern = -1;
    currentPattern = 0;
    kitLoads.fetch_add (1);
    if (onPadsChanged) onPadsChanged();
}

void PadRomplerProcessor::triggerPad (int index, float velocity, bool held)
{
    int s1, n1, s2, n2;
    reqFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { reqBuf[s1] = { index, velocity, -1, 0, held }; reqFifo.finishedWrite (1); }
}

void PadRomplerProcessor::releasePad (int index)
{
    int s1, n1, s2, n2;
    reqFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { reqBuf[s1] = { index, -1.0f }; reqFifo.finishedWrite (1); }
}

void PadRomplerProcessor::auditionRelease (int index)
{
    int s1, n1, s2, n2;
    reqFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { reqBuf[s1] = { index, -2.0f }; reqFifo.finishedWrite (1); }
}

void PadRomplerProcessor::resetKeysTracks()
{
    for (int t = 0; t < Keys::numTracks; ++t)
    {
        auto& kt = keysTracks[t];
        kt.name = "Keys " + String (t + 1);
        kt.sourcePad = -1; kt.root = 60; kt.mono = false; kt.glideMs = 0.0f; kt.level = 1.0f; kt.sliceCount = 0;
    }
    keysTracks[1].name = "Bass";                 // a mono, gliding bass track out of the box
    keysTracks[1].mono = true;
    keysTracks[1].glideMs = 40.0f;
}

void PadRomplerProcessor::triggerKey (int track, int pitch, float velocity)
{
    int s1, n1, s2, n2;
    reqFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { reqBuf[s1] = { -1, velocity, track, pitch }; reqFifo.finishedWrite (1); }
}

Keys::NotesPtr PadRomplerProcessor::makeNotes (Keys::NoteList list)
{
    if (list.empty()) return nullptr;
    std::sort (list.begin(), list.end(), [] (const Keys::Note& a, const Keys::Note& b)
               { return a.step != b.step ? a.step < b.step : a.pitch < b.pitch; });
    auto p = std::make_shared<const Keys::NoteList> (std::move (list));
    noteRegistry.push_back (p);            // freed here (message thread) once nothing else uses it
    return p;
}

void PadRomplerProcessor::drainKeyRecordings()
{
    int s1, n1, s2, n2;
    keyRecFifo.prepareToRead (keyRecFifo.getNumReady(), s1, n1, s2, n2);
    if (n1 + n2 == 0) return;
    auto add = [this] (const KeyRec& r)
    {
        auto& lane = patterns[jlimit (0, Seq::numPatterns - 1, r.pattern)].lanes[jlimit (0, Keys::numTracks - 1, r.track)];
        const int len = jlimit (1, Seq::maxSteps, lane.length.load());
        const double sl = lane.stepLength();
        const int step = (int) (jmax (0LL, std::llround (r.on / sl)) % len);
        const int nlen = jlimit (1, Seq::maxSteps, (int) std::lround ((r.off - r.on) / sl));
        Keys::NoteList list;
        if (auto cur = lane.get()) list = *cur;
        list.erase (std::remove_if (list.begin(), list.end(), [&] (const Keys::Note& n) { return n.step == step && n.pitch == r.pitch; }), list.end());
        list.push_back ({ (uint8_t) r.pitch, (uint8_t) step, (uint8_t) nlen, (uint8_t) jlimit (1, 127, (int) std::lround (r.vel * 127.0f)) });
        lane.set (makeNotes (std::move (list)));
    };
    for (int i = 0; i < n1; ++i) add (keyRecBuf[s1 + i]);
    for (int i = 0; i < n2; ++i) add (keyRecBuf[s2 + i]);
    keyRecFifo.finishedRead (n1 + n2);
    keysRecorded.fetch_add (1);
}

void PadRomplerProcessor::previewRegion (const SamplePtr& s, float start, float end, const PadState* shape)
{
    if (s == nullptr) return;
    auto p = std::make_shared<Preview>();
    p->sample = s; p->start = start; p->end = end;
    if (shape != nullptr) { p->shaped = true; p->shape = *shape; p->shape.layers.clear(); }
    std::shared_ptr<const Preview> cp = p;
    retiredPreviews.push_back (cp);            // message thread keeps a reference
    std::atomic_store (&pendingPreview, cp);
    previewWaiting = true;
}

void PadRomplerProcessor::previewFile (const File& file)
{
    playerLoad (loadSample (file), true);
}

void PadRomplerProcessor::playerLoad (const SamplePtr& s, bool autoPlay)
{
    if (s == nullptr) return;
    if (playerHold != nullptr && playerHold != s) playerRetired.push_back (playerHold);
    playerHold = s;
    playerStopReq = false;
    playerPosOut = 0.0;
    playerSeekReq = 0.0;
    std::atomic_store (&playerSample, s);
    playerPlaying = autoPlay;
}

void PadRomplerProcessor::playerPlay()
{
    if (playerHold == nullptr) return;
    playerStopReq = false;
    playerPlaying = true;
}

void PadRomplerProcessor::playerPause() { playerPlaying = false; }

void PadRomplerProcessor::playerStop()
{
    playerPlaying = false;
    playerStopReq = true;                          // the audio thread rewinds once faded out
    playerPosOut = 0.0;
}

void PadRomplerProcessor::playerSeek (double fraction)
{
    if (playerHold == nullptr) return;
    const double frames = jlimit (0.0, 1.0, fraction) * (double) jmax (0, playerHold->audio.getNumSamples() - 1);
    playerStopReq = false;
    playerSeekReq = frames;
    playerPosOut = frames;
}

double PadRomplerProcessor::playerPositionFraction() const
{
    if (playerHold == nullptr) return 0.0;
    const int len = playerHold->audio.getNumSamples();
    return len > 1 ? jlimit (0.0, 1.0, playerPosOut.load() / (double) (len - 1)) : 0.0;
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

int PadRomplerProcessor::captureToPattern()
{
    const double now = lastPpqEnd.load();
    if (now < 0.0) return -1;
    auto& pat = patterns[jlimit (0, Seq::numPatterns - 1, currentPattern.load())];
    const int len = jlimit (1, Seq::maxSteps, pat.length.load());
    const double stepLen = pat.stepLength();
    const double loopBeats = len * stepLen;
    const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);

    const uint32_t n = capCount.load();
    const uint32_t count = jmin<uint32_t> (n, (uint32_t) capSize);
    // the last loop's worth of playing; if you stopped a while ago, the loop that ended with your last hit
    double end = now, latest = -1.0;
    for (uint32_t i = n - count; i < n; ++i) latest = jmax (latest, capLog[i % capSize].ppq.load());
    if (latest < 0.0) return 0;
    if (latest < now - loopBeats) end = latest + stepLen * 0.5;
    const double from = end - loopBeats;

    int captured = 0;
    for (uint32_t i = n - count; i < n; ++i)
    {
        const auto& e = capLog[i % capSize];
        const double t = e.ppq.load();
        if (t < from || t >= end) continue;
        const int pad = jlimit (0, PadLayout::numPads - 1, e.pad.load());
        const int idx = (int) (nearestStep (t, stepLen, swingOff) % len);
        const auto v = (uint8_t) jlimit (1, 127, (int) std::lround (e.vel.load() * 127.0f));
        if (pat.vel[pad][idx].load() == 0) pat.setStep (pad, idx, v);
        else pat.vel[pad][idx].store (jmax (v, pat.vel[pad][idx].load()));
        ++captured;
    }
    return captured;
}

void PadRomplerProcessor::timerCallback()
{
    drainKeyRecordings();
    noteRegistry.erase (std::remove_if (noteRegistry.begin(), noteRegistry.end(),
                                        [] (const Keys::NotesPtr& p) { return p.use_count() == 1; }),
                        noteRegistry.end());
    retiredPads.erase (std::remove_if (retiredPads.begin(), retiredPads.end(),
                                       [] (const PadPtr& p) { return p.use_count() == 1; }),
                       retiredPads.end());

    auto current = std::atomic_load (&pendingPreview);
    retiredPreviews.erase (std::remove_if (retiredPreviews.begin(), retiredPreviews.end(),
                                           [&] (const std::shared_ptr<const Preview>& p)
                                           { return p != current && p.use_count() == 1; }),
                           retiredPreviews.end());

    playerRetired.erase (std::remove_if (playerRetired.begin(), playerRetired.end(),
                                         [] (const SamplePtr& p) { return p.use_count() == 1; }),
                         playerRetired.end());

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

SamplePtr PadRomplerProcessor::saveEdited (const SampleData& src, const String& name)
{
    auto folder = recordingsFolder().getChildFile ("Edits");
    folder.createDirectory();
    auto file = folder.getNonexistentChildFile (File::createLegalFileName (name), ".wav", false);
    {
        WavAudioFormat wav;
        std::unique_ptr<OutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return nullptr;
        std::unique_ptr<AudioFormatWriter> writer (wav.createWriterFor (stream.get(), src.sampleRate, 2, 24, {}, 0));
        if (writer == nullptr) return nullptr;
        stream.release();
        writer->writeFromAudioSampleBuffer (src.audio, 0, src.audio.getNumSamples());
    }
    return loadSample (file);
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

PadRomplerProcessor::Voice* PadRomplerProcessor::startVoice (const SamplePtr& s, const PadState* padState, float gain, int padIndex,
                                      float start, float end, int keysTrack, double semis, int64 gate)
{
    if (s == nullptr) return nullptr;
    const PadState& ps = padState != nullptr ? *padState : defaultPad();

    // choke groups / mono: fade out the voices this hit should cut (Keys notes don't choke)
    for (auto& v : voices)
    {
        if (! v.active || v.releasing || keysTrack >= 0) continue;
        if ((ps.mono && v.pad == padIndex && padIndex >= 0)
             || (ps.choke > 0 && v.choke == ps.choke && v.pad != padIndex))
            v.release (hostRate);
    }

    const int len = s->audio.getNumSamples();
    const double rs = jlimit (0.0, 1.0, (double) start) * len;
    const double re = jlimit (0.0, 1.0, (double) end) * len;
    if (re - rs < 4.0) return nullptr;

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
    v->fadeOut = jmax (64.0, ps.fadeOutMs * 0.001 * s->sampleRate);
    v->pos = ps.reverse ? re - 1.0 : rs;
    v->baseStep = s->sampleRate / hostRate * std::pow (2.0, (ps.tune + ps.fine / 100.0) / 12.0);
    v->step = v->stepTarget = v->baseStep * std::pow (2.0, semis / 12.0);
    v->glideCoef = 0.0f;
    v->keysTrack = keysTrack;
    v->gateLeft = gate;
    v->gainL = gain * jmin (1.0f, 1.0f - ps.pan);
    v->gainR = gain * jmin (1.0f, 1.0f + ps.pan);
    v->pad = padIndex;
    v->choke = keysTrack >= 0 ? 0 : ps.choke;

    v->releasing = false;
    {
        const double perMs = 0.001 * hostRate;
        v->sustainLvl = jlimit (0.0f, 1.0f, ps.sustain);
        v->kD = envCurveK (ps.curveD);
        v->kR = envCurveK (ps.curveR);
        v->decInc = 1.0 / (jmax (1.0f, ps.decayMs) * perMs);
        v->relInc = 1.0 / (jmax (1.0f, ps.releaseMs) * perMs);
        if (ps.attackMs > 0.5f)              { v->env = 0.0f; v->beginSegment (Voice::stAttack, 0.0f, 1.0f, envCurveK (ps.curveA), 1.0 / (ps.attackMs * perMs)); }
        else if (v->sustainLvl < 0.9999f)    { v->env = 1.0f; v->beginSegment (Voice::stDecay, 1.0f, v->sustainLvl, v->kD, v->decInc); }
        else                                 { v->env = 1.0f; v->stage = Voice::stSustain; }
    }

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
    return v;
}

void PadRomplerProcessor::padRelease (int padIndex)
{
    for (auto& v : voices)
        if (v.active && ! v.releasing && v.pad == padIndex && v.keysTrack < 0 && v.gateLeft < 0)
            v.noteOff();
}

void PadRomplerProcessor::handleHit (int padIndex, float velocity, int64 gate)
{
    auto pad = std::atomic_load (&pads[padIndex]);
    if (pad == nullptr || pad->layers.empty()) return;
    if (pad->play == PlayMode::Toggle)                  // already sounding: this press stops it
    {
        bool stopped = false;
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.stage != Voice::stRelease && v.pad == padIndex && v.keysTrack < 0) { v.noteOff(); stopped = true; }
        if (stopped) { hitCounter[padIndex].fetch_add (1, std::memory_order_relaxed); return; }
    }
    // Hold pads: a given gate (sequencer steps, auditions) or held until the pad is let go; others play out
    gate = pad->play != PlayMode::Hold ? 0 : (gate == 0 ? -1 : gate);
    const int n = (int) pad->layers.size();
    int which = 0;
    switch (pad->mode)
    {
        case LayerMode::RoundRobin: which = rrIndex[padIndex]++ % n; break;
        case LayerMode::Velocity:   which = jlimit (0, n - 1, (int) (velocity * (float) n * 0.9999f)); break;
        case LayerMode::Random:     which = rng.nextInt (n); break;
    }
    const float vcurve = velocity * velocity * 0.7f + velocity * 0.3f;   // gentle curve
    startVoice (pad->layers[(size_t) which], pad.get(), pad->gain * vcurve, padIndex, pad->start, pad->end, -1, 0.0, gate);
    hitCounter[padIndex].fetch_add (1, std::memory_order_relaxed);
}

void PadRomplerProcessor::handleKeyHit (int t, int pitch, float velocity, int64 gate)
{
    if (! isPositiveAndBelow (t, Keys::numTracks)) return;
    auto& kt = keysTracks[t];
    const int src = kt.sourcePad.load();
    if (! isPositiveAndBelow (src, PadLayout::numPads)) return;
    double semis = pitch - kt.root.load();
    int padIdx = src;
    if (const int nSlices = kt.sliceCount.load(); nSlices > 0)        // slice mode: one pad per key, unpitched
    {
        const int k = pitch - kt.root.load();
        if (! isPositiveAndBelow (k, nSlices)) return;
        padIdx = PadLayout::padForNote (PadLayout::noteForPad (src) + k);
        if (padIdx < 0) return;
        semis = 0.0;
    }
    auto pad = std::atomic_load (&pads[padIdx]);
    if (pad == nullptr || pad->layers.empty()) return;
    const float vcurve = velocity * velocity * 0.7f + velocity * 0.3f;
    const float gain = pad->gain * vcurve * kt.level.load();

    if (kt.mono.load())
    {
        const float gl = kt.glideMs.load();
        for (auto& v : voices)
        {
            if (! v.active || v.releasing || v.stage == Voice::stRelease || v.keysTrack != t) continue;
            if (gl > 0.5f && padIdx == src && kt.sliceCount.load() == 0)   // slide the sounding note to the new pitch
            {
                v.stepTarget = v.baseStep * std::pow (2.0, semis / 12.0);
                v.glideCoef = (float) std::exp (-1.0 / (gl * 0.001 * hostRate));
                v.gateLeft = gate;
                v.keysPitch = pitch;
                v.gainL = gain * jmin (1.0f, 1.0f - pad->pan);
                v.gainR = gain * jmin (1.0f, 1.0f + pad->pan);
                return;
            }
            v.release (hostRate, 6.0);
        }
    }
    if (auto* v = startVoice (pad->layers.front(), pad.get(), gain, -1, pad->start, pad->end, t, semis, gate))
        v->keysPitch = pitch;
}

void PadRomplerProcessor::releaseKey (int t, int pitch)
{
    for (auto& v : voices)
        if (v.active && ! v.releasing && v.keysTrack == t && v.keysPitch == pitch && v.gateLeft < 0)
            v.noteOff();
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
        const double fadeLen = v.fadeOut;     // source samples: fade at the region edge (no clicks on chops)

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

            if (v.gateLeft > 0 && --v.gateLeft == 0) v.noteOff();          // gate ends: Keys note, sequenced Hold step
            if (v.glideCoef > 0.0f) v.step = v.stepTarget + (v.step - v.stepTarget) * v.glideCoef;
            if (v.releasing) { v.env *= v.releaseCoef; if (v.env < 1.0e-4f) { v.active = false; break; } }
            else if (v.stage != Voice::stSustain)
            {
                v.segT += v.segInc;
                if (v.segT >= 1.0)
                {
                    v.env = v.segTo;
                    if (v.stage == Voice::stAttack)
                    {
                        if (v.sustainLvl < 0.9999f) v.beginSegment (Voice::stDecay, 1.0f, v.sustainLvl, v.kD, v.decInc);
                        else v.stage = Voice::stSustain;
                    }
                    else if (v.stage == Voice::stDecay) v.stage = Voice::stSustain;
                    else { v.active = false; break; }                          // release finished
                    if (v.stage == Voice::stSustain && v.sustainLvl < 1.0e-4f) { v.active = false; break; }
                }
                else
                {
                    const float t = (float) v.segT;
                    const float sh = std::abs (v.segK) < 1.0e-3f ? t : (std::exp (v.segK * t) - 1.0f) / v.segDen;
                    v.env = v.segFrom + (v.segTo - v.segFrom) * sh;
                }
            }

            const float fade = remaining < fadeLen ? (float) (remaining / fadeLen) : 1.0f;
            const float g = v.env * fade;
            L[i] += xl * v.gainL * g;
            R[i] += xr * v.gainR * g;
            v.pos += v.reverse ? -v.step : v.step;
        }
    }
}

void PadRomplerProcessor::scheduleKeys (double ppq0, double ppqPerSample, int numSamples,
                                        Hit* hits, int& numHits, int maxHits)
{
    const auto& pat = patterns[jlimit (0, Seq::numPatterns - 1, currentPattern.load())];
    const double ppq1 = ppq0 + ppqPerSample * numSamples;
    const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);
    for (int t = 0; t < Keys::numTracks; ++t)
    {
        const auto& lane = pat.lanes[t];
        const int len = jlimit (1, Seq::maxSteps, lane.length.load());
        const double sl = lane.stepLength();
        const double loop = len * sl;
        keysStep[t] = (int) ((long long) std::floor (jmax (0.0, ppq0) / sl) % len);
        auto notes = lane.get();
        if (notes == nullptr) continue;
        for (const auto& n : *notes)
        {
            if (n.step >= len) continue;
            const double start = n.step * sl + ((n.step & 1) ? swingOff * sl : 0.0);
            for (long long m = jmax (0LL, (long long) std::ceil ((ppq0 - start) / loop)); ; ++m)
            {
                const double tt = start + m * loop;
                if (tt >= ppq1) break;
                if (tt < ppq0) continue;
                if (numHits >= maxHits) return;
                const int pos = jlimit (0, numSamples - 1, (int) ((tt - ppq0) / ppqPerSample));
                const auto gate = jmax ((int64) 1, (int64) (jmin ((double) n.len, (double) len) * sl / ppqPerSample));
                hits[numHits++] = { pos, -1, n.vel / 127.0f, t, n.pitch, gate };
            }
        }
    }
}

void PadRomplerProcessor::scheduleSequencer (double ppq0, double ppqPerSample, int numSamples,
                                             Hit* hits, int& numHits, int maxHits)
{
    int patIndex = jlimit (0, Seq::numPatterns - 1, currentPattern.load());
    const PatternData* pat = &patterns[patIndex];
    int len = jlimit (1, Seq::maxSteps, pat->length.load());
    double stepLen = pat->stepLength();
    const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);
    const double ppq1 = ppq0 + ppqPerSample * numSamples;

    // Steps from two before to one after this block: swing and late nudges push hits up to a step later,
    // early nudges pull them up to half a step earlier.
    const auto kFirst = (long long) std::floor (ppq0 / stepLen) - 2;
    const auto kLast  = (long long) std::floor (ppq1 / stepLen) + 1;
    const bool fill = fillOn.load();
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
                if (pat->stepLength() != stepLen) return;   // resolution changed: resume next block
                std::memset (skipOnce, 0, sizeof (skipOnce));
            }
        }
        if (base >= ppq0 && base < ppq1) playStep = stepIdx;

        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            const int vel = pat->vel[pad][stepIdx].load (std::memory_order_relaxed);
            if (vel == 0) continue;
            if (! Seq::condPasses (pat->cond[pad][stepIdx].load (std::memory_order_relaxed), k / len, fill)) continue;
            const int ch = pat->chance[pad][stepIdx].load (std::memory_order_relaxed);
            if (ch < 100 && stepRoll (k, pad) >= ch) continue;
            const double nudged = base + (pat->nudge[pad][stepIdx].load (std::memory_order_relaxed) - 128) / 100.0 * stepLen;
            const int rat = jlimit (1, 4, (int) pat->ratchet[pad][stepIdx].load (std::memory_order_relaxed));
            for (int j = 0; j < rat; ++j)
            {
                const double t = nudged + j * stepLen / rat;
                if (t < ppq0 || t >= ppq1) continue;
                if (j == 0 && skipOnce[pad][stepIdx]) { skipOnce[pad][stepIdx] = 0; continue; }
                if (numHits >= maxHits) return;
                const int pos = jlimit (0, numSamples - 1, (int) ((t - ppq0) / ppqPerSample));
                const float v = (float) vel / 127.0f * (j == 0 ? 1.0f : 0.85f);   // ratchet repeats slightly softer
                hits[numHits++] = { pos, pad, v, -1, 0, jmax ((int64) 1, (int64) (stepLen / rat / ppqPerSample)) };   // Hold pads: one step
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
    if (stopSoundsReq.exchange (false) || (wasRunning && ! running))   // Stop: fade out everything still sounding
    {
        for (auto& v : voices)
            if (v.active && ! v.releasing && (v.pad >= 0 || v.keysTrack >= 0)) v.release (hostRate, 8.0);
        for (auto& h : held) h.on = false;
    }
    wasRunning = running;
    lastPpqEnd = running ? ppq0 + ppqPerSample * numSamples : -1.0;
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
        auto take = [&] (const Request& r)
        {
            if (numHits >= maxHits) return;
            if (r.track >= 0) hits[numHits++] = { 0, -1, r.velocity, r.track, r.pitch, (int64) (0.35 * hostRate) };
            else              hits[numHits++] = { 0, r.pad, r.velocity, -1, 0, r.held || r.velocity < 0.0f ? 0 : (int64) (0.3 * hostRate) };
        };
        for (int i = 0; i < n1; ++i) take (reqBuf[s1 + i]);
        for (int i = 0; i < n2; ++i) take (reqBuf[s2 + i]);
        reqFifo.finishedRead (n1 + n2);
    }
    if (previewWaiting.exchange (false))
        if (auto pv = std::atomic_load (&pendingPreview))
            startVoice (pv->sample, pv->shaped ? &pv->shape : nullptr, 0.8f * (pv->shaped ? pv->shape.gain : 1.0f), -1, pv->start, pv->end);

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

    // A pad hit: plays it, records it when Rec is armed, or marks a chop point during tap chop.
    auto playPad = [&] (int pad, float vel, int pos, int64 gate = 0)
    {
            if (tapActive)                        // tap chop: the hit marks a chop point instead of playing
            {
                int s1, n1, s2, n2;
                tapFifo.prepareToWrite (1, s1, n1, s2, n2);
                if (n1 > 0) { tapBuf[s1] = { pad, tapBlockStart + pos * tapStep }; tapFifo.finishedWrite (1); }
                hitCounter[pad].fetch_add (1, std::memory_order_relaxed);   // still flash the pad
                return;
            }
            if (numHits < maxHits) hits[numHits++] = { pos, pad, vel, -1, 0, gate };
            if (running)                          // remember it for Capture
            {
                auto& e = capLog[capCount.load() % capSize];
                e.ppq.store (ppq0 + pos * ppqPerSample); e.pad.store (pad); e.vel.store (vel);
                capCount.fetch_add (1);
            }

            if (running && seqRecord.load())      // live record, quantized to the nearest step
            {
                auto& pat = patterns[jlimit (0, Seq::numPatterns - 1, currentPattern.load())];
                const int len = jlimit (1, Seq::maxSteps, pat.length.load());
                const double stepLen = pat.stepLength();
                const double swingOff = (2.0 * jlimit (50.0f, 75.0f, swing.load()) / 100.0 - 1.0);
                const double t = ppq0 + pos * ppqPerSample;
                const long long best = nearestStep (t, stepLen, swingOff);
                const double bestTime = best * stepLen + ((best & 1) ? swingOff * stepLen : 0.0);
                const int idx = (int) (best % len);
                pat.setStep (pad, idx, (uint8_t) jlimit (1, 127, (int) std::lround (vel * 127.0f)));
                if (bestTime > t) skipOnce[pad][idx] = 1;   // already heard it; don't double-trigger
            }
    };

    // TempoPAD User mode: a pad press turns that step on/off for the selected pad (top-left = step 1).
    auto toggleStep = [&] (int step, float vel, int pos)
    {
        auto& pat = patterns[jlimit (0, Seq::numPatterns - 1, currentPattern.load())];
        if (step >= jlimit (1, Seq::maxSteps, pat.length.load())) return;
        const int target = jlimit (0, PadLayout::numPads - 1, stepTarget.load());
        if (pat.vel[target][step].load() > 0)
            pat.vel[target][step].store (0);
        else
        {
            pat.setStep (target, step, (uint8_t) jlimit (1, 127, (int) std::lround (vel * 127.0f)));
            if (numHits < maxHits) hits[numHits++] = { pos, target, vel * 0.7f, -1, 0, (int64) (0.3 * hostRate) };   // hear what you added
        }
        lastStepEdit = step;
        stepEdits.fetch_add (1);
    };

    // Roll: remember which pads are held so they can repeat in time
    const int rollRate = rollDiv.load();
    const double bpmNow = running ? ppqPerSample * 60.0 * hostRate : (hostIsPlaying ? hostBpm.load() : internalBpm.load());
    const double rollSamples = rollRate > 0 ? hostRate * 60.0 / jmax (20.0, bpmNow) / rollRate : 0.0;
    auto press = [&] (int pad, float vel, int pos)
    {
        auto& h = held[pad];
        h.on = true; h.vel = vel;
        h.pressPpq = running ? ppq0 + pos * ppqPerSample : -1.0;
        h.nextSample = (double) (sampleClock + pos) + rollSamples;
    };

    const bool padModes = tempoPadModes.load();
    const int keysTrackIn = keysInput.load();
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        const int pos = jlimit (0, numSamples - 1, meta.samplePosition);
        if (msg.isSysEx())
        {
            const auto* d = msg.getSysExData();
            const int n = msg.getSysExDataSize();
            if (n >= 4 && d[0] == 0x7F && d[2] == 0x06)          // MMC transport (TempoPAD with MMC on)
            {
                switch (d[3])
                {
                    case 0x01: case 0x09: internalPlaying = false; seqRecord = false; stopSoundsReq = true; break;   // stop / pause
                    case 0x02: case 0x03: if (! hostIsPlaying) internalPlaying = true; break;  // play
                    case 0x06:                                                            // record: toggle
                        seqRecord = ! seqRecord.load();
                        if (seqRecord.load() && ! hostIsPlaying) internalPlaying = true;
                        break;
                    case 0x07: seqRecord = false; break;                                  // record exit
                    default: break;
                }
            }
            else if (n == 1 && padModes)                          // TempoPAD Easy mode: one byte per pad
            {
                const int b = d[0];
                if ((b & 0x40) != 0)                              // release
                {
                    const int rp = noteToPad[36 + (b & 0x3F)];
                    if (rp >= 0) { held[rp].on = false; if (numHits < maxHits) hits[numHits++] = { pos, rp, -1.0f }; }
                    continue;
                }
                const int pad = noteToPad[36 + (b & 0x3F)];
                if (pad < 0) continue;
                ctrlMode = ctrlPatterns;
                if (pad / PadLayout::cols == 0) selectPattern (pad % PadLayout::cols);   // top row = patterns 1-8
                else { playPad (pad, 0.8f, pos); press (pad, 0.8f, pos); }
            }
            continue;
        }
        if (msg.isNoteOn())
        {
            const int pad = noteToPad[msg.getNoteNumber()];
            if (pad < 0 && keysTrackIn < 0) continue;
            if (padModes && msg.getChannel() == 3)               // TempoPAD User mode
            {
                ctrlMode = ctrlSteps;
                if (pad >= 0) toggleStep (pad, msg.getFloatVelocity(), pos);
                continue;
            }
            if (keysTrackIn >= 0 && ! tapActive)               // pads play the Keys track shown in the Sequencer
            {
                const int note = msg.getNoteNumber();
                const float v = msg.getFloatVelocity();
                if (numHits < maxHits) hits[numHits++] = { pos, -1, v, keysTrackIn, note, -1 };
                keyDown[note] = { keysTrackIn, running ? ppq0 + pos * ppqPerSample : -1.0, v, currentPattern.load() };
                continue;
            }
            if (padModes) { ctrlMode = ctrlPads; stepTarget = pad; }
            playPad (pad, msg.getFloatVelocity(), pos);
            press (pad, msg.getFloatVelocity(), pos);
        }
        else if (msg.isNoteOff())
        {
            const int note = msg.getNoteNumber();
            auto& kd = keyDown[note];
            if (kd.track >= 0)                                  // a held Keys note: end it (and record it)
            {
                if (numHits < maxHits) hits[numHits++] = { pos, -1, -1.0f, kd.track, note, 0 };
                if (running && seqRecord.load() && kd.ppq >= 0.0)
                {
                    const double off = ppq0 + pos * ppqPerSample;
                    int s1, n1, s2, n2;
                    keyRecFifo.prepareToWrite (1, s1, n1, s2, n2);
                    if (n1 > 0) { keyRecBuf[s1] = { kd.track, kd.pattern, note, kd.ppq, jmax (off, kd.ppq), kd.vel }; keyRecFifo.finishedWrite (1); }
                }
                kd = {};
                continue;
            }
            const int pad = noteToPad[note];
            if (pad >= 0)
            {
                held[pad].on = false;
                if (numHits < maxHits) hits[numHits++] = { pos, pad, -1.0f };   // let go: Hold pads stop
            }
        }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
        {
            for (auto& v : voices) if (v.active) v.release (hostRate);
            for (auto& h : held) h.on = false;
            for (auto& k : keyDown) k = {};
        }
    }

    // Roll: held pads repeat on the beat grid while playing, or at the tempo from the press while stopped
    if (rollRate > 0 && ! tapActive)
    {
        const double iv = 1.0 / rollRate;
        const double ppq1 = ppq0 + ppqPerSample * numSamples;
        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            auto& h = held[pad];
            if (! h.on) continue;
            if (auto ps = std::atomic_load (&pads[pad]); ps != nullptr && ps->play == PlayMode::Toggle) continue;   // no rolls on toggles
            const auto rollGate = (int64) jmax (1.0, rollSamples * 0.9);   // Hold pads: each repeat lasts until the next
            if (running)
            {
                for (double g = std::ceil (ppq0 / iv) * iv; g < ppq1; g += iv)
                {
                    if (h.pressPpq >= 0.0 && g < h.pressPpq + 0.5 * iv) continue;   // too close to the press itself
                    playPad (pad, h.vel, jlimit (0, numSamples - 1, (int) ((g - ppq0) / ppqPerSample)), rollGate);
                }
            }
            else
            {
                while (h.nextSample < (double) (sampleClock + numSamples))
                {
                    playPad (pad, h.vel, jlimit (0, numSamples - 1, (int) (h.nextSample - (double) sampleClock)), rollGate);
                    h.nextSample += jmax (1.0, rollSamples);
                }
            }
        }
    }
    else
        for (auto& h : held) h.nextSample = (double) sampleClock;
    sampleClock += numSamples;

    if (running)
    {
        scheduleSequencer (ppq0, ppqPerSample, numSamples, hits, numHits, maxHits);
        scheduleKeys (ppq0, ppqPerSample, numSamples, hits, numHits, maxHits);
    }
    else
        for (auto& k : keysStep) k = -1;

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
        const auto& h = hits[i];
        if (h.track >= 0) { if (h.velocity < 0.0f) releaseKey (h.track, h.pitch); else handleKeyHit (h.track, h.pitch, h.velocity, h.gate); }
        else if (h.velocity < -1.5f) { for (auto& v : voices) if (v.active && v.pad == h.pad && v.keysTrack < 0) v.noteOff(); }
        else if (h.velocity < 0.0f) padRelease (h.pad);
        else handleHit (h.pad, h.velocity, h.gate);
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

    // browser player (with short fades so pause / stop never click)
    {
        auto ps = std::atomic_load (&playerSample);
        const double seek = playerSeekReq.exchange (-1.0);
        if (seek >= 0.0) playerPos = seek;
        const bool stopping = playerStopReq.load();
        const float target = playerPlaying.load() ? 1.0f : 0.0f;
        if (ps != nullptr && (target > 0.0f || playerAmp > 1.0e-4f))
        {
            const auto& a = ps->audio;
            const int len = a.getNumSamples();
            const double step = ps->sampleRate / hostRate;
            const bool loop = playerLoop.load();
            const float* sl = a.getReadPointer (0);
            const float* sr = a.getReadPointer (1);
            auto* L = buffer.getWritePointer (0);
            auto* R = buffer.getWritePointer (1);
            for (int i = 0; i < numSamples; ++i)
            {
                playerAmp += (target - playerAmp) * 0.01f;             // fade of ~15 ms
                if (target == 0.0f && playerAmp < 1.0e-3f) { playerAmp = 0.0f; break; }   // fully faded: hold position
                int idx = (int) playerPos;
                if (idx >= len - 1)
                {
                    if (loop && len > 1) { playerPos = 0.0; idx = 0; }
                    else { playerPlaying = false; playerPos = 0.0; playerAmp = 0.0f; break; }
                }
                const float frac = (float) (playerPos - idx);
                const float g = 0.8f * playerAmp;
                L[i] += g * (sl[idx] + frac * (sl[idx + 1] - sl[idx]));
                R[i] += g * (sr[idx] + frac * (sr[idx + 1] - sr[idx]));
                playerPos += step;
            }
            if (! stopping) playerPosOut = playerPos;
        }
        if (stopping && playerAmp <= 1.0e-4f) { playerPos = 0.0; playerAmp = 0.0f; playerStopReq = false; playerPosOut = 0.0; }
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
        if (p->fadeOutMs > 0.0f) pe->setAttribute ("fadeOut", (double) p->fadeOutMs);
        pe->setAttribute ("sustain", (double) p->sustain);
        pe->setAttribute ("release", (double) p->releaseMs);
        pe->setAttribute ("curveA", (double) p->curveA);
        pe->setAttribute ("curveD", (double) p->curveD);
        pe->setAttribute ("curveR", (double) p->curveR);
        pe->setAttribute ("cutoff", (double) p->cutoff);
        pe->setAttribute ("reso", (double) p->reso);
        pe->setAttribute ("reverse", p->reverse);
        pe->setAttribute ("mono", p->mono);
        pe->setAttribute ("choke", p->choke);
        pe->setAttribute ("play", (int) p->play);
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
        pe->setAttribute ("stepsPer2Beats", pat.stepsPer2Beats.load());
        if (pat.stepsPer2Beats.load() % 2 == 0)                       // older versions read this one
            pe->setAttribute ("stepsPerBeat", pat.stepsPer2Beats.load() / 2);
        for (int pad = 0; pad < PadLayout::numPads; ++pad)
        {
            if (! pat.padHasSteps (pad)) continue;
            auto* re = pe->createNewChildElement ("Row");
            re->setAttribute ("pad", pad);
            re->setAttribute ("vel", toHex (pat.vel[pad], Seq::maxSteps));
            re->setAttribute ("rat", toHex (pat.ratchet[pad], Seq::maxSteps));
            re->setAttribute ("chance", toHex (pat.chance[pad], Seq::maxSteps));
            re->setAttribute ("cond", toHex (pat.cond[pad], Seq::maxSteps));
            re->setAttribute ("nudge", toHex (pat.nudge[pad], Seq::maxSteps));
        }
        for (int t = 0; t < Keys::numTracks; ++t)
        {
            const auto& lane = pat.lanes[t];
            auto notes = lane.get();
            if (notes == nullptr && lane.length.load() == 16 && lane.stepsPer2Beats.load() == 4) continue;
            auto* le = pe->createNewChildElement ("Lane");
            le->setAttribute ("track", t);
            le->setAttribute ("length", lane.length.load());
            le->setAttribute ("stepsPer2Beats", lane.stepsPer2Beats.load());
            String hex;                                       // 4 bytes per note: pitch, step, length, velocity
            if (notes != nullptr)
                for (const auto& n : *notes)
                    for (int b : { (int) n.pitch, (int) n.step, (int) n.len, (int) n.vel })
                        hex << String::toHexString (b).paddedLeft ('0', 2);
            le->setAttribute ("notes", hex);
        }
    }
    seq->setAttribute ("scaleRoot", scaleRoot.load());
    seq->setAttribute ("scaleType", scaleType.load());
    for (int t = 0; t < Keys::numTracks; ++t)
    {
        const auto& kt = keysTracks[t];
        auto* ke = seq->createNewChildElement ("KeysTrack");
        ke->setAttribute ("index", t);
        ke->setAttribute ("name", kt.name);
        ke->setAttribute ("sourcePad", kt.sourcePad.load());
        ke->setAttribute ("root", kt.root.load());
        ke->setAttribute ("mono", kt.mono.load());
        ke->setAttribute ("glide", (double) kt.glideMs.load());
        ke->setAttribute ("level", (double) kt.level.load());
        if (kt.sliceCount.load() > 0) ke->setAttribute ("slices", kt.sliceCount.load());
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
        st->fadeOutMs = (float) pe->getDoubleAttribute ("fadeOut", 0.0);
        if (pe->hasAttribute ("sustain"))
        {
            st->sustain = (float) pe->getDoubleAttribute ("sustain", 1.0);
            st->releaseMs = (float) pe->getDoubleAttribute ("release", 100.0);
            st->curveA = (float) pe->getDoubleAttribute ("curveA", 0.0);
            st->curveD = (float) pe->getDoubleAttribute ("curveD", 0.7);
            st->curveR = (float) pe->getDoubleAttribute ("curveR", 0.8);
        }
        else if (st->decayMs >= 4999.0f) st->decayMs = 300.0f;      // kit from before 0.9.8, Decay off: plays the whole region
        else { st->sustain = 0.0f; st->curveD = 1.0f; }             // old Decay knob: an exponential fade to silence
        st->cutoff = (float) pe->getDoubleAttribute ("cutoff", 20000.0);
        st->reso = (float) pe->getDoubleAttribute ("reso", 0.0);
        st->reverse = pe->getBoolAttribute ("reverse", false);
        st->mono = pe->getBoolAttribute ("mono", false);
        st->choke = jlimit (0, 8, pe->getIntAttribute ("choke", 0));
        st->play = (PlayMode) jlimit (0, 2, pe->getIntAttribute ("play", 0));
        for (auto* se : pe->getChildWithTagNameIterator ("Sample"))
            if (auto s = loadSample (File (se->getStringAttribute ("file"))))
                st->layers.push_back (s);
        setPad (i, st);
    }

    for (auto& pat : patterns) { pat.clear(); pat.length = 16; pat.stepsPer2Beats = 8; }
    resetKeysTracks();
    scaleRoot = 0; scaleType = 0;
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
            pat.stepsPer2Beats = pe->hasAttribute ("stepsPer2Beats") ? jlimit (2, 16, pe->getIntAttribute ("stepsPer2Beats", 8))
                                                                     : jlimit (1, 8, pe->getIntAttribute ("stepsPerBeat", 4)) * 2;
            for (auto* re : pe->getChildWithTagNameIterator ("Row"))
            {
                const int pad = re->getIntAttribute ("pad", -1);
                if (! isPositiveAndBelow (pad, PadLayout::numPads)) continue;
                fromHex (re->getStringAttribute ("vel"), pat.vel[pad], Seq::maxSteps, 0);
                fromHex (re->getStringAttribute ("rat"), pat.ratchet[pad], Seq::maxSteps, 1);
                fromHex (re->getStringAttribute ("chance"), pat.chance[pad], Seq::maxSteps, 100);   // older kits: plain steps
                fromHex (re->getStringAttribute ("cond"), pat.cond[pad], Seq::maxSteps, 0);
                fromHex (re->getStringAttribute ("nudge"), pat.nudge[pad], Seq::maxSteps, 128);
                for (int s2 = 0; s2 < Seq::maxSteps; ++s2)
                {
                    pat.chance[pad][s2].store ((uint8_t) jlimit (1, 100, (int) pat.chance[pad][s2].load()));
                    pat.cond[pad][s2].store ((uint8_t) jlimit (0, (int) Seq::numConds - 1, (int) pat.cond[pad][s2].load()));
                    pat.nudge[pad][s2].store ((uint8_t) jlimit (78, 178, (int) pat.nudge[pad][s2].load()));
                }
            }
            for (auto* le : pe->getChildWithTagNameIterator ("Lane"))
            {
                const int t = le->getIntAttribute ("track", -1);
                if (! isPositiveAndBelow (t, Keys::numTracks)) continue;
                auto& lane = pat.lanes[t];
                lane.length = jlimit (1, Seq::maxSteps, le->getIntAttribute ("length", 16));
                lane.stepsPer2Beats = jlimit (2, 16, le->getIntAttribute ("stepsPer2Beats", 4));
                const String hex = le->getStringAttribute ("notes");
                Keys::NoteList list;
                for (int i = 0; i + 8 <= hex.length(); i += 8)
                {
                    auto byte = [&] (int k) { return hex.substring (i + k * 2, i + k * 2 + 2).getHexValue32(); };
                    Keys::Note n { (uint8_t) jlimit (0, 127, byte (0)), (uint8_t) jlimit (0, Seq::maxSteps - 1, byte (1)),
                                   (uint8_t) jlimit (1, Seq::maxSteps, byte (2)), (uint8_t) jlimit (1, 127, byte (3)) };
                    list.push_back (n);
                }
                lane.set (makeNotes (std::move (list)));
            }
        }
        scaleRoot = jlimit (0, 11, seq->getIntAttribute ("scaleRoot", 0));
        scaleType = jlimit (0, Keys::numScales - 1, seq->getIntAttribute ("scaleType", 0));
        for (auto* ke : seq->getChildWithTagNameIterator ("KeysTrack"))
        {
            const int t = ke->getIntAttribute ("index", -1);
            if (! isPositiveAndBelow (t, Keys::numTracks)) continue;
            auto& kt = keysTracks[t];
            kt.name = ke->getStringAttribute ("name", kt.name);
            kt.sourcePad = jlimit (-1, PadLayout::numPads - 1, ke->getIntAttribute ("sourcePad", -1));
            kt.root = jlimit (0, 127, ke->getIntAttribute ("root", 60));
            kt.mono = ke->getBoolAttribute ("mono", false);
            kt.glideMs = (float) jlimit (0.0, 1000.0, ke->getDoubleAttribute ("glide", 0.0));
            kt.level = (float) jlimit (0.0, 2.0, ke->getDoubleAttribute ("level", 1.0));
            kt.sliceCount = jlimit (0, PadLayout::numPads, ke->getIntAttribute ("slices", 0));
        }
        currentPattern = jlimit (0, Seq::numPatterns - 1, seq->getIntAttribute ("pattern", 0));
    }
    kitLoads.fetch_add (1);
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
