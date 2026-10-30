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
* Config (enable, device index, host rate, gain): `std::atomic`; each setter bumps `configGen_` and signals the worker
  (`SetEvent` on Windows, condvar on the stub). Signalling is done from the audio thread on a parameter change — `SetEvent` is a
  syscall; it only happens on parameter *changes*, not per block (see KNOWN_ISSUES #3 for the cleaner alternative).
* Stats: independent atomics written by the worker, read by the audio thread for meters.

## Worker loop (Windows)
1. Disabled → `WaitForSingleObject(wakeEvt, INFINITE)` (zero CPU). `setActive(false)` joins the thread entirely.
2. Enabled and no device → open (`IMMDevice` by sorted index; 0 = default), shared mode, `EVENTCALLBACK | AUTOCONVERTSRCFORMAT | SRC_DEFAULT_QUALITY`,
   requested format float32 stereo at the endpoint's mix rate (Windows does channel/format conversion). Silence-prefill, `Start()`.
3. `WaitForMultipleObjects(renderEvt, wakeEvt, 2 s)`; render event → `GetCurrentPadding` → `GetBuffer` → `DriftResampler.render` straight into the
   device buffer → `ReleaseBuffer`. Any HRESULT failure / 2 s stall → close, status=Error, retry every 1 s (covers unplug, default-device change by
   reopen on next config bump, exclusive-mode steals).
4. MMCSS "Pro Audio" on the worker.

## Buffering / latency / drift
Target ring fill = 30 ms (host-rate frames) = steady-state streaming latency, plus the WASAPI shared period (~10 ms, device dependent).
`DriftResampler` linearly interpolates (host rate → endpoint rate) and nudges its step by a smoothed P-controller on ring fill (±0.5 % authority)
so crystal drift between the ASIO clock and the output device cannot starve or overflow the ring. Underrun → silence + re-prime to target.

## Why linear interpolation?
Cheapest thing that works; images/aliasing are acceptable for a voice-chat/stream feed when rates differ (44.1↔48 k). When host rate == device rate
(the common case: both 48 k) the step is ≈1.0 and interpolation is nearly an identity (a tiny fractional phase still blends adjacent samples — a better
"exact bypass when ratio==1 and drift≈0" fast path is a handoff item).
