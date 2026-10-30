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
#include <cstring>
#include <type_traits>
#include <memory>
#include <string>
#include <vector>

namespace asb {

struct DeviceInfo {
    std::string id;           // WASAPI endpoint id (UTF-8); empty = system default
    std::string name;         // friendly name
};

int64_t currentProcessId();   // platform helper (used to validate in-process links)

// Deterministic order shared by controller + engine: [0]=system default, then by id.
std::vector<DeviceInfo> enumerateRenderDevices();

// Streaming latency policy, shared by the engines and tests.
// Target ring fill = max(30 ms, 2 host blocks): a big ASIO buffer (1024/2048) delivers
// audio in large bursts, so the cushion must cover at least two of them.
inline size_t targetFillFrames(double hostRate, int hostBlock, int targetMs = 30)
{
    const size_t ms = static_cast<size_t>(hostRate * (targetMs > 0 ? targetMs : 30) / 1000.0);
    const size_t blk2 = static_cast<size_t>(hostBlock > 0 ? hostBlock : 0) * 2;
    return ms > blk2 ? ms : blk2;
}
// Above this fill the resampler hard-resyncs to target (see DriftResampler).
inline size_t resyncAboveFrames(double hostRate, int hostBlock, int targetMs = 30)
{
    return targetFillFrames(hostRate, hostBlock, targetMs) * 3 + static_cast<size_t>(hostBlock > 0 ? hostBlock : 0);
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
    double   latencyMs = 0.0;      // audio currently buffered (ring), in ms; excludes the device's own buffer
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
    // Peak meter for the UI (input, pre-gain). Audio thread: a max over the block, a few ns per sample.
    template <class T> void meterPlanar(const T* l, const T* r, size_t n) noexcept
    {
        float a = peak_[0].load(std::memory_order_relaxed), b = peak_[1].load(std::memory_order_relaxed);
        if constexpr (std::is_same_v<T, float>) {
            // |x| via the sign bit and max as unsigned ints: same order for non-NaN floats, and it vectorizes.
            uint32_t ma = 0, mb = 0;
            for (size_t i = 0; i < n; ++i) {
                uint32_t x, y;
                std::memcpy(&x, l + i, 4); std::memcpy(&y, r + i, 4);
                x &= 0x7fffffffu; y &= 0x7fffffffu;
                ma = x > ma ? x : ma; mb = y > mb ? y : mb;
            }
            if (ma > 0x7f800000u) ma = 0;   // ignore NaN
            if (mb > 0x7f800000u) mb = 0;
            float fa, fb; std::memcpy(&fa, &ma, 4); std::memcpy(&fb, &mb, 4);
            a = fa > a ? fa : a; b = fb > b ? fb : b;
        } else {
            for (size_t i = 0; i < n; ++i) {
                const float x = static_cast<float>(l[i] < 0 ? -l[i] : l[i]);
                const float y = static_cast<float>(r[i] < 0 ? -r[i] : r[i]);
                a = x > a ? x : a; b = y > b ? y : b;
            }
        }
        peak_[0].store(a, std::memory_order_relaxed); peak_[1].store(b, std::memory_order_relaxed);
    }
    // UI thread: peak since the last call (then reset). Racing with the audio thread can only show one stale frame.
    float takePeak(int ch) const noexcept { return peak_[ch & 1].exchange(0.f, std::memory_order_relaxed); }
    float gain() const noexcept { return gain_.load(std::memory_order_relaxed); }

    // ---- audio thread: atomics only, never a syscall. The worker notices within ~50 ms. ----
    void requestEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); configGen_.fetch_add(1, std::memory_order_release); }
    void requestTargetMs(int ms) noexcept { targetMs_.store(ms, std::memory_order_relaxed); configGen_.fetch_add(1, std::memory_order_release); }
    void requestDeviceIndex(int idx) noexcept { deviceIndex_.store(idx, std::memory_order_relaxed); configGen_.fetch_add(1, std::memory_order_release); }
    void setGain(float g) noexcept { gain_.store(g, std::memory_order_relaxed); }

    // ---- control thread (non-RT): same as above but also wakes the worker immediately ----
    void setHostSampleRate(double sr) noexcept { hostRate_.store(sr, std::memory_order_relaxed); bump(); }
    void setHostBlockSize(int frames) noexcept { hostBlock_.store(frames, std::memory_order_relaxed); bump(); }
    void setEnabled(bool on) noexcept { requestEnabled(on); bump(); }
    void setDeviceIndex(int idx) noexcept { requestDeviceIndex(idx); bump(); }
    void setTargetMs(int ms) noexcept { requestTargetMs(ms); bump(); }
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
    std::atomic<int>    targetMs_{30};
    std::atomic<float>  gain_{1.0f};
    mutable std::atomic<float> peak_[2]{};
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
