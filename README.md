# FlipKit64

**A free 64-pad drum sampler, slicer and step sequencer (VST3 + standalone, Windows).**
By seandrethegiant · Version 0.9 beta

FlipKit64 is a pad instrument in the spirit of classic sampling drum machines: drop your samples onto an 8×8 grid of pads, chop loops and records into slices, shape each pad, and sequence it all in sync with your DAW. It works with any MIDI pad controller or keyboard, and the grid follows the common 64-pad "drum rack" note layout (notes 36–99) used by 8×8 pad controllers such as the Synido TempoPAD.

FlipKit64 is free. If it's useful to you, you can support it on [Ko-fi](https://ko-fi.com/seandrethegiant).

Source code and downloads: https://github.com/seandrethegiant/FlipKit64

> **Beta:** it has been tested by its author in UA LUNA on Windows. Please report anything odd.

## Features

**Pads**
- 64 pads; drag samples from the built-in browser or Windows Explorer onto a pad
- Several samples per pad: round-robin, velocity layers or random
- Per pad: level, pan, tune/fine, start/end, attack/decay, low-pass filter with resonance, reverse, mono, choke groups (1–8)
- Click a pad to hear it (lower on the pad = softer)

**Sampler**
- Chop by transients, equal slices or by hand, then assign slices to pads in pad order
- Tap chop: play the sample and hit pads to drop markers, or to chop each slice straight onto the pad you hit
- Waveform zoom and scroll; loads samples up to 12 minutes
- Resample FlipKit64's own output (or an audio input, if your DAW provides one); recordings are saved as WAV to `Documents\FlipKit64\Recordings`

**Sequencer**
- 8 patterns, up to 64 steps, 1/8 to 1/32 and triplets
- Per-step velocity and ratchets (2–4 hits per step, for rolls)
- Swing, live recording from your pads (quantized), pattern switching at the bar line
- Follows your DAW's transport, or runs on its own

**Browser**
- Saved locations for your sample folders and music library ("Add a folder...")
- Formats: WAV, AIFF, FLAC, OGG, MP3

## Install (Windows)

1. Download and unzip the release.
2. Double-click `Install FlipKit64.bat` (Windows asks for admin permission because plugins go into `C:\Program Files\Common Files\VST3`). Or copy the `FlipKit64.vst3` folder there yourself.
3. Restart your DAW and add FlipKit64 as an instrument.

`FlipKit64.exe` is the standalone version (no DAW needed; choose your audio and MIDI devices under Options).

Windows may warn that the installer/app is from an unknown publisher, because it isn't code-signed. Choose "More info" → "Run anyway" if you trust the download.

## Quick start

1. In **PADS**, pick a folder in the browser and drag a few drum hits onto pads.
2. Play them from your controller (notes 36–99) or click them.
3. In **SEQUENCER**, click steps to make a beat; press play in your DAW (or the Play button).
4. In **SAMPLER**, load a loop, press **Chop**, select a starting pad and press **Assign slices to pads**.

Kits and patterns are saved with your DAW project. **Save kit / Load kit** saves `.flipkit` files to reuse elsewhere (kits reference your sample files, so keep them where they are).

## Known limitations

- Windows only for now (VST3 and standalone).
- No time-stretching or built-in effects yet.
- M4A/AAC files aren't supported.
- Tracks that are set to receive "All" MIDI inputs may also hear other controllers' knobs and faders; set the track's MIDI input to your pad controller if that happens.

## Building from source

FlipKit64 is built with [JUCE](https://juce.com) 7 (tested with 7.0.12) and CMake.

```
git clone --branch 7.0.12 https://github.com/juce-framework/JUCE.git
git clone https://github.com/seandrethegiant/FlipKit64.git
cmake -S FlipKit64 -B build -DJUCE_DIR=JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The release binaries are cross-compiled from Linux with MinGW-w64 (`mingw.cmake` toolchain file included). `-DPADROMPLER_TESTS=ON` builds the offline audio test suite (`tests/test.cpp`).

## License

FlipKit64 is free software, released under the **GNU General Public License v3 or later** (see `LICENSE`). It uses the JUCE framework under its GPLv3 option, and the VST3 SDK (MIT license). VST is a registered trademark of Steinberg Media Technologies GmbH.

This program comes with ABSOLUTELY NO WARRANTY.
