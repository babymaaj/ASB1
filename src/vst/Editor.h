// Plugin window. Windows: native Win32 (no UI framework). Other platforms: none (host generic UI).
#pragma once
#include "pluginterfaces/gui/iplugview.h"

namespace asb {
class Controller;
Steinberg::IPlugView* createEditor(Controller* c);   // nullptr where unsupported
} // namespace asb
