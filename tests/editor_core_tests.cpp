// Headless tests for the shared window logic (EditorCore): paint passes run, clicks change parameters,
// settings/prefs round-trip, layout stays inside the window, and frames are only requested when something changed.
#include "ui/EditorCore.h"
#include "vst/Controller.h"
#include "vst/Ids.h"
#include <cstdio>
#include <string>

using namespace asb;
using namespace asb::ui;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::string g_prefs;
namespace asb::ui {
std::string readPrefsFile() { return g_prefs; }
void writePrefsFile(const std::string& s) { g_prefs = s; }
}

struct Rec : Canvas {
    int fills = 0, texts = 0, ellipses = 0, strokes_ = 0, layers = 0, copies = 0, outside = 0, smoothDepth = 0, maxDepth = 0;
    std::vector<std::string> strs;
    void chk(const Rect& r) { if (r.l < 0 || r.t < 0 || r.r > kLogicalW || r.b > kLogicalH) ++outside; }
    void fill(const Rect& r, Color) override { ++fills; chk(r); }
    void text(const Rect& r, const std::string& s, Color, Font, unsigned, int) override { ++texts; chk(r); strs.push_back(s); }
    void ellipse(float, float, float, float, Color, int) override { ++ellipses; }
    void strokes(const std::vector<Polyline>& f, float, Color, int, float, float) override { ++strokes_; CHECK(!f.empty()); }
    void perforation(int, Color) override {}
    void beginSmooth() override { maxDepth = std::max(maxDepth, ++smoothDepth); }
    void endSmooth() override { --smoothDepth; }
    void drawLayer(Layer) override { ++layers; }
    void copyLayerRect(Layer, const Rect& r) override { ++copies; chk(r); }
    bool has(const std::string& s) const { for (auto& x : strs) if (x == s) return true; return false; }
};
struct Host : EditorHost {
    int repaints = 0, layers = 0, captures = 0; std::string url;
    void repaint() override { ++repaints; }
    void layersChanged() override { ++layers; }
    void openUrl(const std::string& u) override { url = u; }
    void captureMouse(bool on) override { captures += on ? 1 : 0; }
};

int main()
{
    Controller ctl;
    ctl.initialize(nullptr);
    Host host;
    EditorCore core(&ctl, &host);

    std::printf("paint passes\n");
    { Rec r; core.paintPanel(r); CHECK(r.strokes_ == 10); CHECK(r.ellipses > 50); CHECK(r.smoothDepth == 0); CHECK(r.outside == 0); CHECK(r.has("by maai")); }
    { Rec r; core.paintLit(r); CHECK(r.ellipses > 50); CHECK(r.smoothDepth == 0); }
    core.tick();
    { Rec r; core.paintFrame(r); CHECK(r.layers == 1); CHECK(r.outside == 0); CHECK(r.has("OUTPUT") && r.has("START STREAM")); }

    std::printf("frames only on change\n");
    host.repaints = 0; core.tick(); core.tick();
    CHECK(host.repaints == 0);

    std::printf("stream button toggles the parameter\n");
    core.mouseDown(380, 247, false, false);
    CHECK(ctl.getParamNormalized(kParamStreamEnable) >= 0.5);
    { Rec r; core.paintFrame(r); CHECK(r.has("STREAMING")); }
    core.mouseDown(380, 247, false, false);
    CHECK(ctl.getParamNormalized(kParamStreamEnable) < 0.5);

    std::printf("fader drag + double-click = 0 dB\n");
    core.mouseDown(118, 143, false, false); core.mouseMove(118, 143, false); core.mouseUp();
    CHECK(ctl.getParamNormalized(kParamOutputLevel) < 0.01);
    CHECK(host.captures == 1);
    core.mouseDown(200, 143, false, true);
    CHECK(std::abs(ctl.getParamNormalized(kParamOutputLevel) - 60.0 / 66.0) < 1e-9);

    std::printf("settings page: latency, channels, accent, size, prefs saved\n");
    core.mouseDown(425, 35, false, false);                   // gear
    { Rec r; core.paintFrame(r); CHECK(r.has("LATENCY") && r.has("120 ms") && r.outside == 0); }
    core.mouseDown(420, 94, false, false);                   // 120 ms (last latency segment, row 0)
    CHECK(latencyIndexFromNorm(ctl.getParamNormalized(kParamLatency)) == 4);
    core.mouseDown(200, 136, false, false);                  // MONO (2nd channel segment, row 1)
    CHECK(channelIndexFromNorm(ctl.getParamNormalized(kParamChannels)) == 1);
    core.mouseDown(118 + 3 * 30 + 10, 157 + 22, false, false);   // accent: Blue
    CHECK(host.layers == 1 && core.accent() == kAccents[3]);
    core.mouseDown(250, 220, false, false);                  // size 125 % (2nd size segment, row 3)
    CHECK(host.layers == 2 && core.pixelWidth() == 575);
    const Prefs p = parsePrefs(g_prefs);
    CHECK(p.accent == 3 && p.scale == 1);
    core.escape();
    { Rec r; core.paintFrame(r); CHECK(r.has("OUTPUT")); }

    std::printf("prefs parse/format\n");
    { Prefs a; a.accent = 5; a.scale = 3; a.autoUpdate = false; a.lastCheck = 1793388263; a.latestTag = "v0.6.1";
      const Prefs b = parsePrefs(formatPrefs(a));
      CHECK(b.accent == 5 && b.scale == 3 && !b.autoUpdate && b.lastCheck == 1793388263 && b.latestTag == "v0.6.1");
      CHECK(parsePrefs("[ui]\naccent=99\nscale=-4\n").accent == kAccentCount - 1);
      CHECK(parsePrefs("garbage").scale == 0); }

    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
