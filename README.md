# ASIO Stream Bridge (alpha 0.3.0)

A small VST3 "audio splitter" for FL Studio. Put it on the **master bus**: audio passes through untouched
(bit-exact, zero added delay) while a copy is sent through a lock-free ring buffer to a worker thread that
plays it on any Windows output device (WASAPI shared mode) — e.g. a virtual cable feeding Discord/OBS.
FL Studio stays on Focusrite ASIO; nothing here touches the ASIO driver or Focusrite input monitoring.

**Status: alpha.** The Windows build has now been compiled (MinGW cross-compiler), and the Windows `.exe`/plugin DLL have been
run under Wine: Steinberg's Windows validator passes 47/47 and `asb_probe` streams real audio through WASAPI to virtual devices.
It also builds with MSVC and passes the extensive validator on real Windows in CI (GitHub Actions).
It has **still never run on a PC with real audio hardware, in FL Studio or with a Focusrite.** See `TEST_RESULTS.md` and `FRIEND_HANDOFF.md`. Changes: `CHANGELOG.md`.

## Layout
| Path | What |
|---|---|
| `src/core/SpscFrameRing.h` | Preallocated lock-free SPSC stereo ring (header-only) |
| `src/core/DriftResampler.h` | Cubic-interp pull resampler, drift controller, hard resync, gain smoothing (header-only) |
| `src/core/Utf8.h` | UTF-8 → UTF-16 for device names |
| `src/engine/StreamEngine.h` | Platform-neutral engine API (ring + worker lifecycle + stats) |
| `src/engine/StreamEngine_win.cpp` | **Windows** WASAPI event-driven renderer, device enumeration, reconnect |
| `src/engine/StreamEngine_stub.cpp` | Non-Windows stand-in (consumes + discards) so Linux CI/tests work |
| `src/vst/` | VST3 Processor, Controller (generic-UI parameters), factory entry, `State.h` (saved-state format) |
| `tests/core_tests.cpp` | Ring, resampler/drift, stall recovery, big-buffer, interpolation, gain, UTF-8 tests |
| `tests/state_tests.cpp` | Saved-state v1/v2 loading, device-by-ID resolution |
| `tools/cross_check_windows.sh`, `cmake/mingw-w64.cmake` | Build + test the Windows version from Linux (MinGW + Wine) |
| `tests/processor_bench.cpp` | Drives the Processor directly: passthrough exactness, per-block cost, worker CPU |
| `third_party/vst3sdk` | Steinberg VST3 SDK 3.8.1 (MIT) — git-ignored, clone it (see BUILD_INSTRUCTIONS.md) |

## Window & parameters
On Windows the plugin has its own window: Streaming on/off, Output device, Level, live status and buffer meter.

Streaming (on/off), Output Level (-60…+6 dB, applied to stream path only), Output Device (list; 0 = system default),
Buffer Health + Status (read-only meters). State (enable/level/device, saved by its Windows endpoint ID) is saved with the project; 0.1.0 projects still load.

## Docs
`ARCHITECTURE.md` · `BUILD_INSTRUCTIONS.md` · `TEST_RESULTS.md` · `KNOWN_ISSUES.md` · `FRIEND_HANDOFF.md`

## Licenses / dependencies
VST3 SDK 3.8.1, MIT (Steinberg Media Technologies). Windows system libs only: ole32, avrt, WASAPI/MMDevice. No other third-party code.
