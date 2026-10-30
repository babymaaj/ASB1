#pragma once
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "engine/StreamEngine.h"
#include <atomic>
#include <vector>

namespace asb {
class Controller : public Steinberg::Vst::EditController {
public:
    static Steinberg::FUnknown* createInstance(void*) { return static_cast<Steinberg::Vst::IEditController*>(new Controller); }
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // For the editor (UI thread).
    const std::vector<DeviceInfo>& devices() const;
    // Live engine stats if the processor is in this process; false otherwise (then use the read-only params).
    bool liveStats(StreamStats& out) const;
    float takeInputPeak(int ch) const { const StreamEngine* e = engine_.load(); return e ? e->takePeak(ch) : 0.f; }
    float streamGain() const { const StreamEngine* e = engine_.load(); return e ? e->gain() : 1.f; }
    // Parameter change from the UI: tells the host (automation/undo) and updates our copy.
    void editParam(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue v);

private:
    std::atomic<const StreamEngine*> engine_{nullptr};
};
} // namespace asb
