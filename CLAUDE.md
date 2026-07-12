# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

DDSP-VST builds two audio plugins (VST3/AU/Standalone) — **DDSP Effect** and **DDSP Synth** — using the JUCE
framework, with pitch/loudness feature extraction and DDSP synthesis controls predicted by embedded TensorFlow
Lite models. Targets macOS and Windows only.

## Setup

Clone, then initialize submodules (JUCE, TensorFlow) and download the DDSP TFLite models:

```shell
./repo-init.sh
```

This runs `git submodule update --init` and `scripts/download-models.sh`. Re-run `scripts/download-models.sh`
alone to refresh `models/ddsp/*.tflite` (it deletes and re-downloads that directory).

## Build

CMake 3.15+ required, C++20.

**macOS** (Xcode project, recommended for dev/debug):
```shell
cmake -B build -S . -G Xcode
```
Open `DDSP.xcodeproj` and build a target. For release builds use Ninja instead — XNNPACK (used by TFLite) is not
supported when generating with Xcode:
```shell
cmake -B build-ninja -S . -G Ninja
cmake --build build-ninja
```
Built plugins are copied to `~/Library/Audio/Plug-Ins` automatically post-build on macOS.

**Windows** (Visual Studio 2022):
```shell
cmake -B build -G "Visual Studio 17 2022"
./scripts/remove-m-lib-win.sh   # removes m.lib dependency CMake incorrectly adds, run after generating build files
```
Plugin binaries must be copied manually to `C:\Program Files\Common Files\VST3`.

To add new source files to the build, edit `cmake/FileList.cmake` (`DDSP_SOURCES` / `DDSP_ASSETS` /
`DDSP_TEST_SOURCES`) — CMakeLists.txt itself rarely needs changes. Compiler/linker options and the project
version live in `cmake/Config.cmake`.

## Tests

Tests use GoogleTest (fetched via CMake `FetchContent`) and run through CTest via the `DDSPUnitTestRunner`
console app target, which links against the `DDSPEffect` plugin target itself.

```shell
cd build-ninja && ctest --output-on-failure -j4
```

Test sources are listed in `cmake/FileList.cmake` under `DDSP_TEST_SOURCES`; there is currently one end-to-end
test (`tests/InferencePipeline_Test.cpp`) that renders a WAV asset through the full plugin pipeline and writes
the result next to the input file for inspection. `pluginval` is additionally run in CI (`--strictness-level 10`)
against the built VST3s — not runnable as a plain unit test.

## Formatting

clang-format (v13, style defined in `.clang-format`) is enforced in CI on everything under `src/`.

```shell
./scripts/format-code.sh
```

Style follows the JUCE coding standard, with the Google C++ style guide's rules for constant names and
enumerator names as the deviation (see `CONTRIBUTING.md`).

## Architecture

### Plugin entry points

`DDSPAudioProcessor` (`src/PluginProcessor.*`) is the JUCE `AudioProcessor` implementation shared by both the
Effect and Synth targets (they differ only in JUCE plugin config in `CMakeLists.txt` — IS_SYNTH, MIDI in/out,
plugin codes — and are built from the same `DDSP_SOURCES`). It owns the `AudioProcessorValueTreeState` (param
state), a `ddsp::ModelLibrary`, a `ddsp::InferencePipeline`, and a `juce::Reverb`. `PluginEditor`/`ContentComponent`
and the `src/ui/*` components render the UI on top of it.

### Inference pipeline (`src/audio/tflite/InferencePipeline.*`)

This is the core signal path and the most important thing to understand before touching audio code:

1. Audio in (host sample rate) is resampled to 16 kHz (`kModelSampleRate_Hz`) via `WindowedSincInterpolator` and
   pushed into `inputRingBuffer` (`AudioRingBuffer`), a lock-free-ish SPSC FIFO.
2. `FeatureExtractionModel` extracts pitch (F0) and loudness features from fixed-size frames
   (`kModelFrameSize` = 1024 samples, hop `kModelHopSize` = 320).
3. `PredictControlsModel` (a stateful GRU-based TFLite model, `kGruModelStateSize` = 512) maps those features to
   synthesis controls: harmonic amplitude/distribution (`kHarmonicsSize` = 60) and noise filter magnitudes
   (`kNoiseAmpsSize` = 65). Model swapping (`currentPredictControlsModel` / `nextPredictControlsModel`, guarded
   by `swappingModel`) lets the instrument model change without audio dropouts.
4. `HarmonicSynthesizer` and `NoiseSynthesizer` (`src/audio/*`) synthesize audio from those controls (additive
   harmonic synthesis + filtered noise), writing into `synthesisBuffer`.
5. Output is resampled back to host sample rate and drained from `outputRingBuffer` in `getNextBlock`.
6. `MidiInputProcessor` converts incoming MIDI (Synth target) into pitch/gate control signals that override the
   extracted F0 when driving the synth.

Inference can run either on the audio thread or on a `juce::HighResolutionTimer` callback
(`hiResTimerCallback`), controlled by `kInferenceOnAudioThread` in `src/util/Constants.h` — this is a compile-time
switch, not runtime-configurable. Timer-driven inference runs at `kModelInferenceTimerCallbackInterval_ms` (20ms);
total pipeline latency is `kTotalInferenceLatency_ms` (64ms). Most magic numbers governing tensor shapes, model
I/O tensor names, and timing live in `src/util/Constants.h` — check there before hardcoding a value.

### Model loading (`ModelLibrary`, `src/audio/tflite/ModelLibrary.*`)

Instrument models are TFLite flatbuffers embedded as binary assets (see `DDSP_ASSETS` in
`cmake/FileList.cmake`, built into the `Assets` binary-data target by `juce_add_binary_data`) plus any
user-provided models found on disk (`searchPathForModels`/`getPathToUserModels`). Each is wrapped in a
`ModelInfo` (name + timestamp + raw `MemoryBlock`) and validated before being made selectable.

### Directory layout

- `src/audio/` — DSP and MIDI: ring buffer, synthesizers, MIDI-to-control conversion.
- `src/audio/tflite/` — TFLite model wrappers and the inference pipeline.
- `src/ui/` — JUCE UI components (top/bottom panels, sliders, look-and-feel, model range visualizer).
- `src/util/` — shared constants and small input-handling helpers.
- `externals/` — JUCE and TensorFlow git submodules (not vendored source, must be initialized).
- `models/` — embedded TFLite model files, populated by `scripts/download-models.sh` (not checked into git).
- `cmake/` — all CMake config: `Config.cmake` (project/version/compile settings), `FileList.cmake` (source/asset/
  test file lists), `Util.cmake` (helper functions e.g. `regroup_juce_target_sources`).

## CI

Two GitHub Actions workflows: `format.yaml` (clang-format check on `src/`) and `build_and_test.yaml` (build with
Ninja, run `ctest`, then run `pluginval` against both built VST3s at `--strictness-level 10`). Currently macOS-only
in the build matrix (Linux/Windows are commented out as TODO).
