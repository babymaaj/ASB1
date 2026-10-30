# ASIO Stream Bridge (alpha 0.1.0)

A small VST3 "audio splitter" for FL Studio. Put it on the **master bus**: audio passes through untouched
(bit-exact, zero added delay) while a copy is sent through a lock-free ring buffer to a worker thread that
plays it on any Windows output device (WASAPI shared mode) — e.g. a virtual cable feeding Discord/OBS.
FL Studio stays on Focusrite ASIO; nothing here touches the ASIO driver or Focusrite input monitoring.

**Status: alpha, authored in a Linux sandbox. The Windows code (WASAPI engine) has NEVER been compiled or run.**
Everything that could be verified on Linux was; see `TEST_RESULTS.md` for exactly what, and `FRIEND_HANDOFF.md` for next steps.

## Layout
| Path | What |
|---|---|
| `src/core/SpscFrameRing.h` | Preallocated lock-free SPSC stereo ring (header-only) |
| `src/core/DriftResampler.h` | Linear-interp pull resampler + fill-level drift controller (header-only) |
| `src/engine/StreamEngine.h` | Platform-neutral engine API (ring + worker lifecycle + stats) |
| `src/engine/StreamEngine_win.cpp` | **Windows** WASAPI event-driven renderer, device enumeration, reconnect |
| `src/engine/StreamEngine_stub.cpp` | Non-Windows stand-in (consumes + discards) so Linux CI/tests work |
| `src/vst/` | VST3 Processor, Controller (generic-UI parameters), factory entry |
| `tests/core_tests.cpp` | Ring + resampler/drift tests |
| `tests/processor_bench.cpp` | Drives the Processor directly: passthrough exactness, per-block cost, worker CPU |
| `third_party/vst3sdk` | Steinberg VST3 SDK 3.8.1 (MIT) — git-ignored, clone it (see BUILD_INSTRUCTIONS.md) |

## Parameters (host generic UI; no custom editor yet)
Streaming (on/off), Output Level (-60…+6 dB, applied to stream path only), Output Device (list; 0 = system default),
Buffer Health + Status (read-only meters). State (enable/level/device index) is saved with the project.

## Docs
`ARCHITECTURE.md` · `BUILD_INSTRUCTIONS.md` · `TEST_RESULTS.md` · `KNOWN_ISSUES.md` · `FRIEND_HANDOFF.md`

## Licenses / dependencies
VST3 SDK 3.8.1, MIT (Steinberg Media Technologies). Windows system libs only: ole32, avrt, WASAPI/MMDevice. No other third-party code.
