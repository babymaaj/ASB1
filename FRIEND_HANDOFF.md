# Handoff — read this first

Repo state: portable core + VST3 shell verified on Linux; **Windows engine is written but unbuilt.** Start by getting it to compile.

## Priority order
1. **Build on Windows** (`BUILD_INSTRUCTIONS.md`). Fix compile errors in `src/engine/StreamEngine_win.cpp`. Confirm the validator passes (it did 47/47 on Linux).
   Check CRT settings in top-level `CMakeLists.txt` (`SMTG_USE_STATIC_CRT`, `CMAKE_MSVC_RUNTIME_LIBRARY`) link cleanly (LNK2038 would mean a mismatch).
2. **Smoke test WASAPI output**: enable Streaming, pick a device, confirm audio, then watch status/health. Functions: `StreamEngine::workerMain`, lambda `openDevice`.
3. **Remove RT-thread syscalls** (KNOWN_ISSUES #3): `Processor::applyParam` → `StreamEngine::setX/bump`. Make setters atomic-only; worker polls.
4. **Device identity**: persist endpoint ID (state v2 in `Processor::getState/setState`, `Controller::setComponentState`), pass to engine from non-RT context; add `IMMNotificationClient` for default-device change/removal (`StreamEngine_win.cpp`).
5. **Real test with Focusrite + FL Studio**: confirm FL stays on Focusrite ASIO, input monitoring unaffected, record + monitor, bypass/mute behaviour, project reload restoring state.
6. **Verify OBS/Discord** via a virtual cable (VB-Cable etc.): OBS audio-output capture or cable input; Discord input = cable output.
7. **Drift/resampler quality**: `src/core/DriftResampler.h` — add exact-bypass fast path when ratio≈1, better filter for 44.1/48, smooth gain, consider `IAudioClient3`.
8. **Minimal UI**: either VSTGUI (`SMTG_ENABLE_VSTGUI_SUPPORT`, heavy) or a raw Win32 `IPlugView` (combo, checkbox, trackbar, 2 labels, 10 Hz timer reading `engine.stats()`). Stats API is ready (`StreamEngine::stats`).
9. **Benchmark vs Voxengo Recorder** under identical conditions (same project, buffer size, 48 kHz): ETW/Process Explorer CPU of FL + `workerCpuMs`, working set, underrun counters, loopback-cable latency measurement. `tests/processor_bench.cpp` is a starting point (Linux-only timing code; port `rssKb/procCpuMs`).
10. Installer (Inno Setup/WiX), signing, docs of limitations.

## Where things live
* RT path: `src/vst/Processor.cpp` (`process`, `passAndStream`), `src/core/SpscFrameRing.h`.
* Worker: `src/engine/StreamEngine_win.cpp::workerMain`; stub equivalent for CI: `StreamEngine_stub.cpp`.
* Params/IDs/dB mapping: `src/vst/Ids.h`. Device param (fixed 32 steps, mapping must match both sides): `Controller.cpp::DeviceParam`, `Processor::applyParam`.
* Don't rewrite the ring/resampler without a measurable reason — they have tests (`tests/core_tests.cpp`).

## Honest caveats
No Windows/Focusrite/FL/OBS/Discord testing was done; no Voxengo comparison exists. All performance numbers in `TEST_RESULTS.md` are Linux stub-sink numbers.
