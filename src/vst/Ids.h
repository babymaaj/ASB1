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
};

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

constexpr const char* kPluginName = "ASIO Stream Bridge";
constexpr const char* kVersion    = "0.3.0-alpha";

} // namespace asb
