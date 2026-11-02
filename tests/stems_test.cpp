// Offline test for StemSeparator: STFT round trip, chunk stitching, normalisation, resampling and
// cancel, using stub ONNX models (tests/make_stub_model.py) that have HTDemucs' tensor signature.
//   stems_test <folder holding stub_wave.onnx and stub_spec.onnx> [dump-spec.bin]
#include "../Source/StemSeparator.h"
#include <juce_events/juce_events.h>
#include <cstdio>
using namespace juce;

static int fails = 0;
static void check (bool ok, const String& what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; }

static AudioBuffer<float> makeMix (int n, double sr, float level)
{
    AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const float l = level * (float) (0.5 * std::sin (6.2831853 * 220.0 * t) + 0.3 * std::sin (6.2831853 * 1830.0 * t) + 0.1 * std::sin (6.2831853 * 6100.0 * t));
        const float r = level * (float) (0.4 * std::sin (6.2831853 * 330.0 * t + 1.0) + 0.3 * std::sin (6.2831853 * 2500.0 * t));
        b.setSample (0, i, l + 0.2f);       // a DC offset, so the mean handling is exercised
        b.setSample (1, i, r + 0.2f);
    }
    return b;
}

// Demucs normalises the whole track by its mono mean / std and undoes it afterwards, so a stub that
// scales by 0.25 in the normalised domain gives 0.25 * (x - mean) + mean.
static float monoMean (const AudioBuffer<float>& b)
{
    double s = 0;
    for (int i = 0; i < b.getNumSamples(); ++i) s += 0.5 * ((double) b.getSample (0, i) + (double) b.getSample (1, i));
    return (float) (s / b.getNumSamples());
}

struct Stems
{
    TemporaryFile files[StemSeparator::numStems] { TemporaryFile (".wav"), TemporaryFile (".wav"), TemporaryFile (".wav"), TemporaryFile (".wav") };
    AudioBuffer<float> out[StemSeparator::numStems];

    String run (StemSeparator& sep, const AudioBuffer<float>& in, double sr, StemSeparator::Progress p = {})
    {
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w[StemSeparator::numStems];
        std::array<AudioFormatWriter*, StemSeparator::numStems> raw {};
        for (int s = 0; s < StemSeparator::numStems; ++s)
        {
            std::unique_ptr<OutputStream> os (files[s].getFile().createOutputStream());
            w[s].reset (wav.createWriterFor (os.get(), 44100.0, 2, 24, {}, 0));
            os.release();
            raw[(size_t) s] = w[s].get();
        }
        auto err = sep.separate (in, sr, raw, p);
        for (auto& x : w) x.reset();                    // flush
        for (int s = 0; s < StemSeparator::numStems; ++s)
        {
            AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (files[s].getFile()));
            if (r == nullptr) continue;
            out[s].setSize (2, (int) r->lengthInSamples);
            r->read (&out[s], 0, (int) r->lengthInSamples, 0, true, true);
        }
        return err;
    }
};

int main (int, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    const File dir = File::getCurrentWorkingDirectory().getChildFile (argv[1] != nullptr ? argv[1] : ".");

    // ---- 1. STFT / inverse STFT ----
    {
        using namespace StemDsp;
        std::vector<float> x ((size_t) segment);
        for (int i = 0; i < segment; ++i)
            x[(size_t) i] = (float) (0.5 * std::sin (6.2831853 * 220.0 * i / sampleRate) + 0.3 * std::sin (6.2831853 * 1830.0 * i / sampleRate)
                                     + 0.1 * std::sin (6.2831853 * 6100.0 * i / sampleRate));
        std::vector<std::complex<float>> spec;
        forward (x.data(), spec);
        check ((int) spec.size() == frames * bins, "spectrogram has 336 x 2048 bins");
        std::vector<float> y ((size_t) segment);
        inverse (spec.data(), y.data());
        double worst = 0;
        for (int i = 4096; i < segment - 4096; ++i) worst = jmax (worst, (double) std::abs (x[(size_t) i] - y[(size_t) i]));
        check (worst < 1e-3, "STFT -> inverse STFT round trip, interior, max error " + String (worst, 7));

        if (argv[2] != nullptr && String (argv[2]) != "-")
        {
            FileOutputStream fo (File::getCurrentWorkingDirectory().getChildFile (argv[2]));
            fo.setPosition (0); fo.truncate();
            fo.write (x.data(), x.size() * sizeof (float));
            fo.write (spec.data(), spec.size() * sizeof (std::complex<float>));
        }
    }

    // ---- 2. chunk stitching, normalisation (waveform-branch stub: stem = 0.25 * mix) ----
    check (StemSeparator::runtimeAvailable(), "ONNX Runtime library found: " + StemSeparator::runtimeStatus());
    {
        StemSeparator sep;
        auto err = sep.load (dir.getChildFile ("stub_wave.onnx"));
        check (err.isEmpty(), "stub model loads " + err);
        if (err.isEmpty())
        {
            const int n = 44100 * 20;                      // 20 s = four overlapping chunks
            auto in = makeMix (n, 44100.0, 0.8f);
            Stems st;
            float lastP = 0; int calls = 0;
            err = st.run (sep, in, 44100.0, [&] (float f) { lastP = f; ++calls; return true; });
            check (err.isEmpty(), "separate() succeeds " + err);
            check (calls == 4 && lastP == 1.0f, "progress reaches 1.0 over 4 chunks");
            bool lenOk = true; double worst = 0; const float mean = monoMean (in);
            for (int s = 0; s < StemSeparator::numStems; ++s)
            {
                lenOk = lenOk && st.out[s].getNumSamples() == n;
                if (st.out[s].getNumSamples() == n)
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < n; ++i)
                            worst = jmax (worst, (double) std::abs (st.out[s].getSample (ch, i) - (0.25f * (in.getSample (ch, i) - mean) + mean)));
            }
            check (lenOk, "every stem has the input's length");
            check (worst < 2e-4, "stems = 0.25 x (mix - mean) + mean across chunk seams, max error " + String (worst, 7));
        }

        // cancel
        if (err.isEmpty())
        {
            Stems st;
            auto in = makeMix (44100 * 20, 44100.0, 0.5f);
            check (st.run (sep, in, 44100.0, [] (float) { return false; }) == "cancelled", "cancel stops the job");
        }

        // 48 kHz mono-ish input is resampled, length scales, level is preserved
        if (err.isEmpty())
        {
            Stems st;
            auto in = makeMix (48000 * 10, 48000.0, 0.8f);
            auto e2 = st.run (sep, in, 48000.0);
            const int expect = (int) std::floor (48000.0 * 10 * 44100.0 / 48000.0);
            check (e2.isEmpty() && st.out[0].getNumSamples() == expect, "48 kHz input resampled to 44.1 kHz length");
            const float m48 = monoMean (in);
            double e2sum = 0; for (int i = 0; i < in.getNumSamples(); ++i) { const double v = 0.25 * (in.getSample (0, i) - m48) + m48; e2sum += v * v; }
            const float expectRms = (float) std::sqrt (e2sum / in.getNumSamples());
            check (std::abs (st.out[0].getRMSLevel (0, 0, expect) - expectRms) < 0.01f, "resampled level preserved");
        }

        // silence must not produce NaNs
        if (err.isEmpty())
        {
            Stems st; AudioBuffer<float> silent (2, 44100 * 3); silent.clear();
            auto e3 = st.run (sep, silent, 44100.0);
            check (e3.isEmpty() && st.out[0].getMagnitude (0, st.out[0].getNumSamples()) == 0.0f, "silent input gives silent stems");
        }
    }

    // ---- 3. spectral-branch stub: STFT -> model -> inverse STFT, away from the track ends ----
    {
        StemSeparator sep;
        auto err = sep.load (dir.getChildFile ("stub_spec.onnx"));
        if (err.isEmpty())
        {
            const int n = 44100 * 20;
            auto in = makeMix (n, 44100.0, 0.8f);
            Stems st;
            err = st.run (sep, in, 44100.0);
            double worst = 0; const float mean = monoMean (in);
            if (err.isEmpty())
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 8192; i < n - 8192; ++i)
                        worst = jmax (worst, (double) std::abs (st.out[3].getSample (ch, i) - (0.25f * (in.getSample (ch, i) - mean) + mean)));
            check (err.isEmpty() && worst < 2e-3, "spectral branch reproduces 0.25 x (mix - mean) + mean, max error " + String (worst, 7) + " " + err);
        }
        else check (false, "spec stub loads " + err);
    }

    // ---- 4. optional: a real model on a real file   stems_test <dir> - <model.onnx> <audio> ----
    if (argv[2] != nullptr && argv[3] != nullptr && argv[4] != nullptr)
    {
        StemSeparator sep;
        auto err = sep.load (File (argv[3]));
        check (err.isEmpty(), "real model loads " + err);
        AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (File (argv[4])));
        if (err.isEmpty() && r != nullptr)
        {
            AudioBuffer<float> in (2, (int) r->lengthInSamples);
            r->read (&in, 0, in.getNumSamples(), 0, true, true);
            if (r->numChannels == 1) in.copyFrom (1, 0, in, 0, 0, in.getNumSamples());
            Stems st;
            const auto t0 = Time::getMillisecondCounterHiRes();
            err = st.run (sep, in, r->sampleRate, [] (float f) { std::printf ("  %.0f%%\n", f * 100.0f); std::fflush (stdout); return true; });
            const auto secs = (Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            check (err.isEmpty(), "real separation ran in " + String (secs, 1) + " s for " + String (in.getNumSamples() / r->sampleRate, 1) + " s of audio " + err);
            if (err.isEmpty())
            {
                const int n = st.out[0].getNumSamples();
                AudioBuffer<float> sum (2, n); sum.clear();
                for (int s = 0; s < StemSeparator::numStems; ++s)
                {
                    for (int ch = 0; ch < 2; ++ch) sum.addFrom (ch, 0, st.out[s], ch, 0, n);
                    std::printf ("  %-7s rms %.4f\n", StemSeparator::stemName (s), st.out[s].getRMSLevel (0, 0, n));
                }
                double num = 0, den = 0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i) { const double d = sum.getSample (ch, i) - in.getSample (ch, i); num += d * d; den += (double) in.getSample (ch, i) * in.getSample (ch, i); }
                std::printf ("  mix rms %.4f, sum-of-stems relative error %.4f\n", in.getRMSLevel (0, 0, n), std::sqrt (num / jmax (1e-12, den)));
                check (std::sqrt (num / jmax (1e-12, den)) < 0.1, "stems add up to the mix");
                for (int s = 0; s < StemSeparator::numStems; ++s)
                    std::printf ("  wrote %s\n", st.files[s].getFile().copyFileTo (File ("/tmp/stem_" + String (StemSeparator::stemName (s)) + ".wav")) ? ("/tmp/stem_" + String (StemSeparator::stemName (s)) + ".wav").toRawUTF8() : "?");
            }
        }
    }

    std::printf (fails == 0 ? "ALL PASSED\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
