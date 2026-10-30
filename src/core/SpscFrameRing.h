// Lock-free single-producer / single-consumer ring of stereo frames.
// Producer = VST3 audio callback. Consumer = streaming worker thread.
// Storage is interleaved L,R float, preallocated at construction (never grows).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace asb {

class SpscFrameRing {
public:
    // Capacity is rounded up to a power of two. Allocation happens ONLY here.
    explicit SpscFrameRing(size_t minFrames)
    {
        size_t c = 64;
        while (c < minFrames) c <<= 1;
        cap_  = c;
        mask_ = c - 1;
        buf_.reset(new float[cap_ * 2]());
    }

    size_t capacity() const noexcept { return cap_; }

    // ---- producer side (real-time safe: no alloc, no locks, no syscalls) ----

    // Planar -> interleaved. Returns frames written; the remainder is dropped
    // (never blocks) and counted in overflowFrames().
    template <class T>
    size_t writePlanar(const T* l, const T* r, size_t n) noexcept
    {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const size_t freeFrames = cap_ - static_cast<size_t>(h - t);
        const size_t w = n < freeFrames ? n : freeFrames;
        float* b = buf_.get();
        for (size_t i = 0; i < w; ++i) {
            const size_t idx = static_cast<size_t>(h + i) & mask_;
            b[idx * 2]     = static_cast<float>(l[i]);
            b[idx * 2 + 1] = static_cast<float>(r[i]);
        }
        head_.store(h + w, std::memory_order_release);
        if (w < n) overflow_.fetch_add(n - w, std::memory_order_relaxed);
        return w;
    }

    size_t writeSilence(size_t n) noexcept
    {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const size_t freeFrames = cap_ - static_cast<size_t>(h - t);
        const size_t w = n < freeFrames ? n : freeFrames;
        float* b = buf_.get();
        for (size_t i = 0; i < w; ++i) {
            const size_t idx = static_cast<size_t>(h + i) & mask_;
            b[idx * 2] = 0.f;
            b[idx * 2 + 1] = 0.f;
        }
        head_.store(h + w, std::memory_order_release);
        if (w < n) overflow_.fetch_add(n - w, std::memory_order_relaxed);
        return w;
    }

    // ---- consumer side ----

    // Reads up to maxFrames interleaved frames. Returns frames read.
    size_t read(float* out, size_t maxFrames) noexcept
    {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        const size_t avail = static_cast<size_t>(h - t);
        const size_t n = maxFrames < avail ? maxFrames : avail;
        const float* b = buf_.get();
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = static_cast<size_t>(t + i) & mask_;
            out[i * 2]     = b[idx * 2];
            out[i * 2 + 1] = b[idx * 2 + 1];
        }
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    // Consumer-side discard (used when re-priming after a device change).
    void discardAll() noexcept
    {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

    // Safe from either thread (approximate).
    size_t readableFrames() const noexcept
    {
        const uint64_t h = head_.load(std::memory_order_acquire);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(h - t);
    }

    uint64_t overflowFrames() const noexcept { return overflow_.load(std::memory_order_relaxed); }

private:
    size_t cap_ = 0, mask_ = 0;
    std::unique_ptr<float[]> buf_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) std::atomic<uint64_t> overflow_{0};
};

} // namespace asb
