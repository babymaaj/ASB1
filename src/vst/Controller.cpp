#include "vst/Controller.h"
#include <utility>
#include <vector>
#include "vst/Ids.h"
#include "vst/State.h"
#include "core/Utf8.h"
#include "engine/StreamEngine.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstparameters.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace asb {

// Level parameter: shows dB.
class LevelParam : public Parameter {
public:
    LevelParam() : Parameter(STR16("Output Level"), kParamOutputLevel, STR16("dB"), 60.0 / 66.0, 0, ParameterInfo::kCanAutomate) {}
    void toString(ParamValue v, String128 s) const override
    {
        UString u(s, 128);
        if (v <= 0.001) { u.fromAscii("-inf"); return; }
        u.printFloat(kMinDb + v * (kMaxDb - kMinDb), 1);
    }
    bool fromString(const TChar* s, ParamValue& v) const override
    {
        UString u(const_cast<TChar*>(s), 128); double d;
        if (!u.scanFloat(d)) return false;
        v = (d - kMinDb) / (kMaxDb - kMinDb); v = v < 0 ? 0 : (v > 1 ? 1 : v); return true;
    }
};

// Stepped device selector: index 0 = system default. Names shown by the host's generic UI.
class DeviceParam : public Parameter {
public:
    explicit DeviceParam(std::vector<DeviceInfo> d)
        : Parameter(STR16("Output Device"), kParamDevice, nullptr, 0.0, kMaxDevices - 1,
                    ParameterInfo::kCanAutomate | ParameterInfo::kIsList), devs_(std::move(d)) {}
    void toString(ParamValue v, String128 s) const override
    {
        UString u(s, 128);
        const size_t i = size_t(toPlain(v) + 0.5);
        if (i < devs_.size()) {
            const std::u16string w = utf8ToUtf16(devs_[i].name);
            u.assign(reinterpret_cast<const char16*>(w.c_str()), int32(w.size()) + 1);
        } else {
            u.fromAscii("(unavailable)");
        }
    }
    bool fromString(const TChar* s, ParamValue& v) const override
    {
        // exact name match (lets hosts/validator round-trip the displayed string)
        for (size_t i = 0; i < devs_.size(); ++i) {
            const std::u16string w = utf8ToUtf16(devs_[i].name);
            if (std::u16string(reinterpret_cast<const char16_t*>(s)) == w) { v = toNormalized(double(i)); return true; }
        }
        return false;
    }
    const std::vector<DeviceInfo>& devices() const { return devs_; }
private:
    std::vector<DeviceInfo> devs_;
};

tresult PLUGIN_API Controller::initialize(FUnknown* context)
{
    tresult r = EditController::initialize(context);
    if (r != kResultOk) return r;

    auto* streaming = new StringListParameter(STR16("Streaming"), kParamStreamEnable, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    streaming->appendString(STR16("Off"));
    streaming->appendString(STR16("On"));
    parameters.addParameter(streaming);
    parameters.addParameter(new LevelParam);

    // Device list: enumerated on the main thread once per plugin instance (reopen plugin to refresh).
    // Fixed stepCount so normalized<->index mapping matches Processor::applyParam without sharing state.
    auto devices = enumerateRenderDevices();
    if (devices.size() > size_t(kMaxDevices)) devices.resize(kMaxDevices);
    parameters.addParameter(new DeviceParam(std::move(devices)));

    parameters.addParameter(STR16("Buffer Health"), nullptr, 0, 0.0, ParameterInfo::kIsReadOnly, kParamBufferHealth);
    parameters.addParameter(STR16("Status"), nullptr, 3, 0.0, ParameterInfo::kIsReadOnly, kParamStatus);
    return kResultOk;
}

tresult PLUGIN_API Controller::setComponentState(IBStream* state)
{
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    PluginState st;
    if (!readState(s, st)) return kResultFalse;
    setParamNormalized(kParamStreamEnable, st.enable ? 1.0 : 0.0);
    setParamNormalized(kParamOutputLevel, st.level);
    if (auto* p = static_cast<DeviceParam*>(parameters.getParameter(kParamDevice)))
        setParamNormalized(kParamDevice, p->toNormalized(resolveSavedDevice(st, p->devices())));
    return kResultOk;
}

} // namespace asb
