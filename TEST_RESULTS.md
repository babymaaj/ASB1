# Test results (what was ACTUALLY run)

## 0.2.0 — 2026-10-30 (Linux sandbox + MinGW cross-compiler + Wine)
Environment: GCC 13.3 (Linux), MinGW-w64 GCC 13.2 (Windows target), Wine 9 (`wine64`), PulseAudio null sinks as fake
output devices ("VirtualCable", "Speakers", 44.1 kHz). Still **no real Windows, MSVC, FL Studio, Focusrite, OBS, Discord or Voxengo.**

| Test | Linux build | Windows build (run in Wine) |
|---|---|---|
| `core_tests` (ring, 8 drift cases incl. 1024/2048 blocks with 0 resyncs, stall recovery, cubic HF, gain/fade, UTF-8) | PASS | PASS |
| `state_tests` (v2 round trip, v1 load, ID resolution, garbage rejected) | PASS | PASS |
| Steinberg validator on the plugin | 47/47 | **47/47 (Windows validator.exe on the Windows DLL)** |
| `processor_bench` (bit-exact passthrough 32/64-bit, separate + in-place; paced 20 s) | PASS | n/a (Linux-only) |
| `asb_probe` real WASAPI streaming, 48k → 44.1k device | n/a | **status=Streaming, 0 underruns, 0 overflow, fill settles at ~30 ms target** |

Key numbers:
* Before the resync fix, `asb_probe` under Wine showed the buffer stuck at ~97 % (~600 ms late) with 3.3 k frames dropped;
  the device consumed ~28 k frames/s for the first 2 s instead of 44.1 k. After: fill ≈ 0.03–0.04 (≈ target), drift settles.
  Wine's audio start is slow, so 5–11 startup resyncs were counted there; expect ~0 on real Windows.
* Interpolation HF @ worst position: 5 k −0.05 dB, 10 k −0.61 dB, 15 k −2.69 dB (linear was −0.47 / −2.01 / −5.11).
* Gain/fade max per-sample step on 0.5 DC: 0.0025 (an unsmoothed change would be 0.5).
* `process()` paced, live consumer: mean ≈1.3 µs/block @256; idle 5 s with stream OFF: 5.3 ms CPU (was 0.05 ms; 50 ms poll).
* Worker CPU under Wine: ~6 ms per second of streaming (~0.6 % of a core) — Wine number, not Windows.

## 0.1.0 — 2026-10-28 (OX session)

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
