# Handoff — read this first

Repo state (0.2.0): the Windows build compiles (MinGW), passes the Windows validator 47/47 and streams real WASAPI audio
under Wine. **Not yet run on a real Windows PC / MSVC.** What changed: `CHANGELOG.md`. What's open: `KNOWN_ISSUES.md`.

## Priority order
1. **Build on Windows with MSVC** (push → Actions → `windows-build`, or `BUILD_INSTRUCTIONS.md`). Expect it to be close; send back
   `build-log.txt` if red. Check CRT settings link cleanly (LNK2038 = mismatch).
2. **`asb_probe`** (`TEST_ON_WINDOWS.md`): tone on a virtual cable, `underruns`/`resyncs` should stay ~0 after the first second.
3. **Real test with Focusrite + FL Studio**: FL stays on Focusrite ASIO, input monitoring unaffected, record + monitor,
   project save/reload restores the device (now by ID: try unplugging/replugging a device between saves), try ASIO buffer 128 and 2048.
4. **OBS/Discord** via a virtual cable (VB-Cable etc.).
5. **Default-device following**: `IMMNotificationClient` in `StreamEngine_win.cpp` (KNOWN_ISSUES #4).
6. **Bypass decision** (KNOWN_ISSUES #13).
7. **Minimal UI**: raw Win32 `IPlugView` (combo, checkbox, trackbar, 2 labels, 10 Hz timer reading `engine.stats()`) or VSTGUI.
8. **Benchmark vs Voxengo Recorder** under identical conditions.
9. Installer, signing.

## Where things live
* RT path: `src/vst/Processor.cpp` (`process`, `passAndStream`, `applyParam`), `src/core/SpscFrameRing.h`.
  Rule: audio thread uses only `requestEnabled / requestDeviceIndex / setGain / push*`.
* Worker: `src/engine/StreamEngine_win.cpp::workerMain` (`kPollMs`), stub for CI: `StreamEngine_stub.cpp`.
* Latency policy: `targetFillFrames / resyncAboveFrames` in `StreamEngine.h`. Resampler: `src/core/DriftResampler.h`.
* State format: `src/vst/State.h` (v2, used by both Processor and Controller). Bump the version if you add fields.
* Windows check without Windows: `tools/cross_check_windows.sh`.

## Honest caveats
No Windows/Focusrite/FL/OBS/Discord testing yet; no Voxengo comparison. Wine is a different audio stack; its numbers are evidence, not Windows numbers.
