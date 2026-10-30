# Known issues / missing features

1. **Windows engine never compiled.** Expect compile errors/warnings in `StreamEngine_win.cpp` (headers: `initguid.h`/`ksmedia.h` ordering, `ComPtr` helper).
2. **No custom UI.** Only host-generic parameter UI (device list, enable, level, meters). Spec wants device select, ON/OFF, level, buffer health, status — all exist as parameters, not as a dedicated window.
3. **`SetEvent`/condvar signalled from audio thread on parameter changes** (via `engine_.setX()` inside `process()`). It is rare (only on changes) but is a syscall on the RT thread, which violates the stated rule. Fix: set atomics only in `process()`, let the worker poll a generation counter on a timer (e.g. 100 ms wait timeout) or have a non-RT message thread apply changes.
4. **Device selection by sorted index** is racy and unstable if devices appear/disappear; controller and worker enumerate independently. Should persist endpoint ID string in state and send it to the engine from a non-RT thread.
5. Device list only refreshed when the plugin instance is created.
6. Device names converted ASCII-only in controller (`UString::fromAscii`); use UTF-16 → `String128`.
7. Default-device following: "System default" is resolved at open time only; a default-device change is not detected (needs `IMMNotificationClient`).
8. Resampler is linear interpolation, always active even when rates are equal (fast bit-exact path missing). No anti-alias filtering for 48→44.1.
9. Streaming latency target (30 ms) hard-coded; not user adjustable. Shared-mode period not minimized (could use `IAudioClient3` low-latency shared).
10. Stream gain applies as a hard step (no smoothing) → possible zipper noise.
11. `getLatencySamples` is 0 (correct for passthrough). Verify FL doesn't treat the plugin specially when streaming is on.
12. When the project is closed/plugin deactivated the worker thread is joined in `setActive(false)` on the main thread (≤5 s wait worst case).
13. Overflow policy drops the *newest* audio (ring full); acceptable but not tuned.
14. Mono / surround buses unsupported (stereo only).
15. No installer, no code signing.
16. `StreamStats.workerCpuMs` is a diagnostic; the "buffer health" meter maps fill×2 clipped to 1 (so 50 % ring = full bar) — arbitrary.
