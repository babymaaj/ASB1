// Consumer-side pull resampler with clock-drift compensation.
// 4-point cubic (Catmull-Rom) interpolation: cheap, and at the worst fractional
// position it loses ~0.5 dB @10 kHz vs ~2 dB for linear (see core_tests). Not a
// mastering SRC: no anti-alias filter for 48k->44.1k. Output level is smoothed
// (no zipper on level moves) and fades in after every (re)prime (no click). The step ratio is nudged by a slow P-controller on ring
// fill level so producer/consumer clock drift never lets the ring run dry or
// grow without bound. If the ring backs up far past target (device stalled,
// slow device start, USB hiccup) it hard-resyncs: drops the oldest audio down
// to target, because drift control alone would take minutes to drain it.
// Runs on the streaming worker thread only.
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
    // resyncAboveFrames: fill level that triggers a hard resync (0 = 3x target).
    void configure(SpscFrameRing* ring, double hostRate, double devRate, size_t targetFillFrames,
                   size_t resyncAboveFrames = 0)
    {
        ring_     = ring;
        baseStep_ = hostRate / devRate;
        target_   = static_cast<double>(targetFillFrames);
        resyncAbove_ = resyncAboveFrames ? resyncAboveFrames : targetFillFrames * 3;
        reset();
    }

    void reset() noexcept
    {
        have_ = pos_ = 0;
        frac_ = 0.0;
        adj_  = 0.0;
        primed_ = false;
        g_ = 0.f;
        for (auto& c : h_) c[0] = c[1] = 0.f;
    }

    // Catmull-Rom between s[1] and s[2]; s[0], s[3] are the outer neighbours. f in [0,1).
    static float interp(const float s[4], float f) noexcept
    {
        const float c1 = 0.5f * (s[2] - s[0]);
        const float c2 = s[0] - 2.5f * s[1] + 2.f * s[2] - 0.5f * s[3];
        const float c3 = 0.5f * (s[3] - s[0]) + 1.5f * (s[1] - s[2]);
        return ((c3 * f + c2) * f + c1) * f + s[1];
    }

    // Produce exactly outFrames interleaved stereo frames (gain applied).
    // Returns frames of real audio; remainder is silence (underrun -> re-prime).
    size_t render(float* out, size_t outFrames, float gain) noexcept
    {
        const size_t fill = fillFrames();
        if (fill > resyncAbove_) {          // way behind: jump to target instead of crawling back
            ring_->discard(fill - static_cast<size_t>(target_));
            adj_ = 0.0;
            ++resyncs_;
        }
        if (!primed_) {
            if (static_cast<double>(ring_->readableFrames()) < target_) {
                std::fill(out, out + outFrames * 2, 0.f);
                return 0;
            }
            if (!pull() || !pull() || !pull()) return underrun(out, 0, outFrames);
            frac_ = 0.0;
            g_ = 0.f;                       // fade in from silence
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
            const float l[4] = {h_[0][0], h_[1][0], h_[2][0], h_[3][0]};
            const float r[4] = {h_[0][1], h_[1][1], h_[2][1], h_[3][1]};
            g_ += (gain - g_) * kGainCoef;  // one-pole, ~5 ms; reaches the target to float precision
            if (std::fabs(gain - g_) < 1e-6f) g_ = gain;
            out[n * 2]     = interp(l, f) * g_;
            out[n * 2 + 1] = interp(r, f) * g_;
            frac_ += step;
        }
        updateDrift();
        return n;
    }

    uint64_t underruns() const noexcept { return underruns_; }
    uint64_t resyncs() const noexcept { return resyncs_; }
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
        for (int k = 0; k < 3; ++k) { h_[k][0] = h_[k + 1][0]; h_[k][1] = h_[k + 1][1]; }
        h_[3][0] = scratch_[pos_ * 2]; h_[3][1] = scratch_[pos_ * 2 + 1];
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
    static constexpr float kGainCoef = 0.005f; // per output sample: ~4.2 ms time constant @48k
    float  h_[4][2]{};                 // last 4 input frames (L,R), oldest first
    float  g_ = 0.f;                   // smoothed gain actually applied
    float  scratch_[kChunk * 2]{};
    size_t have_ = 0, pos_ = 0;
    bool   primed_ = false;
    size_t   resyncAbove_ = 0;
    uint64_t underruns_ = 0, resyncs_ = 0;
};

} // namespace asb
