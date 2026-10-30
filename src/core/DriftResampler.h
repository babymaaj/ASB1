// Consumer-side pull resampler with clock-drift compensation.
// Linear interpolation (deliberately cheap: this is a monitor/stream feed, not
// a mastering SRC). The step ratio is nudged by a slow P-controller on ring
// fill level so producer/consumer clock drift never lets the ring run dry or
// grow without bound. Runs on the streaming worker thread only.
#pragma once
#include "SpscFrameRing.h"
#include <algorithm>
#include <cmath>

namespace asb {

class DriftResampler {
public:
    static constexpr size_t kChunk = 512;

    // hostRate: rate frames enter the ring. devRate: rate we must output.
    // targetFillFrames: desired ring fill (host-rate frames) == streaming latency.
    void configure(SpscFrameRing* ring, double hostRate, double devRate, size_t targetFillFrames)
    {
        ring_     = ring;
        baseStep_ = hostRate / devRate;
        target_   = static_cast<double>(targetFillFrames);
        reset();
    }

    void reset() noexcept
    {
        have_ = pos_ = 0;
        frac_ = 0.0;
        adj_  = 0.0;
        primed_ = false;
        prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0.f;
    }

    // Produce exactly outFrames interleaved stereo frames (gain applied).
    // Returns frames of real audio; remainder is silence (underrun -> re-prime).
    size_t render(float* out, size_t outFrames, float gain) noexcept
    {
        if (!primed_) {
            if (static_cast<double>(ring_->readableFrames()) < target_) {
                std::fill(out, out + outFrames * 2, 0.f);
                return 0;
            }
            if (!pull() || !pull()) return underrun(out, 0, outFrames);
            frac_ = 0.0;
            primed_ = true;
        }
        const double step = baseStep_ * (1.0 + adj_);
        size_t n = 0;
        for (; n < outFrames; ++n) {
            while (frac_ >= 1.0) {
                if (!pull()) return underrun(out, n, outFrames);
                frac_ -= 1.0;
            }
            const float f = static_cast<float>(frac_);
            out[n * 2]     = (prev_[0] + (cur_[0] - prev_[0]) * f) * gain;
            out[n * 2 + 1] = (prev_[1] + (cur_[1] - prev_[1]) * f) * gain;
            frac_ += step;
        }
        updateDrift();
        return n;
    }

    uint64_t underruns() const noexcept { return underruns_; }
    double   adjust() const noexcept { return adj_; }
    bool     primed() const noexcept { return primed_; }
    size_t   fillFrames() const noexcept { return ring_->readableFrames() + (have_ - pos_); }

private:
    size_t underrun(float* out, size_t from, size_t total) noexcept
    {
        ++underruns_;
        primed_ = false;
        std::fill(out + from * 2, out + total * 2, 0.f);
        return from;
    }

    bool pull() noexcept
    {
        if (pos_ >= have_) {
            have_ = ring_->read(scratch_, kChunk);
            pos_  = 0;
            if (have_ == 0) return false;
        }
        prev_[0] = cur_[0]; prev_[1] = cur_[1];
        cur_[0] = scratch_[pos_ * 2]; cur_[1] = scratch_[pos_ * 2 + 1];
        ++pos_;
        return true;
    }

    void updateDrift() noexcept
    {
        // err > 0: ring too full -> consume slightly faster (larger step).
        const double err  = (static_cast<double>(fillFrames()) - target_) / std::max(target_, 1.0);
        const double want = std::clamp(err * 0.01, -0.005, 0.005); // +-0.5% authority
        adj_ += (want - adj_) * 0.02;
    }

    SpscFrameRing* ring_ = nullptr;
    double baseStep_ = 1.0, target_ = 0.0, frac_ = 0.0, adj_ = 0.0;
    float  prev_[2]{}, cur_[2]{};
    float  scratch_[kChunk * 2]{};
    size_t have_ = 0, pos_ = 0;
    bool   primed_ = false;
    uint64_t underruns_ = 0;
};

} // namespace asb
