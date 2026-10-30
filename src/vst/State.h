// Plugin state (de)serialization shared by Processor and Controller.
// v1: int32 ver=1, int32 enable, double level, int32 deviceIndex
// v2: v1 fields with ver=2, then str8 deviceId (WASAPI endpoint id; "" = system default).
//     On load the id wins; the index is only a fallback (e.g. device unplugged).
#pragma once
#include "base/source/fstreamer.h"
#include <string>

namespace asb {

struct PluginState {
    bool        enable = false;
    double      level = 60.0 / 66.0;
    int         deviceIndex = 0;
    std::string deviceId;      // empty: system default (or v1 state)
    int         version = 2;
};

inline bool readState(Steinberg::IBStreamer& s, PluginState& st)
{
    Steinberg::int32 ver = 0, en = 0, dev = 0; double lvl = 0;
    if (!s.readInt32(ver) || (ver != 1 && ver != 2)) return false;
    if (!s.readInt32(en) || !s.readDouble(lvl) || !s.readInt32(dev)) return false;
    st.version = ver; st.enable = en != 0; st.level = lvl; st.deviceIndex = dev; st.deviceId.clear();
    if (ver >= 2) {
        Steinberg::char8* id = s.readStr8();
        if (id) { st.deviceId = id; delete[] id; }
    }
    return true;
}

inline bool writeState(Steinberg::IBStreamer& s, const PluginState& st)
{
    return s.writeInt32(2) && s.writeInt32(st.enable ? 1 : 0) && s.writeDouble(st.level) &&
           s.writeInt32(st.deviceIndex) && s.writeStr8(st.deviceId.c_str());
}

// Which list index to use after loading: the saved endpoint id if it is in the list, otherwise
// the saved index (v1 behaviour). Returns 0 (system default) if that index is out of range too.
template <class Devices>
inline int resolveSavedDevice(const PluginState& st, const Devices& devs)
{
    if (st.version >= 2 && st.deviceIndex > 0 && !st.deviceId.empty()) {
        for (size_t i = 0; i < devs.size(); ++i) if (devs[i].id == st.deviceId) return int(i);
        return 0;      // that exact device is gone: fall back to system default, not a random neighbour
    }
    return (st.deviceIndex >= 0 && size_t(st.deviceIndex) < devs.size()) ? st.deviceIndex : 0;
}

} // namespace asb
