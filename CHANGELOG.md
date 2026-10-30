# Changelog

## 0.5.0-alpha (2026-10-30)
* **Installer: `ASB-Setup-0.5.0.exe`** (by maai). Installs `ASB.vst3` into the standard VST3 folder, removes the old
  `ASIOStreamBridge.vst3`, offers to close FL if the plugin is in use, adds an uninstaller (Windows Apps), and links to
  VB-CABLE if it isn't installed yet.
* **Plugin is now `ASB.vst3`** with the ASB icon and Windows file properties (product ASB, company maai).
* **Settings page** (slider icon, top right):
  * **Latency** 10 / 20 / 30 / 60 / 120 ms (saved per project). Lower = tighter, higher = safer on a busy PC.
  * **Channels** Stereo / Mono / Swap / Left / Right (saved per project). The SIGNAL meter shows the result.
  * **Accent** colour: green, white, red, blue, violet, amber (your preference, all projects).
  * **Size** 100 / 125 / 150 / 200 % — redrawn at full resolution, not stretched (your preference).
  * **Updates**: checks GitHub Releases at most once a day when the window opens (can be set to manual);
    an UPDATE tag appears in the header when a newer version exists.
* Preferences live in `%APPDATA%\maai\ASB\settings.ini`.
* START STREAM / STREAMING label is now exactly centred (letter-spacing was not accounted for).
* Releases: pushing a tag `vX.Y.Z` builds, (optionally) signs, and publishes the installer on GitHub Releases.
* Fixed: a pending update-check thread could terminate the host when the plugin was unloaded.


## 0.4.0-alpha (2026-10-30)
* **Renamed: "ASB" by maai** (vendor maai, maai.lol). Same plugin ID, so existing projects still load it.
  (The bundle is still `ASIOStreamBridge.vst3`; replace the old folder when updating.)
* **New window design**, industrial and quiet: fine perforated panel, custom "ASB" wordmark built from rounded strokes
  and cut into the panel (white S face), stamped/engraved caps labels (Bahnschrift), milled divider lines,
  LED-dot SIGNAL meter (L/R, green/amber/red, peak-hold LED + readout), flat level fader (drag, shift-drag fine, wheel,
  double-click = 0 dB), device list, START STREAM / STREAMING button.
* **Peak metering** in the engine (`meterPlanar`, ~0.1 µs per 256-sample block on the audio thread).
* CPU: everything static (and the "all lit" LED layer) is rendered once per window open; a frame is a few blits plus
  plain GDI text (~0.7 ms under Wine; an earlier all-GDI+ draft cost ~23 ms). 30 Hz timer only while open, and frames
  are drawn only when something visible changed.
* CI pushes real-Windows screenshots of the window to the `ci-screenshots` branch.

## 0.3.0-alpha (2026-10-30)
* **Plugin window** (Windows, native Win32, no UI framework): Streaming on/off button, Output device dropdown (Unicode names),
  Level slider with dB readout (automation-friendly begin/perform/end edits), live status (device rate, ms buffered),
  buffer meter, dropout/resync counters. Follows automation and project loads (10 Hz refresh). Dark dropdown on Windows 10+.
* Live stats come straight from the engine via a processor→controller message (same-process only; otherwise falls back
  to the read-only Status/Buffer Health parameters).
* `StreamStats.latencyMs` (audio currently buffered).
* `editor_preview.exe` (Windows): opens the plugin window without a DAW, streaming a test tone; `--stream` to start on.

## 0.2.0-alpha (2026-10-30, Maaj session)
Built on OX's 0.1.0. Everything below is covered by tests; see TEST_RESULTS.md.

**Windows build fixes (found by actually compiling it)**
* `AUDCLNT_STREAMFLAGS_AUTOCONVERTSRCFORMAT` does not exist → `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`. Would have failed MSVC too.
* `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` link error (initguid/ksmedia include order) → GUID defined locally.
* Windows libs (ole32, avrt, uuid) linked from CMake instead of MSVC-only `#pragma comment`.

**Found by the first real-Windows MSVC build (GitHub Actions, Visual Studio 2026)**
* **Crash fixed:** `enumerateRenderDevices()` released the device enumerator *after* `CoUninitialize` → access violation
  whenever the plugin is created on a thread without COM (Steinberg validator; very likely FL's plugin scanner). Wine hid it.
* `functiondiscoverykeys_devpkey.h` needs `propkeydef.h` first under MSVC.
* MSVC now compiles sources as UTF-8 (`/utf-8`); Release builds emit PDB symbols for readable crash stacks.
* CI: works on `windows-latest` (VS 2026; the hard-coded VS 2022 generator broke it), runs the extensive validator,
  asb_probe and tests as separate steps, captures a cdb stack on crash, and always uploads logs.

**Streaming fixes**
* **Latency could get stuck at ~600 ms.** If the output device starts slowly or stalls (device switch, USB glitch, sleep),
  the backlog never drained (drift control only moves ±0.5 %, i.e. minutes). Added hard resync: above 3× target fill the
  oldest audio is dropped back to target. Counted in `StreamStats.resyncs`. Seen and fixed under real WASAPI (Wine).
* **Big ASIO buffers would underrun.** Target latency is now max(30 ms, 2 host blocks); FL at 1024/2048 samples is covered.
  `StreamEngine::setHostBlockSize`, fed from `setupProcessing.maxSamplesPerBlock`.
* Linear → 4-point cubic interpolation: worst-case HF loss 2.0 → 0.6 dB @10 kHz, 5.1 → 2.7 dB @15 kHz.
* Output level is smoothed (~4 ms) and audio fades in after every (re)start/underrun → no zipper noise or clicks.

**Real-time safety (KNOWN_ISSUES #3)**
* The audio thread no longer makes syscalls. `process()` uses `requestEnabled/requestDeviceIndex/setGain` (atomics only);
  the worker polls every 50 ms when idle. Control-thread setters (`setEnabled` etc.) still wake it immediately.

**Devices (KNOWN_ISSUES #4, #6)**
* Saved state v2 stores the device's Windows endpoint ID; on load the ID wins, so plugging in a new device no longer
  changes which device a project uses. If the saved device is gone, it falls back to System default (not a random neighbour).
  v1 (0.1.0) projects still load.
* Processor and controller share one device-list snapshot per instance (no more independent enumerations racing).
* Device names are proper Unicode (accents, CJK, emoji). Device and Streaming params round-trip their text (validator infos gone).

**Tooling**
* `tools/cross_check_windows.sh` + `cmake/mingw-w64.cmake`: compile and test the Windows build from Linux.
* `ASB_RUN_VALIDATOR` option (default ON) so cross-builds can skip the post-link validator.
* New `tests/state_tests.cpp`; `asb_probe` prints resyncs.
