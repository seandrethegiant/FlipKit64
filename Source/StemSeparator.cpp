#include "StemSeparator.h"
#include <onnxruntime_c_api.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace juce;

// ---------------------------------------------------------------------------------------------
// Model location. htdemucs_fp16.onnx from HRSadeghi/demucs-onnx on Hugging Face (fp16 weights, fp32
// I/O, 91.3 MB; the graph is cut at the complex boundary, so STFT / iSTFT / overlap-add are done
// here). Not hosted by FlipKit64.
// LICENCE: the repo's README says the weights carry Meta's Demucs terms and calls them
// non-commercial (CC BY-NC 4.0); Meta's own repo is MIT and other mirrors say MIT. Check before
// anyone uses this in a paid product.
// ---------------------------------------------------------------------------------------------
static const char* const kModelUrl  = "https://huggingface.co/HRSadeghi/demucs-onnx/resolve/main/htdemucs_fp16.onnx";
static const char* const kModelName = "htdemucs_fp16.onnx";
static constexpr int64   kModelBytes = 91312383;                 // exact size, checked after download and on load

// =============================================================================================
// StemDsp: STFT / inverse STFT exactly as Demucs does them around the network.
// =============================================================================================
namespace
{
    using cf = std::complex<float>;

    struct Fft
    {
        int n;
        std::vector<cf> tw;
        std::vector<int> rev;

        explicit Fft (int size) : n (size), tw ((size_t) size / 2), rev ((size_t) size)
        {
            for (int i = 0; i < n / 2; ++i)
            {
                const double a = -2.0 * MathConstants<double>::pi * i / n;
                tw[(size_t) i] = cf ((float) std::cos (a), (float) std::sin (a));
            }
            int bits = 0; while ((1 << bits) < n) ++bits;
            for (int i = 0; i < n; ++i)
            {
                int r = 0;
                for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
                rev[(size_t) i] = r;
            }
        }

        // In place. inverse = true is the unnormalised inverse transform (no 1/n).
        void run (cf* a, bool inverse) const
        {
            for (int i = 0; i < n; ++i) { const int r = rev[(size_t) i]; if (r > i) std::swap (a[i], a[r]); }
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len / 2, step = n / len;
                for (int i = 0; i < n; i += len)
                    for (int j = 0; j < half; ++j)
                    {
                        cf w = tw[(size_t) (j * step)];
                        if (inverse) w = std::conj (w);
                        const cf u = a[i + j], v = a[i + j + half] * w;
                        a[i + j] = u + v;
                        a[i + j + half] = u - v;
                    }
            }
        }
    };

    const Fft& fft()
    {
        static const Fft f (StemDsp::nfft);
        return f;
    }

    // torch.hann_window (periodic)
    const std::vector<float>& hann()
    {
        static const std::vector<float> w = []
        {
            std::vector<float> v ((size_t) StemDsp::nfft);
            for (int i = 0; i < StemDsp::nfft; ++i)
                v[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * MathConstants<double>::pi * i / StemDsp::nfft));
            return v;
        }();
        return w;
    }

    // torch "reflect" padding (the edge sample is not repeated)
    std::vector<float> reflectPad (const float* x, int len, int left, int right)
    {
        std::vector<float> out ((size_t) (left + len + right));
        for (int j = 0; j < (int) out.size(); ++j)
        {
            int i = j - left;
            const int period = 2 * (len - 1);
            i = ((i % period) + period) % period;
            if (i >= len) i = period - i;
            out[(size_t) j] = x[i];
        }
        return out;
    }

    constexpr int padHalf = StemDsp::hop / 2 * 3;                               // 1536
    constexpr int paddedLen = StemDsp::frames * StemDsp::hop;                   // 344064
    constexpr int centre = StemDsp::nfft / 2;                                   // 2048
    constexpr int totalFrames = 1 + (StemDsp::segment + padHalf + (padHalf + paddedLen - StemDsp::segment)
                                     + 2 * centre - StemDsp::nfft) / StemDsp::hop;   // 340
    static_assert (totalFrames == StemDsp::frames + 4, "frame layout");
}

void StemDsp::forward (const float* x, std::vector<cf>& spec)
{
    using namespace StemDsp;
    // Demucs: pad1d (pad, pad + le*hl - length) reflect, then torch.stft(center=True, reflect, normalized).
    auto p1 = reflectPad (x, segment, padHalf, padHalf + paddedLen - segment);
    auto p2 = reflectPad (p1.data(), (int) p1.size(), centre, centre);

    spec.assign ((size_t) frames * bins, cf());
    const auto& w = hann();
    const float scale = 1.0f / std::sqrt ((float) nfft);
    std::vector<cf> buf ((size_t) nfft);

    for (int f = 2; f < 2 + frames; ++f)      // Demucs keeps frames [2, 2 + le)
    {
        const float* src = p2.data() + (size_t) f * hop;
        for (int n = 0; n < nfft; ++n) buf[(size_t) n] = cf (src[n] * w[(size_t) n] * scale, 0.0f);
        fft().run (buf.data(), false);
        std::copy (buf.begin(), buf.begin() + bins, spec.begin() + (size_t) (f - 2) * bins);
    }
}

void StemDsp::inverse (const cf* spec, float* out)
{
    using namespace StemDsp;
    const int olaLen = nfft + hop * (totalFrames - 1);
    std::vector<float> ola ((size_t) olaLen, 0.0f), env ((size_t) olaLen, 0.0f);
    const auto& w = hann();
    std::vector<cf> buf ((size_t) nfft);

    for (int f = 0; f < totalFrames; ++f)
    {
        float* e = env.data() + (size_t) f * hop;
        for (int n = 0; n < nfft; ++n) e[n] += w[(size_t) n] * w[(size_t) n];

        if (f < 2 || f >= 2 + frames) continue;          // Demucs zero-pads two frames each side
        const cf* X = spec + (size_t) (f - 2) * bins;
        for (int k = 0; k < bins; ++k) { buf[(size_t) k] = X[k]; }
        buf[(size_t) bins] = cf();                       // the dropped Nyquist bin
        for (int k = 1; k < bins; ++k) buf[(size_t) (nfft - k)] = std::conj (X[k]);
        fft().run (buf.data(), true);

        const float scale = 1.0f / std::sqrt ((float) nfft);
        float* o = ola.data() + (size_t) f * hop;
        for (int n = 0; n < nfft; ++n) o[n] += buf[(size_t) n].real() * scale * w[(size_t) n];
    }

    const int start = centre + padHalf;
    for (int i = 0; i < segment; ++i)
    {
        const float e = env[(size_t) (start + i)];
        out[i] = e > 1e-11f ? ola[(size_t) (start + i)] / e : 0.0f;
    }
}

// =============================================================================================
// ONNX Runtime, loaded at run time
// =============================================================================================
namespace
{
    String libName()
    {
       #if JUCE_WINDOWS
        return "onnxruntime.dll";
       #elif JUCE_MAC
        return "libonnxruntime.dylib";
       #else
        return "libonnxruntime.so";
       #endif
    }

    // Looked up by full path first. On Windows we never fall back to the bare name, because
    // System32 ships an old, unrelated onnxruntime.dll.
    Array<File> runtimeCandidates()
    {
        Array<File> c;
        const auto name = libName();
        for (auto dir : { StemSeparator::stemsFolder(),
                          File::getSpecialLocation (File::currentExecutableFile).getParentDirectory(),
                          File::getSpecialLocation (File::currentApplicationFile).getParentDirectory() })
        {
            auto f = dir.getChildFile (name);
            if (f.existsAsFile()) c.addIfNotAlreadyThere (f);
            if (dir.isDirectory())
                for (auto& v : dir.findChildFiles (File::findFiles, false, name + "*"))
                    c.addIfNotAlreadyThere (v);
        }
        return c;
    }

    struct Runtime
    {
        DynamicLibrary lib;
        const OrtApi* api = nullptr;
        String error;
    };

    Runtime* getRuntime()
    {
        static Runtime* rt = []
        {
            auto* r = new Runtime();   // intentionally leaked: the library must outlive every session
            auto tryOpen = [r] (const String& path) -> bool
            {
                if (! r->lib.open (path)) return false;
                using GetBase = const OrtApiBase* (ORT_API_CALL*) (void);
                auto getBase = (GetBase) r->lib.getFunction ("OrtGetApiBase");
                if (getBase == nullptr) { r->lib.close(); return false; }
                // Older runtimes return null for a newer API version; the tables only ever grow,
                // so an older table is fine for the handful of calls used here.
                for (int v = ORT_API_VERSION; v >= 14 && r->api == nullptr; --v)
                    r->api = getBase()->GetApi ((uint32_t) v);
                if (r->api == nullptr) { r->lib.close(); return false; }
                return true;
            };
            for (auto& f : runtimeCandidates())
                if (tryOpen (f.getFullPathName())) return r;
           #if ! JUCE_WINDOWS
            if (tryOpen (libName())) return r;
           #endif
            r->error = "ONNX Runtime (" + libName() + ") not found. Put it in " + StemSeparator::stemsFolder().getFullPathName();
            return r;
        }();
        return rt;
    }
}

// =============================================================================================
// StemSeparator
// =============================================================================================
struct StemSeparator::Impl
{
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* mem = nullptr;
    std::string inName[2], outName[2];       // [0] = mix / out_wave, [1] = spec / out_spec

    // Tensor layouts, found from the model so either common export order works.
    struct Axes { int t = -1, c = -1, f = -1, s = -1; int64_t stride[5] {}; };
    Axes spec, outSpec;
    int64_t specElems = 0, outSpecElems = 0;
    bool waveFirstOutput = true;             // out_wave is the rank-4 output

    ~Impl()
    {
        if (api == nullptr) return;
        if (session) api->ReleaseSession (session);
        if (opts)    api->ReleaseSessionOptions (opts);
        if (mem)     api->ReleaseMemoryInfo (mem);
        if (env)     api->ReleaseEnv (env);
    }

    String status (OrtStatus* s)
    {
        if (s == nullptr) return {};
        String m = String::fromUTF8 (api->GetErrorMessage (s));
        api->ReleaseStatus (s);
        return m.isEmpty() ? String ("ONNX Runtime error") : m;
    }

    static void fillStrides (Axes& a, const std::vector<int64_t>& dims)
    {
        int64_t s = 1;
        for (int i = (int) dims.size() - 1; i >= 0; --i) { a.stride[i] = s; s *= std::max<int64_t> (1, dims[(size_t) i]); }
    }

    // Reads dims of an input or output. Dynamic axes come back as -1.
    String dimsOf (bool input, size_t index, std::vector<int64_t>& dims)
    {
        OrtTypeInfo* ti = nullptr;
        auto e = status (input ? api->SessionGetInputTypeInfo (session, index, &ti)
                               : api->SessionGetOutputTypeInfo (session, index, &ti));
        if (e.isNotEmpty()) return e;
        const OrtTensorTypeAndShapeInfo* info = nullptr;
        e = status (api->CastTypeInfoToTensorInfo (ti, &info));
        if (e.isEmpty())
        {
            size_t n = 0;
            e = status (api->GetDimensionsCount (info, &n));
            if (e.isEmpty()) { dims.resize (n); e = status (api->GetDimensions (info, dims.data(), n)); }
        }
        api->ReleaseTypeInfo (ti);
        return e;
    }

    String nameOf (bool input, size_t index, std::string& out)
    {
        OrtAllocator* alloc = nullptr;
        auto e = status (api->GetAllocatorWithDefaultOptions (&alloc));
        if (e.isNotEmpty()) return e;
        char* n = nullptr;
        e = status (input ? api->SessionGetInputName (session, index, alloc, &n)
                          : api->SessionGetOutputName (session, index, alloc, &n));
        if (e.isEmpty() && n != nullptr) { out = n; alloc->Free (alloc, n); }
        return e;
    }

    // Axis order of a spectrogram tensor: which position holds time, channels, bins (and stems).
    static bool findAxes (const std::vector<int64_t>& d, Axes& a, bool withStem)
    {
        a = Axes();
        for (int i = 0; i < (int) d.size(); ++i)
        {
            if (d[(size_t) i] == StemDsp::frames && a.t < 0) a.t = i;
            else if (d[(size_t) i] == StemDsp::bins && a.f < 0) a.f = i;
        }
        std::vector<int> rest;
        for (int i = 1; i < (int) d.size(); ++i) if (i != a.t && i != a.f) rest.push_back (i);   // axis 0 is batch
        if (a.t < 0 || a.f < 0) return false;
        if (withStem) { if (rest.size() != 2) return false; a.s = rest[0]; a.c = rest[1]; }
        else          { if (rest.size() != 1) return false; a.c = rest[0]; }
        fillStrides (a, d);
        return true;
    }

    String init (const File& modelFile)
    {
        auto* rt = getRuntime();
        if (rt->api == nullptr) return rt->error;
        api = rt->api;

        auto e = status (api->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "flipkit64", &env));
        if (e.isNotEmpty()) return e;
        e = status (api->CreateSessionOptions (&opts));
        if (e.isNotEmpty()) return e;
        // Leave a core free for the DAW's own audio thread.
        status (api->SetIntraOpNumThreads (opts, std::max (1, SystemStats::getNumCpus() - 1)));
        status (api->SetSessionGraphOptimizationLevel (opts, ORT_ENABLE_ALL));

       #if JUCE_WINDOWS
        e = status (api->CreateSession (env, modelFile.getFullPathName().toWideCharPointer(), opts, &session));
       #else
        e = status (api->CreateSession (env, modelFile.getFullPathName().toRawUTF8(), opts, &session));
       #endif
        if (e.isNotEmpty()) return "Could not load the model: " + e;

        e = status (api->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &mem));
        if (e.isNotEmpty()) return e;

        size_t nIn = 0, nOut = 0;
        status (api->SessionGetInputCount (session, &nIn));
        status (api->SessionGetOutputCount (session, &nOut));
        if (nIn != 2 || nOut != 2) return "Unexpected model: expected 2 inputs and 2 outputs";

        // Inputs: rank 3 = mix [1,2,N], rank 4 = spec. Outputs: rank 5 = out_spec, rank 4 = out_wave.
        std::vector<int64_t> d[2];
        int mixIdx = -1, specIdx = -1;
        for (size_t i = 0; i < 2; ++i)
        {
            e = dimsOf (true, i, d[i]);
            if (e.isNotEmpty()) return e;
            if (d[i].size() == 3) mixIdx = (int) i; else if (d[i].size() == 4) specIdx = (int) i;
        }
        if (mixIdx < 0 || specIdx < 0) return "Unexpected model inputs";
        if (d[mixIdx][2] != StemDsp::segment && d[mixIdx][2] > 0) return "Unsupported model: wrong segment length";
        if (! findAxes (d[specIdx], spec, false)) return "Unsupported model: spectrogram input shape";
        specElems = (int64_t) StemDsp::frames * 4 * StemDsp::bins;
        inName[0] = {}; inName[1] = {};
        if ((e = nameOf (true, (size_t) mixIdx, inName[0])).isNotEmpty()) return e;
        if ((e = nameOf (true, (size_t) specIdx, inName[1])).isNotEmpty()) return e;

        std::vector<int64_t> o[2];
        int waveIdx = -1, specOut = -1;
        for (size_t i = 0; i < 2; ++i)
        {
            e = dimsOf (false, i, o[i]);
            if (e.isNotEmpty()) return e;
            if (o[i].size() == 4) waveIdx = (int) i; else if (o[i].size() == 5) specOut = (int) i;
        }
        if (waveIdx < 0 || specOut < 0) return "Unexpected model outputs";
        if (! findAxes (o[specOut], outSpec, true))
        {
            // Some exports leave the output dims dynamic. The spectral output is the input with a
            // stem axis in front of the channel axis ([B,C,F,T] -> [B,S,C,F,T], [B,T,C,F] -> [B,T,S,C,F]).
            std::vector<int64_t> od;
            for (int i = 0; i < (int) d[specIdx].size(); ++i)
            {
                if (i == spec.c) od.push_back (numStems);
                od.push_back (i == spec.t ? StemDsp::frames : i == spec.f ? StemDsp::bins : i == spec.c ? 4 : 1);
            }
            outSpec = Axes();
            int pos = 0;   // walk the input axes, shifting by one after the inserted stem axis
            for (int i = 0; i < (int) d[specIdx].size(); ++i)
            {
                if (i == spec.c) { outSpec.s = pos++; outSpec.c = pos++; continue; }
                if (i == spec.t) outSpec.t = pos;
                if (i == spec.f) outSpec.f = pos;
                ++pos;
            }
            fillStrides (outSpec, od);
        }
        outSpecElems = (int64_t) StemDsp::frames * numStems * 4 * StemDsp::bins;
        if ((e = nameOf (false, (size_t) waveIdx, outName[0])).isNotEmpty()) return e;
        if ((e = nameOf (false, (size_t) specOut, outName[1])).isNotEmpty()) return e;
        return {};
    }
};

StemSeparator::StemSeparator() {}
StemSeparator::~StemSeparator() = default;

const char* StemSeparator::stemName (int stem)
{
    static const char* const names[numStems] = { "drums", "bass", "other", "vocals" };
    return (stem >= 0 && stem < numStems) ? names[stem] : "";
}

File StemSeparator::stemsFolder()
{
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("FlipKit64").getChildFile ("stems");
}

File StemSeparator::modelFile()     { return stemsFolder().getChildFile (kModelName); }
bool StemSeparator::modelPresent()  { auto f = modelFile(); return f.existsAsFile() && f.getSize() == kModelBytes; }
bool StemSeparator::runtimeAvailable() { return getRuntime()->api != nullptr; }

String StemSeparator::runtimeStatus()
{
    auto* rt = getRuntime();
    if (rt->api == nullptr) return rt->error;
    if (! modelPresent()) return "Model not downloaded yet (about 91 MB)";
    return "Ready";
}

String StemSeparator::load (const File& modelOverride)
{
    auto file = modelOverride != File() ? modelOverride : modelFile();
    if (! file.existsAsFile()) return "Model file not found: " + file.getFullPathName();
    auto m = std::make_unique<Impl>();
    auto err = m->init (file);
    if (err.isNotEmpty()) return err;
    impl = std::move (m);
    return {};
}

String StemSeparator::downloadModel (Progress progress)
{
    auto folder = stemsFolder();
    if (! folder.createDirectory()) return "Could not create " + folder.getFullPathName();
    auto part = folder.getChildFile (String (kModelName) + ".part");
    part.deleteFile();

    bool cancelled = false;
    String error;

    // 1) JUCE's own HTTP stream (WinInet on Windows). FlipKit64 is built without libcurl, so on
    //    Linux this returns nothing and we fall through to the curl command line.
    {
        URL url (kModelUrl);
        std::unique_ptr<InputStream> in (url.createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inAddress)
                                                                    .withConnectionTimeoutMs (20000)
                                                                    .withNumRedirectsToFollow (8)));
        if (in != nullptr)
        {
            std::unique_ptr<FileOutputStream> out (part.createOutputStream());
            if (out == nullptr) return "Could not write " + part.getFullPathName();
            const int64 total = in->getTotalLength() > 0 ? in->getTotalLength() : kModelBytes;
            HeapBlock<char> buf (256 * 1024);
            int64 done = 0;
            while (! in->isExhausted())
            {
                const int n = in->read (buf, 256 * 1024);
                if (n < 0) { error = "Download interrupted"; break; }
                if (n == 0) break;
                out->write (buf, (size_t) n);
                done += n;
                if (progress && ! progress ((float) jmin (1.0, (double) done / (double) total))) { cancelled = true; break; }
            }
            out.reset();
        }
        else
        {
            ChildProcess curl;
            const StringArray args { "curl", "-L", "--fail", "--silent", "--show-error", "--retry", "2",
                                     "-o", part.getFullPathName(), kModelUrl };
            if (! curl.start (args)) return "Could not download the model (no network access from here, and curl was not found)";
            while (curl.isRunning())
            {
                Thread::sleep (300);
                if (progress && ! progress ((float) jmin (1.0, (double) part.getSize() / (double) kModelBytes)))
                { cancelled = true; curl.kill(); break; }
            }
            if (! cancelled && curl.getExitCode() != 0)
                error = "Download failed: " + curl.readAllProcessOutput().trim();
        }
    }

    if (cancelled) { part.deleteFile(); return "cancelled"; }
    if (error.isNotEmpty()) { part.deleteFile(); return error; }
    if (part.getSize() != kModelBytes) { part.deleteFile(); return "The download was incomplete"; }

    auto dest = modelFile();
    dest.deleteFile();
    if (! part.moveFileTo (dest)) return "Could not save the model";
    if (progress) progress (1.0f);
    return {};
}

String StemSeparator::separate (const AudioBuffer<float>& input, double inputRate,
                                std::array<AudioFormatWriter*, numStems> writers, Progress progress)
{
    using namespace StemDsp;
    if (impl == nullptr) return "Model not loaded";
    for (auto* w : writers) if (w == nullptr) return "Missing output writer";
    if (input.getNumSamples() < 1 || input.getNumChannels() < 1 || inputRate <= 0) return "Nothing to separate";

    // ---- 44.1 kHz stereo working copy ----
    const int inLen = input.getNumSamples();
    const bool native = std::abs (inputRate - (double) sampleRate) < 0.5;
    const int total = native ? inLen : (int) std::floor ((double) inLen * sampleRate / inputRate);
    if (total < 1) return "Nothing to separate";
    AudioBuffer<float> mix (2, total);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = input.getReadPointer (jmin (ch, input.getNumChannels() - 1));
        if (native) mix.copyFrom (ch, 0, src, total);
        else
        {
            LagrangeInterpolator li;
            li.process (inputRate / sampleRate, src, mix.getWritePointer (ch), total);
        }
    }

    // ---- whole-track normalisation (demucs.separate): mono mean / std ----
    double sum = 0, sumSq = 0;
    for (int i = 0; i < total; ++i)
    {
        const double m = 0.5 * ((double) mix.getSample (0, i) + (double) mix.getSample (1, i));
        sum += m; sumSq += m * m;
    }
    const double mean = sum / total;
    const double var = total > 1 ? jmax (0.0, (sumSq - sum * mean) / (total - 1)) : 0.0;
    const float stdv = (float) std::sqrt (var);
    const float fmean = (float) mean;
    const float normStd = stdv + 1e-8f;           // demucs.api: wav /= ref.std() + 1e-8, and back
    const float invStd = 1.0f / normStd;

    // ---- chunking ----
    const int stride = (int) ((1.0 - 0.25) * segment);       // overlap 0.25, as Demucs
    std::vector<int> offsets;
    for (int off = 0; off < total; off += stride)
    {
        offsets.push_back (off);
        if (off + segment >= total) break;
    }

    std::vector<float> weight ((size_t) segment);
    const int halfSeg = segment / 2;
    for (int i = 0; i < segment; ++i) weight[(size_t) i] = (float) (i < halfSeg ? i + 1 : segment - i) / (float) halfSeg;

    // Rolling accumulators for the region [offset, offset + segment)
    std::vector<float> acc[numStems][2], accW ((size_t) segment, 0.0f);
    for (auto& s : acc) for (auto& c : s) c.assign ((size_t) segment, 0.0f);

    std::vector<float> mixIn ((size_t) 2 * segment), specIn ((size_t) impl->specElems);
    std::vector<cf> sp[2];
    std::vector<cf> osp ((size_t) frames * bins);
    std::vector<float> tmp ((size_t) segment);
    AudioBuffer<float> outBuf (2, stride);

    const std::vector<int64_t> mixDims { 1, 2, segment };
    std::vector<int64_t> specDims (4, 1), outSpecDims;
    // Real dims were validated at load; rebuild them in the model's axis order.
    {
        const auto& a = impl->spec;
        specDims[(size_t) a.t] = frames; specDims[(size_t) a.c] = 4; specDims[(size_t) a.f] = bins;
    }

    auto* api = impl->api;
    const char* inNames[2]  = { impl->inName[0].c_str(), impl->inName[1].c_str() };
    const char* outNames[2] = { impl->outName[0].c_str(), impl->outName[1].c_str() };

    for (size_t k = 0; k < offsets.size(); ++k)
    {
        const int off = offsets[k];
        const int len = jmin (segment, total - off);
        const bool last = (k + 1 == offsets.size());

        // normalised chunk, zero-padded to the model's fixed length
        std::fill (mixIn.begin(), mixIn.end(), 0.0f);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = mix.getReadPointer (ch) + off;
            float* dst = mixIn.data() + (size_t) ch * segment;
            for (int i = 0; i < len; ++i) dst[i] = (src[i] - fmean) * invStd;
        }

        // spectrogram, laid out in the model's axis order: channels [L.re, L.im, R.re, R.im]
        for (int ch = 0; ch < 2; ++ch) forward (mixIn.data() + (size_t) ch * segment, sp[ch]);
        {
            const auto& a = impl->spec;
            for (int t = 0; t < frames; ++t)
                for (int f = 0; f < bins; ++f)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const cf v = sp[ch][(size_t) (t * bins + f)];
                        specIn[(size_t) (t * a.stride[a.t] + (2 * ch)     * a.stride[a.c] + f * a.stride[a.f])] = v.real();
                        specIn[(size_t) (t * a.stride[a.t] + (2 * ch + 1) * a.stride[a.c] + f * a.stride[a.f])] = v.imag();
                    }
        }

        // ---- inference ----
        OrtValue* inVals[2] = { nullptr, nullptr };
        OrtValue* outVals[2] = { nullptr, nullptr };
        auto release = [&]
        {
            for (auto* v : inVals)  if (v) api->ReleaseValue (v);
            for (auto* v : outVals) if (v) api->ReleaseValue (v);
        };

        String err = impl->status (api->CreateTensorWithDataAsOrtValue (impl->mem, mixIn.data(), mixIn.size() * sizeof (float),
                                        mixDims.data(), mixDims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVals[0]));
        if (err.isEmpty())
            err = impl->status (api->CreateTensorWithDataAsOrtValue (impl->mem, specIn.data(), specIn.size() * sizeof (float),
                                        specDims.data(), specDims.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVals[1]));
        if (err.isEmpty())
            err = impl->status (api->Run (impl->session, nullptr, inNames, inVals, 2, outNames, 2, outVals));
        if (err.isNotEmpty()) { release(); return "Separation failed: " + err; }

        float* waveOut = nullptr; float* specOut = nullptr;
        err = impl->status (api->GetTensorMutableData (outVals[0], (void**) &waveOut));
        if (err.isEmpty()) err = impl->status (api->GetTensorMutableData (outVals[1], (void**) &specOut));
        if (err.isNotEmpty()) { release(); return err; }

        // ---- per stem: inverse STFT of the spectral branch + the waveform branch ----
        const auto& o = impl->outSpec;
        for (int s = 0; s < numStems; ++s)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int t = 0; t < frames; ++t)
                    for (int f = 0; f < bins; ++f)
                    {
                        const int64_t base = t * o.stride[o.t] + s * o.stride[o.s] + f * o.stride[o.f];
                        osp[(size_t) (t * bins + f)] = cf (specOut[base + (2 * ch)     * o.stride[o.c]],
                                                         specOut[base + (2 * ch + 1) * o.stride[o.c]]);
                    }
                inverse (osp.data(), tmp.data());

                const float* wave = waveOut + ((size_t) s * 2 + (size_t) ch) * segment;   // [1, stem, ch, N]
                auto& a = acc[s][ch];
                for (int i = 0; i < len; ++i) a[(size_t) i] += (tmp[(size_t) i] + wave[i]) * weight[(size_t) i];
            }
        }
        for (int i = 0; i < len; ++i) accW[(size_t) i] += weight[(size_t) i];
        release();

        // ---- everything before the next chunk's start is final: denormalise and write it ----
        const int n = last ? len : stride;
        outBuf.setSize (2, n, false, false, true);
        for (int s = 0; s < numStems; ++s)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                float* dst = outBuf.getWritePointer (ch);
                const auto& a = acc[s][ch];
                for (int i = 0; i < n; ++i) dst[i] = a[(size_t) i] / accW[(size_t) i] * normStd + fmean;
            }
            if (! writers[(size_t) s]->writeFromAudioSampleBuffer (outBuf, 0, n)) return "Could not write the stem files";
        }

        if (! last)
        {
            for (auto& st : acc)
                for (auto& c : st)
                {
                    std::copy (c.begin() + stride, c.end(), c.begin());
                    std::fill (c.end() - stride, c.end(), 0.0f);
                }
            std::copy (accW.begin() + stride, accW.end(), accW.begin());
            std::fill (accW.end() - stride, accW.end(), 0.0f);
        }

        if (progress && ! progress ((float) (k + 1) / (float) offsets.size()))
            return "cancelled";
    }
    return {};
}
