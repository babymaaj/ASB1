#include "vst/Controller.h"
#include "vst/Ids.h"
#include "vst/Processor.h"
#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

bool InitModule() { return true; }
bool DeinitModule() { return true; }

BEGIN_FACTORY_DEF("maai", "https://maai.lol", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(asb::kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           asb::kPluginName, Vst::kDistributable, Vst::PlugType::kFx, asb::kVersion, kVstVersionString,
           asb::Processor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(asb::kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "ASB Controller", 0, "", asb::kVersion, kVstVersionString,
           asb::Controller::createInstance)

END_FACTORY
