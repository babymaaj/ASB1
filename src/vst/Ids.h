#pragma once
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"
#include <cmath>

namespace asb {

enum ParamId : Steinberg::Vst::ParamID {
    kParamStreamEnable = 0,   // 0/1
    kParamOutputLevel  = 1,   // normalized 0..1 -> -60..+6 dB (0 = -inf below -59.9)
    kParamDevice       = 2,   // stepped list index (0 = system default)
    kParamBufferHealth = 3,   // read-only meter, 0..1 ring fill
    kParamStatus       = 4,   // read-only: 0 Off,1 Connecting,2 Streaming,3 Error (normalized /3)
    kParamLatency      = 5,   // stepped: index into kLatencyMs
    kParamChannels     = 6,   // stepped: ChannelMode (Stereo, Mono, Swap, Left, Right)
};

// Stream latency choices (target buffer; the device adds its own ~10 ms). Default: 30 ms.
constexpr int kLatencyMs[] = {10, 20, 30, 60, 120};
constexpr int kLatencyCount = 5, kLatencyDefault = 2;
inline int latencyIndexFromNorm(double v) { const int i = int(v * (kLatencyCount - 1) + 0.5); return i < 0 ? 0 : (i >= kLatencyCount ? kLatencyCount - 1 : i); }
inline double latencyNorm(int i) { return double(i) / (kLatencyCount - 1); }
constexpr int kChannelCount = 5;
inline int channelIndexFromNorm(double v) { const int i = int(v * (kChannelCount - 1) + 0.5); return i < 0 ? 0 : (i >= kChannelCount ? kChannelCount - 1 : i); }
inline double channelNorm(int i) { return double(i) / (kChannelCount - 1); }

constexpr int    kMaxDevices = 32;
constexpr double kMinDb = -60.0, kMaxDb = 6.0;

inline float normToGain(double n)
{
    if (n <= 0.001) return 0.f;
    const double db = kMinDb + n * (kMaxDb - kMinDb);
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

static const Steinberg::FUID kProcessorUID(0x5A7C1E01, 0x3B4D4F10, 0x9E2A6C11, 0xA51B0D01);
static const Steinberg::FUID kControllerUID(0x5A7C1E02, 0x3B4D4F10, 0x9E2A6C11, 0xA51B0D02);

constexpr const char* kPluginName = "ASB";            // ASIO Stream Bridge, by maai
constexpr const char* kVersion    = "0.5.0-alpha";

} // namespace asb
