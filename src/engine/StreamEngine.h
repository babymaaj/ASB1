// StreamEngine: owns the ring buffer, the worker thread and the output device.
// Threading contract
//   audio thread  : pushPlanar()/pushSilence() only (lock-free, wait-free)
//   control thread: everything else (start/stop/setters). Setters only touch atomics.
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

enum class StreamStatus : int { Off = 0, Connecting = 1, Streaming = 2, Error = 3 };

struct StreamStats {
    StreamStatus status = StreamStatus::Off;
    float    bufferFill = 0.f;     // 0..1 of ring capacity
    uint64_t underruns = 0;
    uint64_t overflowFrames = 0;
    double   deviceRate = 0.0;
    double   driftPpm = 0.0;
    float    workerCpuMs = 0.f;    // cumulative worker CPU ms (thread time), for benchmarking
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

    // ---- control thread (non-RT) ----
    void setHostSampleRate(double sr) noexcept { hostRate_.store(sr, std::memory_order_relaxed); bump(); }
    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); bump(); }
    void setDeviceIndex(int idx) noexcept { deviceIndex_.store(idx, std::memory_order_relaxed); bump(); }
    void setGain(float g) noexcept { gain_.store(g, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    void start();   // create worker thread (idempotent). Worker sleeps when disabled.
    void stop();    // join worker thread (idempotent).

    StreamStats stats() const noexcept;

private:
    void bump() noexcept;          // wake worker to re-evaluate config
    void workerMain();
    struct Impl;                   // platform specific (WASAPI / stub)
    std::unique_ptr<Impl> impl_;

    SpscFrameRing ring_{32768};    // ~680 ms @ 48k, 256 KB, allocated once
    std::atomic<bool>   enabled_{false};
    std::atomic<int>    deviceIndex_{0};
    std::atomic<double> hostRate_{48000.0};
    std::atomic<float>  gain_{1.0f};
    std::atomic<uint32_t> configGen_{0};

    std::atomic<int>      status_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<double>   devRate_{0.0};
    std::atomic<double>   driftPpm_{0.0};
    std::atomic<float>    fill_{0.f};
    std::atomic<float>    cpuMs_{0.f};
    friend struct Impl;
};

} // namespace asb
