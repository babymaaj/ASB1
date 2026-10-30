#pragma once
#include "engine/StreamEngine.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include <atomic>

namespace asb {

class Processor : public Steinberg::Vst::AudioEffect {
public:
    Processor();
    ~Processor() override;

    static Steinberg::FUnknown* createInstance(void*) { return static_cast<Steinberg::Vst::IAudioProcessor*>(new Processor); }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

    // test/benchmark hook
    StreamEngine& engine() { return engine_; }

private:
    template <class T> void passAndStream(Steinberg::Vst::ProcessData& d, T* const* in, T* const* out) noexcept;
    void applyParam(Steinberg::Vst::ParamID id, double v) noexcept;

    StreamEngine engine_;
    double level_ = 0.75;        // normalized; ~ 0 dB default (see Ids.h mapping)
    bool   enable_ = false;
    int    device_ = 0;
    int    meterCountdown_ = 0;
    double sampleRate_ = 48000.0;
    int    maxBlock_ = 512;
};

} // namespace asb
