# Test results (what was ACTUALLY run)

**Environment:** Linux x86_64 cloud sandbox, GCC 13.3, CMake 3.28, Ninja, VST3 SDK 3.8.1. No Windows, no MSVC, no MinGW, no audio hardware,
**no Focusrite, no FL Studio, no Voxengo Recorder, no Discord/OBS.** Date: 2026-10-28.

## Run and passed
| Test | Result |
|---|---|
| `core_tests`: ring wrap/overflow accounting, 2 M-frame two-thread order/integrity | PASS |
| `core_tests`: resampler 48→48, 44.1→48, 48→44.1, ±300/500 ppm host-clock offset, 120 s simulated each: 0 underruns, 0 overflows, ring fill bounded (≈1.0–1.6 k frames around 1.44 k target), max sample-step 0.065–0.071 on a 0.5-amp 1 kHz sine (no clicks) | PASS |
| Steinberg **validator** on the Linux build of the plugin (`ASIOStreamBridge.vst3`, x86_64-linux) | **47 passed, 0 failed** (incl. 44.1k–384k, 32/64-bit processing, state, param automation, bus arrangements) |
| `processor_bench`: stereo passthrough bit-exact, 32-bit and 64-bit, stream OFF/ON, separate and in-place buffers | PASS |

## Measurements (Linux, stub sink — NOT Windows/WASAPI numbers)
* `Processor::process`, 256 frames, with a live consumer (real ring writes), 20 s real-time paced: **mean ≈1.4 µs/block** (≈0.03 % of the 5.33 ms @48k budget); worst single call seen 158 µs (OS scheduling noise, ≈3 % of budget) — one sample, not a distribution.
* Tight-loop figure (~100 ns) is optimistic: the ring was full so writes were dropped. Ignore it.
* Whole-process CPU in the paced run 0.96 % of a core, of which the stub worker 0.35 % (10 ms sleep-poll, not representative of WASAPI event mode) and the rest bench-harness timer wakeups.
* Idle (stream OFF): process CPU delta 0.049 ms over 5 s (worker blocked).
* RSS delta over 20 s: +48 kB. Ring is 256 kB allocated once.
* In the paced run: 1 underrun (initial prime), 0 overflow frames, fill stable, drift estimate 9–14 ppm.

## NOT tested (no way to here) — do not assume these work
* Any Windows compile (MSVC) of anything, incl. `StreamEngine_win.cpp`, and the static-CRT CMake settings.
* Real WASAPI output, device enumeration, switching, unplug/replug, default-device change, exclusive-mode conflicts.
* FL Studio loading the plugin, project save/load, automation, bypass/mute behaviour on the master bus, plugin delay compensation.
* Focusrite ASIO coexistence, input monitoring, any end-to-end latency measurement.
* OBS / Discord capture, virtual-cable routing.
* Benchmark vs Voxengo Recorder. **No claim is made that this is lighter than Voxengo.**
* The 0.5 % CPU / 15 MB / 20–60 ms targets on Windows.
* Non-ASCII device names (controller converts ASCII-only).
