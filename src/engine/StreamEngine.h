// StreamEngine: owns the ring buffer, the worker thread and the output device.
// Threading contract
//   audio thread  : pushPlanar()/pushSilence() only (lock-free, wait-free)
//   audio thread  : also requestX()/setGain() -- atomics only, no syscalls
//   control thread: everything else (start/stop/setX). setX = atomics + wake the worker.
//   worker thread : device open/close, resampling, WASAPI rendering. Never touches VST objects.
#pragma once
#include "core/SpscFrameRing.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace asb {

struct DeviceInfo {
    std::string id;           // WASAPI endpoint id (UTF-8); empty = system default
    std::string name;         // friendly name
};

// Deterministic order shared by controller + engine: [0]=system default, then by id.
std::vector<DeviceInfo> enumerateRenderDevices();

// Streaming latency policy, shared by the engines and tests.
// Target ring fill = max(30 ms, 2 host blocks): a big ASIO buffer (1024/2048) delivers
// audio in large bursts, so the cushion must cover at least two of them.
inline size_t targetFillFrames(double hostRate, int hostBlock)
{
    const size_t ms30 = static_cast<size_t>(hostRate * 0.03);
    const size_t blk2 = static_cast<size_t>(hostBlock > 0 ? hostBlock : 0) * 2;
    return ms30 > blk2 ? ms30 : blk2;
}
// Above this fill the resampler hard-resyncs to target (see DriftResampler).
inline size_t resyncAboveFrames(double hostRate, int hostBlock)
{
    return targetFillFrames(hostRate, hostBlock) * 3 + static_cast<size_t>(hostBlock > 0 ? hostBlock : 0);
}

enum class StreamStatus : int { Off = 0, Connecting = 1, Streaming = 2, Error = 3 };

struct StreamStats {
    StreamStatus status = StreamStatus::Off;
    float    bufferFill = 0.f;     // 0..1 of ring capacity
    uint64_t underruns = 0;
    uint64_t overflowFrames = 0;
    double   deviceRate = 0.0;
    double   driftPpm = 0.0;
    float    workerCpuMs = 0.f;    // cumulative worker CPU ms (thread time), for benchmarking
    uint64_t deviceFrames = 0;     // total frames handed to the output device (diagnostic)
    uint64_t resyncs = 0;          // times the stream jumped back to target latency after a stall
};

class StreamEngine {
public:
    StreamEngine();
    ~StreamEngine();
    StreamEngine(const StreamEngine&) = delete;
    StreamEngine& operator=(const StreamEngine&) = delete;

    // ---- audio thread ----
    template <class T> void pushPlanar(const T* l, const T* r, size_t n) noexcept { ring_.writePlanar(l, r, n); }
    void pushSilence(size_t n) noexcept { ring_.writeSilence(n); }

    // ---- audio thread: atomics only, never a syscall. The worker notices within ~50 ms. ----
    void requestEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); configGen_.fetch_add(1, std::memory_order_release); }
    void requestDeviceIndex(int idx) noexcept { deviceIndex_.store(idx, std::memory_order_relaxed); configGen_.fetch_add(1, std::memory_order_release); }
    void setGain(float g) noexcept { gain_.store(g, std::memory_order_relaxed); }

    // ---- control thread (non-RT): same as above but also wakes the worker immediately ----
    void setHostSampleRate(double sr) noexcept { hostRate_.store(sr, std::memory_order_relaxed); bump(); }
    void setHostBlockSize(int frames) noexcept { hostBlock_.store(frames, std::memory_order_relaxed); bump(); }
    void setEnabled(bool on) noexcept { requestEnabled(on); bump(); }
    void setDeviceIndex(int idx) noexcept { requestDeviceIndex(idx); bump(); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // Device list snapshot the worker resolves indices against (same order the controller shows).
    // Control thread only, and only while the worker is stopped. If never set, the worker enumerates itself.
    void setDevices(std::vector<DeviceInfo> d) { if (!running()) devices_ = std::move(d); }
    const std::vector<DeviceInfo>& devices() const noexcept { return devices_; }
    // Index of an endpoint id in the snapshot; -1 if absent. "" (system default) -> 0.
    int indexOfDeviceId(const std::string& id) const noexcept;

    void start();   // create worker thread (idempotent). Worker sleeps when disabled.
    void stop();    // join worker thread (idempotent).
    bool running() const noexcept;

    StreamStats stats() const noexcept;

private:
    void bump() noexcept;          // wake worker to re-evaluate config (control thread only)
    std::string resolveDeviceId(int idx);  // worker: index -> endpoint id via snapshot
    void workerMain();
    struct Impl;                   // platform specific (WASAPI / stub)
    std::unique_ptr<Impl> impl_;

    SpscFrameRing ring_{32768};    // ~680 ms @ 48k, 256 KB, allocated once
    std::atomic<bool>   enabled_{false};
    std::atomic<int>    deviceIndex_{0};
    std::vector<DeviceInfo> devices_;  // immutable while the worker runs
    std::atomic<double> hostRate_{48000.0};
    std::atomic<int>    hostBlock_{512};
    std::atomic<float>  gain_{1.0f};
    std::atomic<uint32_t> configGen_{0};

    std::atomic<int>      status_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<double>   devRate_{0.0};
    std::atomic<double>   driftPpm_{0.0};
    std::atomic<float>    fill_{0.f};
    std::atomic<float>    cpuMs_{0.f};
    std::atomic<uint64_t> devFrames_{0};
    std::atomic<uint64_t> resyncs_{0};
    friend struct Impl;
};

} // namespace asb
