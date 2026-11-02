#include "StemJob.h"

using namespace juce;

namespace
{
    StemJob* current = nullptr;
    double barValue = 0.0;     // read by the window's progress bar, which can outlive the job, so it is not a member
}

bool StemJob::busy() { return current != nullptr; }

void StemJob::start (const SamplePtr& source, Done onDone)
{
    if (current != nullptr || source == nullptr) return;
    current = new StemJob (source, std::move (onDone));
}

void StemJob::cancelAndWait()
{
    if (current != nullptr) current->finish (false);
}

StemJob::StemJob (const SamplePtr& s, Done d) : Thread ("FlipKit64 stem separation"), source (s), done (std::move (d))
{
    const bool needModel = ! StemSeparator::modelPresent();
    phaseText = needModel ? "Downloading the model (one time, about 91 MB)..." : "Loading the model...";
    shownPhase = phaseText;

    auto* w = new AlertWindow ("Splitting into stems",
                               source->file.getFileName() + "\n\n" + phaseText,
                               MessageBoxIconType::NoIcon);
    barValue = 0.0;
    w->addProgressBarComponent (barValue);
    w->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));
    window = w;
    w->enterModalState (true, ModalCallbackFunction::create ([flag = cancelRequested] (int) { *flag = true; }), true);

    startThread (Thread::Priority::low);   // keep the DAW's audio thread ahead of us
    startTimer (100);
}

StemJob::~StemJob()
{
    stopTimer();
    stopThread (30000);
    if (current == this) current = nullptr;
}

bool StemJob::progress (float f)
{
    fraction = f;
    return ! cancelRequested->load() && ! threadShouldExit();
}

void StemJob::run()
{
    auto setPhase = [this] (const String& t) { const ScopedLock sl (lock); phaseText = t; fraction = 0.0f; };

    if (! StemSeparator::modelPresent())
    {
        auto e = StemSeparator::downloadModel ([this] (float f) { return progress (f); });
        if (e.isNotEmpty()) { error = e; return; }
    }

    setPhase ("Loading the model...");
    StemSeparator sep;
    auto e = sep.load();
    if (e.isNotEmpty()) { error = e; return; }

    // Output: one folder per split, one 24-bit WAV per stem.
    const auto base = File::createLegalFileName (source->file.getFileNameWithoutExtension());
    outFolder = PadRomplerProcessor::recordingsFolder().getChildFile ("Edits").getNonexistentChildFile (base + " stems", {}, false);
    if (! outFolder.createDirectory()) { error = "Could not create " + outFolder.getFullPathName(); return; }

    WavAudioFormat wav;
    std::unique_ptr<AudioFormatWriter> writers[StemSeparator::numStems];
    std::array<AudioFormatWriter*, StemSeparator::numStems> raw {};
    for (int s = 0; s < StemSeparator::numStems; ++s)
    {
        auto file = outFolder.getChildFile (base + " - " + StemSeparator::stemName (s) + ".wav");
        std::unique_ptr<OutputStream> stream (file.createOutputStream());
        if (stream != nullptr)
            writers[s].reset (wav.createWriterFor (stream.get(), (double) StemDsp::sampleRate, 2, 24, {}, 0));
        if (writers[s] == nullptr) { error = "Could not write " + file.getFullPathName(); break; }
        stream.release();
        raw[(size_t) s] = writers[s].get();
        outFiles.add (file);
    }

    if (error.isEmpty())
    {
        setPhase ("Separating drums, bass, other and vocals...");
        error = sep.separate (source->audio, source->sampleRate, raw, [this] (float f) { return progress (f); });
    }
    for (auto& w : writers) w.reset();      // flush and close

    if (error.isNotEmpty()) { outFolder.deleteRecursively(); outFiles.clear(); }
}

void StemJob::timerCallback()
{
    if (isThreadRunning())
    {
        String t;
        { const ScopedLock sl (lock); t = phaseText; }
        barValue = (double) fraction.load();
        if (t != shownPhase && window != nullptr)
        {
            shownPhase = t;
            window->setMessage (source->file.getFileName() + "\n\n" + t);
        }
        return;
    }
    finish (true);
}

// Deletes this. Called from the timer when the thread ends, or (without the callback) when the editor goes away.
void StemJob::finish (bool callDone)
{
    stopTimer();
    *cancelRequested = true;
    stopThread (30000);
    if (window != nullptr) window->exitModalState (0);
    auto cb = std::move (done);
    const auto err = error;
    const auto folder = outFolder;
    const auto files = outFiles;
    current = nullptr;
    delete this;
    if (callDone && cb) cb (err, folder, files);
}
