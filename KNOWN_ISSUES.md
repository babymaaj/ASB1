# Known issues / missing features

Fixed in 0.2.0 (see CHANGELOG.md): old #1 compile errors (MinGW-verified; MSVC still unverified), #3 RT-thread syscalls,
#4 device by index, #6 ASCII-only names, #8 linear interpolation (now cubic), #10 gain zipper. Plus two new bugs found
and fixed: stuck high latency after a device stall, and underruns with big ASIO buffers.

Still open:
1. **Never run on real Windows / MSVC.** MinGW + Wine are good evidence but not proof. MSVC `/W4 /permissive-` may still warn.
2. **No custom UI.** Host-generic parameters only.
3. Device list only refreshed when the plugin instance is created (reopen the plugin to see a newly plugged device).
4. "System default" is resolved when the device opens; a default-device change while streaming is not followed
   (needs `IMMNotificationClient`).
5. No anti-alias filter for 48k → 44.1k (cubic, not a proper SRC). Fine for voice/streams; avoid 44.1k devices for critical listening.
6. Streaming latency target (max(30 ms, 2 blocks)) not user adjustable. Shared-mode period not minimized (`IAudioClient3`).
7. While stalled-then-resynced, the dropped audio is gone (by design: catching up beats staying 0.5 s late).
8. Idle worker wakes 20×/s to pick up audio-thread requests (~0.1 % of a core on Linux; expect less on Windows).
9. `getLatencySamples` is 0 (correct for passthrough). Verify FL doesn't treat the plugin specially when streaming is on.
10. `setActive(false)` joins the worker on the main thread (≤5 s worst case).
11. Ring overflow drops the newest audio (rare now that resync exists).
12. Mono / surround buses unsupported (stereo only).
13. No bypass parameter: when FL bypasses the plugin, it is unspecified whether the stream continues. Decide and add `kIsBypass`.
14. No installer, no code signing.
15. Device list capped at 32 entries.
