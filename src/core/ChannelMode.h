// What goes to the stream: stereo as-is, mono sum, swapped, or one side on both channels.
// Audio thread: no allocation; writes into caller-provided float buffers.
#pragma once
#include <cstddef>

namespace asb {

enum class ChannelMode : int { Stereo = 0, Mono = 1, Swap = 2, LeftOnly = 3, RightOnly = 4 };
constexpr int kChannelModeCount = 5;

template <class T>
inline void applyChannelMode(ChannelMode m, const T* l, const T* r, size_t n, float* outL, float* outR) noexcept
{
    switch (m) {
    case ChannelMode::Mono:
        for (size_t i = 0; i < n; ++i) outL[i] = outR[i] = 0.5f * (float(l[i]) + float(r[i]));
        break;
    case ChannelMode::Swap:
        for (size_t i = 0; i < n; ++i) { outL[i] = float(r[i]); outR[i] = float(l[i]); }
        break;
    case ChannelMode::LeftOnly:
        for (size_t i = 0; i < n; ++i) outL[i] = outR[i] = float(l[i]);
        break;
    case ChannelMode::RightOnly:
        for (size_t i = 0; i < n; ++i) outL[i] = outR[i] = float(r[i]);
        break;
    default:
        for (size_t i = 0; i < n; ++i) { outL[i] = float(l[i]); outR[i] = float(r[i]); }
        break;
    }
}

} // namespace asb
