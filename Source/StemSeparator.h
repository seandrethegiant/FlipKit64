#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <complex>
#include <functional>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Stem separation (drums / bass / other / vocals) with HTDemucs, run locally through ONNX Runtime.
//
// - The model (an MIT-licensed ONNX export of Meta's HTDemucs) is NOT bundled. It is downloaded
//   once on first use into <app data>/FlipKit64/stems/.
// - ONNX Runtime is loaded at run time (onnxruntime.dll / libonnxruntime.so) instead of being
//   linked, so FlipKit64 still builds, loads and runs without it; the feature just reports
//   "unavailable". This also keeps the MinGW cross-build free of MSVC import libraries.
// - The model only covers the neural network. The STFT / inverse STFT, normalisation and the
//   overlapping-chunk stitching are done here (StemDsp), following Demucs' own apply_model.
// - Stems are streamed to WAV writers chunk by chunk, so memory stays flat for long tracks.
//
// Everything here is blocking: call it from a background thread, never the message or audio thread.
// ---------------------------------------------------------------------------------------------

namespace StemDsp
{
    constexpr int    sampleRate = 44100;
    constexpr int    segment    = 343980;   // 7.8 s, the model's fixed input length
    constexpr int    nfft       = 4096;
    constexpr int    hop        = 1024;
    constexpr int    bins       = 2048;     // nfft/2, the Nyquist bin is dropped as in Demucs
    constexpr int    frames     = 336;      // ceil (segment / hop)

    // Demucs-style spectrogram of one channel. x holds exactly `segment` samples.
    // spec is frames x bins (frame-major), un-normalised apart from Demucs' 1/sqrt(nfft) scale.
    void forward (const float* x, std::vector<std::complex<float>>& spec);

    // Inverse of forward(): spec is frames x bins, out receives `segment` samples.
    void inverse (const std::complex<float>* spec, float* out);
}

class StemSeparator
{
public:
    enum Stem { Drums = 0, Bass, Other, Vocals, numStems };
    static const char* stemName (int stem);                  // "drums", "bass", "other", "vocals"

    // Return false from the callback to cancel. fraction is 0..1.
    using Progress = std::function<bool (float fraction)>;

    // ---- availability ----
    static juce::File stemsFolder();                          // <app data>/FlipKit64/stems
    static juce::File modelFile();
    static bool modelPresent();
    static bool runtimeAvailable();                           // ONNX Runtime library can be loaded
    static juce::String runtimeStatus();                      // human readable, for the UI

    // Fetches the model into stemsFolder(). Blocking. Returns an empty string on success, else an error.
    static juce::String downloadModel (Progress progress);

    // ---- separation ----
    // Loads the model and ONNX Runtime. Returns an empty string on success, else an error message.
    juce::String load (const juce::File& modelOverride = {});
    bool isLoaded() const { return impl != nullptr; }

    // input: any channel count / rate. One WAV writer per stem (44.1 kHz, 2 channels expected).
    // Returns an empty string on success, "cancelled", or an error message.
    juce::String separate (const juce::AudioBuffer<float>& input, double inputRate,
                           std::array<juce::AudioFormatWriter*, numStems> writers, Progress progress);

    StemSeparator();
    ~StemSeparator();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE (StemSeparator)
};
