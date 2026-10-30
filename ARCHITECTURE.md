# Architecture

```
Mic → Focusrite ASIO → FL Studio → Master bus → [ASIO Stream Bridge] → (FL output, unchanged)
                                                      │ Path B (copy)
                                                      ▼
                                       SPSC ring (32768 stereo frames, 256 KB)
                                                      ▼
                                   worker thread: DriftResampler → WASAPI shared, event-driven
                                                      ▼
                                   chosen Windows endpoint (e.g. virtual cable → Discord/OBS)
```

## Path A — passthrough (`Processor::passAndStream`)
`memcpy` input→output only if the host supplied different buffers; nothing else. No buffering, no DSP, no latency
reported (`getLatencySamples` is the default 0). Output silence flags are copied from input.

## Path B — streaming
Audio thread: `ring.writePlanar()` (float or double input → float interleaved). Wait-free: one relaxed load, one acquire load,
one release store. If the ring is full the excess is **dropped** and counted (`overflowFrames`) — the audio thread never waits.

## Threads & ownership
| Thread | Does | Must never |
|---|---|---|
| Host audio thread | `Processor::process`: param atomics, memcpy, `writePlanar`, meter output params every ~100 ms | alloc, lock, syscall, wait |
| Host main/control thread | `initialize/setActive/setState`, `StreamEngine::start/stop` (thread create/join) | — |
| Worker (`StreamEngine::workerMain`) | WASAPI open/close, `GetBuffer/ReleaseBuffer`, resampling, stats atomics, reconnect backoff | touch VST host objects |

Controller (`EditController`) enumerates devices once at `initialize` on the main thread (COM init/uninit local to that call).

## Synchronization
* Ring: head/tail `uint64` monotonic counters, each on its own cache line; producer = audio thread, consumer = worker. Release/acquire pairs.
  `discardAll()` is consumer-side only (moves tail to head).
* Config (enable, device index, host rate, block size, gain): `std::atomic`. Audio-thread `requestX()` only stores + bumps
  `configGen_` (no syscall); the worker polls every `kPollMs` (50 ms) when idle and every device period while streaming.
  Control-thread `setX()` additionally signals the worker (`SetEvent` / condvar) for an immediate response.
* Devices: processor and controller each enumerate once at `initialize` (same sorted order, same 32 cap). The processor hands
  its list to the engine (`setDevices`, immutable while the worker runs); the worker resolves index → endpoint ID from it.
  Saved state stores the endpoint ID (`src/vst/State.h`).
* Stats: independent atomics written by the worker, read by the audio thread for meters.

## Worker loop (Windows)
1. Disabled → `WaitForSingleObject(wakeEvt, 50 ms)` poll (≈zero CPU). `setActive(false)` joins the thread entirely.
2. Enabled and no device → open (`IMMDevice` by sorted index; 0 = default), shared mode, `EVENTCALLBACK | AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`,
   requested format float32 stereo at the endpoint's mix rate (Windows does channel/format conversion). Silence-prefill, `Start()`.
3. `WaitForMultipleObjects(renderEvt, wakeEvt, 2 s)`; render event → `GetCurrentPadding` → `GetBuffer` → `DriftResampler.render` straight into the
   device buffer → `ReleaseBuffer`. Any HRESULT failure / 2 s stall → close, status=Error, retry every 1 s (covers unplug, default-device change by
   reopen on next config bump, exclusive-mode steals).
4. MMCSS "Pro Audio" on the worker.

## Buffering / latency / drift
Target ring fill = max(30 ms, 2 host blocks) (host-rate frames) = steady-state streaming latency, plus the WASAPI shared
period (~10 ms, device dependent). `DriftResampler` interpolates host rate → endpoint rate with a 4-point cubic
(Catmull-Rom) and nudges its step by a smoothed P-controller on ring fill (±0.5 % authority) so crystal drift between the
ASIO clock and the output device cannot starve or overflow the ring.
* Underrun → silence, re-prime to target, fade back in.
* Fill > 3× target + 1 block (device stalled / slow start) → **hard resync**: drop oldest audio down to target. Drift control
  alone would need minutes to drain such a backlog.
* Gain is a one-pole ramp (~4 ms) applied per output sample.

## Why cubic, not a real SRC?
Cheap (a few multiplies per sample) and good enough for a stream feed: worst-case loss −0.6 dB @10 kHz (linear was −2 dB).
There is no anti-alias filter, so 48k → 44.1k can alias a little above ~20 kHz. Run the output device at the same rate as FL
(usually 48 kHz) for the cleanest result.
