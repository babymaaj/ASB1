// Plugin state format: v2 round trip, v1 projects still load, saved device found by endpoint id.
#include "vst/State.h"
#include "engine/StreamEngine.h"
#include "public.sdk/source/common/memorystream.h"
#include <cstdio>
#include <vector>

using namespace Steinberg;
using namespace asb;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::vector<DeviceInfo> devs(std::initializer_list<const char*> ids)
{
    std::vector<DeviceInfo> d{{"", "System default"}};
    for (auto* i : ids) d.push_back({i, std::string("dev ") + i});
    return d;
}

int main()
{
    std::printf("state v2 round trip\n");
    {
        MemoryStream m; IBStreamer w(&m, kLittleEndian);
        PluginState a; a.enable = true; a.level = 0.5; a.deviceIndex = 2; a.deviceId = "{0.0.0.00000000}.{CABLE}";
        CHECK(writeState(w, a));
        int64 pos; m.seek(0, IBStream::kIBSeekSet, &pos);
        IBStreamer r(&m, kLittleEndian); PluginState b;
        CHECK(readState(r, b));
        CHECK(b.version == 2 && b.enable && b.level == 0.5 && b.deviceIndex == 2 && b.deviceId == a.deviceId);
    }
    std::printf("v1 project still loads\n");
    {
        MemoryStream m; IBStreamer w(&m, kLittleEndian);
        w.writeInt32(1); w.writeInt32(1); w.writeDouble(0.25); w.writeInt32(1);
        int64 pos; m.seek(0, IBStream::kIBSeekSet, &pos);
        IBStreamer r(&m, kLittleEndian); PluginState b;
        CHECK(readState(r, b));
        CHECK(b.version == 1 && b.enable && b.level == 0.25 && b.deviceIndex == 1 && b.deviceId.empty());
        CHECK(resolveSavedDevice(b, devs({"A", "B"})) == 1);    // v1: index as before
        CHECK(resolveSavedDevice(b, devs({})) == 0);            // out of range -> default
    }
    std::printf("garbage / future versions rejected\n");
    {
        MemoryStream m; IBStreamer w(&m, kLittleEndian); w.writeInt32(99);
        int64 pos; m.seek(0, IBStream::kIBSeekSet, &pos);
        IBStreamer r(&m, kLittleEndian); PluginState b;
        CHECK(!readState(r, b));
    }
    std::printf("device found by id after list order changes\n");
    {
        PluginState st; st.deviceIndex = 1; st.deviceId = "CABLE";
        CHECK(resolveSavedDevice(st, devs({"CABLE", "SPK"})) == 1);
        CHECK(resolveSavedDevice(st, devs({"AAA_NEW_USB", "CABLE", "SPK"})) == 2); // new device sorted first
        CHECK(resolveSavedDevice(st, devs({"SPK"})) == 0);      // unplugged -> system default, not SPK
        PluginState def; def.deviceIndex = 0;
        CHECK(resolveSavedDevice(def, devs({"X"})) == 0);
    }
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
