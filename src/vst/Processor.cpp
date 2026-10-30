#include "vst/Processor.h"
#include "vst/Ids.h"
#include "vst/State.h"
#include "vst/EngineLink.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace asb {

// 0 dB on the 0..1 mapping: (0 - (-60)) / 66
static constexpr double kDefaultLevelNorm = 60.0 / 66.0;

Processor::Processor() { setControllerClass(kControllerUID); level_ = kDefaultLevelNorm; }
Processor::~Processor() = default;

tresult PLUGIN_API Processor::initialize(FUnknown* context)
{
    tresult r = AudioEffect::initialize(context);
    if (r != kResultOk) return r;
    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
    // Same list (same order, same cap) the controller builds, so indices mean the same device on both sides.
    auto devices = enumerateRenderDevices();
    if (devices.size() > size_t(kMaxDevices)) devices.resize(kMaxDevices);
    engine_.setDevices(std::move(devices));
    engine_.setGain(normToGain(level_));
    return kResultOk;
}

tresult PLUGIN_API Processor::terminate()
{
    engine_.stop();
    return AudioEffect::terminate();
}

tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement* in, int32 nIn, SpeakerArrangement* out, int32 nOut)
{
    if (nIn == 1 && nOut == 1 && in[0] == SpeakerArr::kStereo && out[0] == SpeakerArr::kStereo)
        return AudioEffect::setBusArrangements(in, nIn, out, nOut);
    return kResultFalse;
}

tresult PLUGIN_API Processor::canProcessSampleSize(int32 s)
{
    return (s == kSample32 || s == kSample64) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Processor::setupProcessing(ProcessSetup& setup)
{
    sampleRate_ = setup.sampleRate;
    maxBlock_ = setup.maxSamplesPerBlock;
    engine_.setHostSampleRate(setup.sampleRate);
    engine_.setHostBlockSize(setup.maxSamplesPerBlock);
    return AudioEffect::setupProcessing(setup);
}

tresult PLUGIN_API Processor::setActive(TBool state)
{
    if (state) {
        engine_.setHostSampleRate(sampleRate_);
        engine_.setHostBlockSize(maxBlock_);
        engine_.setGain(normToGain(level_));
        engine_.setDeviceIndex(device_);
        engine_.setEnabled(enable_);
        engine_.start();          // thread blocks idle if streaming disabled
    } else {
        engine_.stop();           // join; no background activity while inactive
    }
    return AudioEffect::setActive(state);
}

void Processor::applyParam(ParamID id, double v) noexcept
{
    // Called from process() on the audio thread: atomics only, no syscalls (worker polls).
    switch (id) {
    case kParamStreamEnable: {
        const bool on = v >= 0.5;
        if (on != enable_) { enable_ = on; engine_.requestEnabled(on); }
        break;
    }
    case kParamOutputLevel:  level_ = v; engine_.setGain(normToGain(v)); break;
    case kParamDevice: {
        const int idx = static_cast<int>(v * (kMaxDevices - 1) + 0.5);
        if (idx != device_) { device_ = idx; engine_.requestDeviceIndex(idx); }
        break;
    }
    default: break;
    }
}

template <class T>
void Processor::passAndStream(ProcessData& d, T* const* in, T* const* out) noexcept
{
    const int32 n = d.numSamples;
    // Path A: transparent passthrough, zero added delay (memcpy only if host gave separate buffers).
    for (int ch = 0; ch < 2; ++ch)
        if (in[ch] != out[ch]) std::memcpy(out[ch], in[ch], sizeof(T) * size_t(n));
    // Meter (UI): skipped for silent blocks.
    if (d.inputs[0].silenceFlags != 0x3) engine_.meterPlanar(in[0], in[1], size_t(n));
    // Path B: copy into lock-free ring. Never blocks; drops (counted) on overflow.
    if (enable_) {
        if (d.inputs[0].silenceFlags == 0x3) engine_.pushSilence(size_t(n));
        else engine_.pushPlanar(in[0], in[1], size_t(n));
    }
}

tresult PLUGIN_API Processor::process(ProcessData& data)
{
    if (data.inputParameterChanges) {
        const int32 np = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < np; ++i) {
            IParamValueQueue* q = data.inputParameterChanges->getParameterData(i);
            if (!q) continue;
            ParamValue v; int32 off;
            const int32 pc = q->getPointCount();
            if (pc > 0 && q->getPoint(pc - 1, off, v) == kResultTrue) applyParam(q->getParameterId(), v);
        }
    }
    if (data.numSamples <= 0 || data.numInputs < 1 || data.numOutputs < 1 ||
        data.inputs[0].numChannels < 2 || data.outputs[0].numChannels < 2)
        return kResultOk;

    if (data.symbolicSampleSize == kSample32)
        passAndStream<float>(data, data.inputs[0].channelBuffers32, data.outputs[0].channelBuffers32);
    else
        passAndStream<double>(data, data.inputs[0].channelBuffers64, data.outputs[0].channelBuffers64);
    data.outputs[0].silenceFlags = data.inputs[0].silenceFlags;

    // Report meters ~every 100 ms (read-only output params; atomics only).
    meterCountdown_ -= data.numSamples;
    if (meterCountdown_ <= 0 && data.outputParameterChanges) {
        meterCountdown_ = static_cast<int>(sampleRate_ * 0.1);
        const StreamStats s = engine_.stats();
        int32 idx;
        if (auto* q = data.outputParameterChanges->addParameterData(kParamBufferHealth, idx)) q->addPoint(0, s.bufferFill * 2.0 > 1.0 ? 1.0 : s.bufferFill * 2.0, idx);
        if (auto* q = data.outputParameterChanges->addParameterData(kParamStatus, idx)) q->addPoint(0, static_cast<int>(s.status) / 3.0, idx);
    }
    return kResultOk;
}

// Give the controller (editor) read-only access to engine stats. Main thread only. The controller ignores the
// pointer unless it is in the same process (hosts that bridge plugins into another process just get no live meter).
void Processor::sendEngineLink(bool attach)
{
    IMessage* m = allocateMessage();
    if (!m) return;
    m->setMessageID(kEngineLinkMsg);
    m->getAttributes()->setInt(kEngineLinkPtr, attach ? int64(reinterpret_cast<intptr_t>(&engine_)) : 0);
    m->getAttributes()->setInt(kEngineLinkPid, currentProcessId());
    sendMessage(m);
    m->release();
}

tresult PLUGIN_API Processor::connect(IConnectionPoint* other)
{
    const tresult r = AudioEffect::connect(other);
    if (r == kResultOk) sendEngineLink(true);
    return r;
}

tresult PLUGIN_API Processor::disconnect(IConnectionPoint* other)
{
    sendEngineLink(false);   // before the link goes away
    return AudioEffect::disconnect(other);
}

tresult PLUGIN_API Processor::setState(IBStream* state)
{
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    PluginState st;
    if (!readState(s, st)) return kResultFalse;
    enable_ = st.enable; level_ = st.level;
    device_ = resolveSavedDevice(st, engine_.devices());
    engine_.setGain(normToGain(level_));
    engine_.setDeviceIndex(device_);
    engine_.setEnabled(enable_);
    return kResultOk;
}

tresult PLUGIN_API Processor::getState(IBStream* state)
{
    if (!state) return kResultFalse;
    IBStreamer s(state, kLittleEndian);
    PluginState st;
    st.enable = enable_; st.level = level_; st.deviceIndex = device_;
    const auto& devs = engine_.devices();
    if (device_ > 0 && size_t(device_) < devs.size()) st.deviceId = devs[size_t(device_)].id;
    return writeState(s, st) ? kResultOk : kResultFalse;
}

} // namespace asb
