#include "ui/EditorCore.h"
#include "vst/Controller.h"
#include "vst/Ids.h"
#include "vst/Update.h"
#include "core/Version.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

namespace asb::ui {

// ---------------------------------------------------------------- constants
namespace {
constexpr int kW = kLogicalW, kH = kLogicalH;
constexpr int kPad = 24, kCol = 118, kRight = kW - kPad;
const Rect rHeader{0, 0, kW, 72};
const Rect rOutput{0, 73, kW, 119};
const Rect rLevel{0, 120, kW, 166};
const Rect rSignal{0, 167, kW, 213};
const Rect rFader{kCol, 136, kRight - 70, 150};
const Rect rButton{kRight - 158, 228, kRight, 266};
const Rect rGear{kRight - 22, 24, kRight, 46};
constexpr int kListRowH = 28, kListMaxRows = 7;

constexpr int kDot = 5, kPitch = 7;
constexpr int kMeterX = kCol, kMeterSpan = (kRight - 44) - kCol;
constexpr int kDots = (kMeterSpan + (kPitch - kDot)) / kPitch;
constexpr int kMeterY0 = 181, kMeterRowGap = 10;

constexpr Color cBg = rgb(20, 20, 21), cPerf = rgb(25, 25, 26), cLine = rgb(42, 42, 44), cInk = rgb(232, 232, 228),
                cDim = rgb(128, 128, 126), cFaint = rgb(66, 66, 68), cAmber = rgb(226, 170, 72), cRed = rgb(226, 84, 70),
                cPanel = rgb(28, 28, 29), cDotOff = rgb(36, 36, 38), cOnAccent = rgb(16, 18, 14);

constexpr int kSetTop = 73, kSetRowH = 42;
Rect setRow(int i) { return Rect{0, kSetTop + i * kSetRowH, kW, kSetTop + (i + 1) * kSetRowH}; }
enum SetRow { kRowLatency = 0, kRowChannels, kRowAccent, kRowSize, kRowUpdates, kSetRows };

constexpr int kHoverNone = -100, kHoverOutput = -10, kHoverButton = -11, kHoverFader = -12, kHoverGear = -13,
              kHoverUpdate = -14, kHoverCheck = -15, kHoverAuto = -16;
int segHover(int row, int idx) { return 1000 + row * 16 + idx; }

double levelDb(double norm) { return kMinDb + norm * (kMaxDb - kMinDb); }
double dbOf(float lin) { return lin > 1e-6f ? 20.0 * std::log10(double(lin)) : -120.0; }
double meterPos(double db) { return std::clamp((db + 48.0) / 51.0, 0.0, 1.0); }   // -48..+3 dB -> 0..1
Color lighten(Color c, int d) { return rgb(std::min(255, red(c) + d), std::min(255, green(c) + d), std::min(255, blue(c) + d)); }
Rect faderHit() { return Rect{rFader.l - 8, rFader.t - 8, rFader.r + 8, rFader.b + 8}; }

int segCount(int row)
{
    switch (row) {
    case kRowLatency: return kLatencyCount;
    case kRowChannels: return kChannelCount;
    case kRowAccent: return kAccentCount;
    case kRowSize: return kScaleCount;
    default: return 0;
    }
}
Rect segRect(int row, int i)
{
    const Rect r = setRow(row);
    if (row == kRowAccent) { const int x = kCol + i * 30; return Rect{x, r.t + 12, x + 20, r.t + 32}; }
    const int n = segCount(row), gap = 4;
    const int w = (kRight - kCol - gap * (n - 1)) / n;
    return Rect{kCol + i * (w + gap), r.t + 9, kCol + i * (w + gap) + w, r.b - 9};
}
Rect updateLinkRect() { const Rect r = setRow(kRowUpdates); return Rect{kCol, r.t + 6, kRight - 150, r.b - 6}; }
Rect checkRect() { const Rect r = setRow(kRowUpdates); return Rect{kRight - 66, r.t + 9, kRight, r.b - 9}; }
Rect autoRect() { const Rect r = setRow(kRowUpdates); return Rect{kRight - 140, r.t + 9, kRight - 72, r.b - 9}; }
Rect headerUpdateRect() { return Rect{kRight - 170, 26, kRight - 112, 44}; }

std::string fmt(const char* f, ...)
{
    char buf[256];
    va_list ap; va_start(ap, f); std::vsnprintf(buf, sizeof buf, f, ap); va_end(ap);
    return buf;
}

// ---------------------------------------------------------------- wordmark geometry
// "ASB" from rounded strokes on a 24 x 40 unit grid per letter (centre lines). The A's crossbar floats free.
enum : int { kLetterA = 1, kLetterS = 2, kLetterB = 4 };
struct FigBuilder {
    std::vector<Polyline> figs;
    float x, y, s;
    void start() { figs.emplace_back(); }
    void pt(float px, float py)
    {
        const PointF p{x + px * s, y + py * s};
        auto& f = figs.back();
        if (f.empty() || std::fabs(f.back().x - p.x) > 1e-4f || std::fabs(f.back().y - p.y) > 1e-4f) f.push_back(p);
    }
    void L(float x0, float y0, float x1, float y1) { pt(x0, y0); pt(x1, y1); }
    void A(float cx, float cy, float r, float startDeg, float sweepDeg)      // y-down, clockwise positive (GDI+ style)
    {
        const int n = std::max(6, int(std::fabs(sweepDeg) / 6));
        for (int i = 0; i <= n; ++i) {
            const double a = (startDeg + sweepDeg * i / n) * 3.14159265358979 / 180.0;
            pt(float(cx + r * std::cos(a)), float(cy + r * std::sin(a)));
        }
    }
};
std::vector<Polyline> wordmarkFigures(float x, float y, float s, int letters)
{
    FigBuilder b{{}, x, y, s};
    const float r = 8;
    if (letters & kLetterA) {
        b.start(); b.L(0, 40, 0, 12); b.A(12, 12, 12, 180, 180); b.L(24, 12, 24, 40);
        b.start(); b.L(7, 27, 17, 27);
    }
    const float sx = 34;
    if (letters & kLetterS) {
        b.start();
        b.L(sx + 24, 0, sx + r, 0); b.A(sx + r, r, r, 270, -90); b.L(sx, r, sx, 20 - r); b.A(sx + r, 20 - r, r, 180, -90);
        b.L(sx + r, 20, sx + 24 - r, 20); b.A(sx + 24 - r, 20 + r, r, 270, 90); b.L(sx + 24, 20 + r, sx + 24, 40 - r);
        b.A(sx + 24 - r, 40 - r, r, 0, 90); b.L(sx + 24 - r, 40, sx, 40);
    }
    const float bx = 68;
    if (letters & kLetterB) {
        b.start(); b.L(bx, 0, bx, 40);
        b.start(); b.L(bx, 0, bx + 22 - r, 0); b.A(bx + 22 - r, r, r, 270, 90); b.L(bx + 22, r, bx + 22, 20 - r);
        b.A(bx + 22 - r, 20 - r, r, 0, 90); b.L(bx + 22 - r, 20, bx, 20);
        b.start(); b.L(bx, 20, bx + 24 - r, 20); b.A(bx + 24 - r, 20 + r, r, 270, 90); b.L(bx + 24, 20 + r, bx + 24, 40 - r);
        b.A(bx + 24 - r, 40 - r, r, 0, 90); b.L(bx + 24 - r, 40, bx, 40);
    }
    return b.figs;
}
} // namespace

// ---------------------------------------------------------------- prefs
const Color kAccents[kAccentCount] = {rgb(150, 205, 120), rgb(236, 236, 230), rgb(232, 92, 78),
                                      rgb(110, 170, 240), rgb(172, 142, 240), rgb(236, 180, 80)};
const char* const kAccentNames[kAccentCount] = {"Green", "White", "Red", "Blue", "Violet", "Amber"};
const double kScales[kScaleCount] = {1.0, 1.25, 1.5, 2.0};

Prefs parsePrefs(const std::string& ini)
{
    Prefs p;
    std::istringstream in(ini);
    std::string line, section;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto num = [&](long long def) { try { return std::stoll(v); } catch (...) { return def; } };
        if (section == "ui" && k == "accent") p.accent = int(std::clamp<long long>(num(0), 0, kAccentCount - 1));
        else if (section == "ui" && k == "scale") p.scale = int(std::clamp<long long>(num(0), 0, kScaleCount - 1));
        else if (section == "updates" && k == "auto") p.autoUpdate = num(1) != 0;
        else if (section == "updates" && k == "lastCheck") p.lastCheck = num(0);
        else if (section == "updates" && k == "latest") p.latestTag = v;
    }
    return p;
}
std::string formatPrefs(const Prefs& p)
{
    return "[ui]\r\naccent=" + std::to_string(p.accent) + "\r\nscale=" + std::to_string(p.scale) +
           "\r\n[updates]\r\nauto=" + (p.autoUpdate ? "1" : "0") + "\r\nlastCheck=" + std::to_string(p.lastCheck) +
           "\r\nlatest=" + p.latestTag + "\r\n";
}

// ---------------------------------------------------------------- core
bool EditorCore::Shown::operator==(const Shown& o) const { return std::memcmp(this, &o, sizeof(Shown)) == 0; }

EditorCore::EditorCore(Controller* c, EditorHost* host) : ctl_(c), host_(host), prefs_(parsePrefs(readPrefsFile())) {}

int EditorCore::pixelWidth() const { return int(std::lround(kW * scale())); }
int EditorCore::pixelHeight() const { return int(std::lround(kH * scale())); }
void EditorCore::savePrefs() { writePrefsFile(formatPrefs(prefs_)); }
void EditorCore::dirty() { forceFrame_ = true; tick(); }

void EditorCore::opened()
{
    const long long now = (long long)std::time(nullptr);
    if (prefs_.autoUpdate && now - prefs_.lastCheck > 24 * 3600) {
        updateCheckStart();
        prefs_.lastCheck = now;
        savePrefs();
    }
}

int EditorCore::deviceIndex() const { return int(ctl_->getParamNormalized(kParamDevice) * (kMaxDevices - 1) + 0.5); }
int EditorCore::deviceCount() const { return int(ctl_->devices().size()); }
void EditorCore::selectDevice(int i)
{
    if (i >= 0 && i < deviceCount()) { ctl_->editParam(kParamDevice, double(i) / double(kMaxDevices - 1)); dirty(); }
}
void EditorCore::stepDevice(int d) { const int n = deviceCount(); if (n > 0) selectDevice(((deviceIndex() + d) % n + n) % n); }
Rect EditorCore::listRect() const
{
    const int rows = std::min(deviceCount(), kListMaxRows);
    return Rect{kCol - 10, rOutput.b - 6, kRight + 4, rOutput.b - 6 + rows * kListRowH + 8};
}
int EditorCore::maxScroll() const { return std::max(0, deviceCount() - kListMaxRows); }
void EditorCore::openList()
{
    listOpen_ = true;
    listScroll_ = std::clamp(deviceIndex() - kListMaxRows / 2, 0, maxScroll());
    hover_ = kHoverNone;
    dirty();
}
int EditorCore::listRowAt(int x, int y) const
{
    const Rect r = listRect();
    if (!r.contains(x, y)) return -1;
    const int row = (y - r.t - 4) / kListRowH;
    if (row < 0 || row >= kListMaxRows) return -1;
    const int i = row + listScroll_;
    return i < deviceCount() ? i : -1;
}

bool EditorCore::updateAvailable() const
{
    const UpdateInfo u = updateCheckResult();
    if (u.state == UpdateState::Available) return true;
    if (u.state == UpdateState::UpToDate) return false;
    return !prefs_.latestTag.empty() && isNewerVersion(prefs_.latestTag, kVersion);
}
void EditorCore::openReleasePage()
{
    const UpdateInfo u = updateCheckResult();
    host_->openUrl(u.pageUrl.empty() ? kReleasesPage : u.pageUrl);
}

int EditorCore::hoverAt(int x, int y) const
{
    if (rGear.contains(x, y)) return kHoverGear;
    if (updateAvailable() && headerUpdateRect().contains(x, y)) return kHoverUpdate;
    if (settings_) {
        for (int row = 0; row < kRowUpdates; ++row)
            for (int i = 0; i < segCount(row); ++i) if (segRect(row, i).contains(x, y)) return segHover(row, i);
        if (checkRect().contains(x, y)) return kHoverCheck;
        if (autoRect().contains(x, y)) return kHoverAuto;
        if (updateAvailable() && updateLinkRect().contains(x, y)) return kHoverUpdate;
        return kHoverNone;
    }
    if (listOpen_) { const int r = listRowAt(x, y); return r >= 0 ? r : kHoverNone; }
    if (rButton.contains(x, y)) return kHoverButton;
    if (rOutput.contains(x, y)) return kHoverOutput;
    if (faderHit().contains(x, y)) return kHoverFader;
    return kHoverNone;
}

// ---------------------------------------------------------------- input
void EditorCore::mouseDown(int x, int y, bool shift, bool dbl)
{
    if (dbl && !settings_ && !listOpen_ && faderHit().contains(x, y)) {
        ctl_->editParam(kParamOutputLevel, (0.0 - kMinDb) / (kMaxDb - kMinDb));   // 0 dB
        dirty();
        return;
    }
    (void)shift;
    if (rGear.contains(x, y)) { settings_ = !settings_; listOpen_ = false; dirty(); return; }
    if (updateAvailable() && headerUpdateRect().contains(x, y)) { openReleasePage(); return; }
    if (settings_) { settingsDown(x, y); return; }
    if (listOpen_) {
        const int row = listRowAt(x, y);
        if (row >= 0) selectDevice(row);
        listOpen_ = false; hover_ = kHoverNone; dirty();
        return;
    }
    if (rButton.contains(x, y)) {
        const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
        ctl_->editParam(kParamStreamEnable, on ? 0.0 : 1.0);
        dirty();
        return;
    }
    if (rOutput.contains(x, y)) { openList(); return; }
    if (faderHit().contains(x, y)) {
        dragging_ = true;
        host_->captureMouse(true);
        ctl_->beginEdit(kParamOutputLevel);
        setLevelFromX(x, false);
    }
}

void EditorCore::settingsDown(int x, int y)
{
    for (int row = 0; row < kRowUpdates; ++row)
        for (int i = 0; i < segCount(row); ++i) {
            if (!segRect(row, i).contains(x, y)) continue;
            switch (row) {
            case kRowLatency: ctl_->editParam(kParamLatency, latencyNorm(i)); break;
            case kRowChannels: ctl_->editParam(kParamChannels, channelNorm(i)); break;
            case kRowAccent: prefs_.accent = i; savePrefs(); host_->layersChanged(); break;
            case kRowSize:
                if (prefs_.scale != i) { prefs_.scale = i; savePrefs(); host_->layersChanged(); }
                break;
            }
            dirty();
            return;
        }
    if (checkRect().contains(x, y)) { updateCheckStart(); prefs_.lastCheck = (long long)std::time(nullptr); savePrefs(); dirty(); return; }
    if (autoRect().contains(x, y)) { prefs_.autoUpdate = !prefs_.autoUpdate; savePrefs(); dirty(); return; }
    if (updateAvailable() && updateLinkRect().contains(x, y)) openReleasePage();
}

void EditorCore::mouseMove(int x, int y, bool shift)
{
    if (dragging_) { setLevelFromX(x, shift); return; }
    const int hv = hoverAt(x, y);
    if (hv != hover_) { hover_ = hv; dirty(); }
}
void EditorCore::mouseUp()
{
    if (dragging_) { dragging_ = false; host_->captureMouse(false); ctl_->endEdit(kParamOutputLevel); dirty(); }
}
void EditorCore::captureLost() { if (dragging_) { dragging_ = false; ctl_->endEdit(kParamOutputLevel); dirty(); } }
void EditorCore::mouseLeave() { if (hover_ != kHoverNone) { hover_ = kHoverNone; dirty(); } }
void EditorCore::escape() { if (listOpen_ || settings_) { listOpen_ = false; settings_ = false; dirty(); } }
void EditorCore::wheel(int x, int y, int steps, bool shift)
{
    if (settings_) return;
    if (listOpen_) { listScroll_ = std::clamp(listScroll_ - steps, 0, maxScroll()); dirty(); }
    else if (rOutput.contains(x, y)) stepDevice(-steps);
    else nudgeLevel(steps * (shift ? 0.1 : 0.5));
}

void EditorCore::setLevelFromX(int x, bool fine)
{
    double v;
    if (fine) v = std::clamp(lastDragV_ + (x - lastDragX_) / 2000.0, 0.0, 1.0);
    else v = std::clamp(double(x - rFader.l) / double(rFader.r - rFader.l), 0.0, 1.0);
    lastDragX_ = x; lastDragV_ = v;
    ctl_->setParamNormalized(kParamOutputLevel, v);
    ctl_->performEdit(kParamOutputLevel, v);
    dirty();
}
void EditorCore::nudgeLevel(double db)
{
    const double v = std::clamp(ctl_->getParamNormalized(kParamOutputLevel) + db / (kMaxDb - kMinDb), 0.0, 1.0);
    ctl_->editParam(kParamOutputLevel, v);
    dirty();
}

// ---------------------------------------------------------------- tick
void EditorCore::tick()
{
    StreamStats st;
    live_ = ctl_->liveStats(st);
    const double gainDb = dbOf(live_ ? ctl_->streamGain() : 1.f);
    const double dt = kFrameMs / 1000.0;
    double outMax = -120.0;
    for (int ch = 0; ch < 2; ++ch) {   // signal sent to the stream (post level): instant attack, 24 dB/s release
        const double db = live_ ? dbOf(ctl_->takeInputPeak(ch)) + gainDb : -120.0;
        meter_[ch] = std::max(db, meter_[ch] - 24.0 * dt);
        holdAge_[ch] += dt;
        if (db >= hold_[ch] || holdAge_[ch] > 1.2) { hold_[ch] = db; holdAge_[ch] = 0; }
        outMax = std::max(outMax, db);
    }
    peakAge_ += dt;
    if (outMax >= peak_ || peakAge_ > 1.5) { peak_ = outMax; peakAge_ = 0; }

    const UpdateInfo u = updateCheckResult();
    if (u.state == UpdateState::Available && prefs_.latestTag != u.latestTag) { prefs_.latestTag = u.latestTag; savePrefs(); }
    else if (u.state == UpdateState::UpToDate && !prefs_.latestTag.empty()) { prefs_.latestTag.clear(); savePrefs(); }

    Shown s;
    s.on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
    s.dev = deviceIndex();
    s.status = live_ ? int(st.status) : int(std::lround(ctl_->getParamNormalized(kParamStatus) * 3.0));
    s.levelPos = int(std::lround(ctl_->getParamNormalized(kParamOutputLevel) * 660));
    s.hover = hover_; s.list = listOpen_; s.scroll = listScroll_;
    for (int ch = 0; ch < 2; ++ch) {
        s.lit[ch] = int(std::floor(meterPos(meter_[ch]) * kDots + 0.5));
        s.hold[ch] = hold_[ch] > -48.0 ? std::clamp(int(std::floor(meterPos(hold_[ch]) * kDots + 0.5)) - 1, 0, kDots - 1) : -1;
    }
    s.peak10 = int(std::lround(std::max(peak_, -99.0) * 10));
    if (live_) { s.rateHz = int(st.deviceRate); s.latMs = int(std::lround(st.latencyMs)); s.drops = st.underruns; }
    s.settings = settings_;
    s.latency = latencyIndexFromNorm(ctl_->getParamNormalized(kParamLatency));
    s.channels = channelIndexFromNorm(ctl_->getParamNormalized(kParamChannels));
    s.accent = prefs_.accent; s.scale = prefs_.scale; s.autoUpd = prefs_.autoUpdate;
    s.upd = int(u.state); s.updAvail = updateAvailable();
    if (settings_) { s.lit[0] = s.lit[1] = s.hold[0] = s.hold[1] = 0; }
    if (forceFrame_ || !(s == shown_)) { shown_ = s; forceFrame_ = false; host_->repaint(); }
}

// ---------------------------------------------------------------- painting helpers
void EditorCore::hline(Canvas& c, int x0, int x1, int y, Color col) { c.fill(Rect{x0, y, x1, y + 1}, col); }
void EditorCore::groove(Canvas& c, int x0, int x1, int y) { hline(c, x0, x1, y, rgb(9, 9, 10)); hline(c, x0, x1, y + 1, rgb(36, 36, 38)); }
void EditorCore::frame(Canvas& c, const Rect& r, Color col)
{
    hline(c, r.l, r.r, r.t, col); hline(c, r.l, r.r, r.b - 1, col);
    c.fill(Rect{r.l, r.t, r.l + 1, r.b}, col); c.fill(Rect{r.r - 1, r.t, r.r, r.b}, col);
}
void EditorCore::engrave(Canvas& c, const Rect& r, const std::string& t, Color col, Font f, unsigned align, int tracking)
{
    c.text(Rect{r.l, r.t - 1, r.r, r.b - 1}, t, rgb(6, 6, 7), f, align, tracking);      // shadow above
    c.text(Rect{r.l, r.t + 1, r.r, r.b + 1}, t, rgb(46, 46, 48), f, align, tracking);    // highlight below
    c.text(r, t, col, f, align, tracking);
}
void EditorCore::stamp(Canvas& c, const Rect& row, const std::string& t)
{
    engrave(c, Rect{kPad, row.t, kCol - 8, row.b}, t, rgb(150, 150, 148), Font::Label, Left | VCenter, 3);
}
void EditorCore::segment(Canvas& c, const Rect& r, const std::string& t, bool sel, bool hot)
{
    if (sel) c.fill(r, accent());
    else { c.fill(r, hot ? rgb(36, 36, 38) : cPanel); frame(c, r, hot ? cDim : rgb(48, 48, 50)); }
    c.text(r, t, sel ? cOnAccent : (hot ? cInk : rgb(190, 190, 186)), Font::Button, Center | VCenter);
}
Color EditorCore::ledColorAt(int i) const
{
    const double pos = (i + 0.5) / kDots;
    if (pos >= meterPos(0.0)) return cRed;
    if (pos >= meterPos(-6.0)) return cAmber;
    return (prefs_.accent == 2 || prefs_.accent == 5) ? kAccents[0] : accent();   // keep low LEDs distinct from warnings
}
void EditorCore::leds(Canvas& c, bool lit)
{
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kDots; ++i) {
            const float x = float(kMeterX + i * kPitch), y = float(kMeterY0 + ch * kMeterRowGap);
            if (!lit) {
                c.ellipse(x - 0.6f, y - 0.6f, kDot + 1.2f, kDot + 1.2f, rgb(12, 12, 13));   // socket
                c.ellipse(x, y + 0.3f, float(kDot), float(kDot), cDotOff);
            } else {
                c.ellipse(x, y, float(kDot), float(kDot), ledColorAt(i));
            }
        }
}
// Cut into the panel: shadow lip, machined rim, face; the S gets a white face.
void EditorCore::wordmark(Canvas& c)
{
    const float wx = float(kPad + 4), wy = 15.f, ws = 1.0f, st = 7.5f;
    struct Pass { int letters; Color face, faceLit; };
    const Pass passes[2] = {{kLetterA | kLetterB, rgb(13, 13, 14), rgb(17, 17, 18)}, {kLetterS, rgb(214, 214, 210), rgb(240, 240, 236)}};
    for (const Pass& p : passes) {
        const auto figs = wordmarkFigures(wx, wy, ws, p.letters);
        c.strokes(figs, st + 3.4f, rgb(255, 255, 255), 70, 0, 1.2f);   // lower lip catches light
        c.strokes(figs, st + 3.4f, rgb(6, 6, 7), 255, 0, -1.0f);       // upper lip in shadow
        c.strokes(figs, st + 2.2f, rgb(74, 74, 78), 255);              // machined rim
        c.strokes(figs, st, p.face, 255);                              // face
        c.strokes(figs, st - 2.6f, p.faceLit, 255, 0, 1.4f);           // inset shading
    }
}

// ---------------------------------------------------------------- layers
void EditorCore::paintPanel(Canvas& c)
{
    c.fill(Rect{0, 0, kW, kH}, cBg);
    c.perforation(4, cPerf);
    frame(c, Rect{0, 0, kW, kH}, rgb(10, 10, 11));
    c.beginSmooth();
    wordmark(c);
    leds(c, false);
    c.endSmooth();
    engrave(c, Rect{kPad + 112, 16, kW, 56}, "by maai", cDim, Font::Brand, Left | Bottom, 1);
    groove(c, 0, kW, rHeader.b);
}
void EditorCore::paintLit(Canvas& c)
{
    c.beginSmooth();
    leds(c, true);
    c.endSmooth();
}

// ---------------------------------------------------------------- frame
void EditorCore::paintFrame(Canvas& c)
{
    c.drawLayer(Layer::Panel);
    paintHeader(c);
    if (settings_) paintSettings(c);
    else {
        paintMain(c);
        paintFooter(c);
        if (listOpen_) paintList(c);
    }
}

void EditorCore::paintHeader(Canvas& c)
{
    if (shown_.updAvail == 1) {
        const Rect r = headerUpdateRect();
        c.fill(r, shown_.hover == kHoverUpdate ? lighten(accent(), 20) : accent());
        c.text(r, "UPDATE", cOnAccent, Font::Label, Center | VCenter, 2);
    }
    engrave(c, Rect{0, 16, rGear.l - 10, 56}, fmt("v%s", kVersion), cFaint, Font::Small, Right | VCenter, 0);
    // settings button: three slider lines with knobs
    const Color col = settings_ ? accent() : (shown_.hover == kHoverGear ? cInk : cDim);
    const int x0 = rGear.l + 3, x1 = rGear.r - 3;
    const int ys[3] = {rGear.t + 5, rGear.t + 11, rGear.t + 17}, ks[3] = {x0 + 12, x0 + 4, x0 + 9};
    for (int i = 0; i < 3; ++i) { hline(c, x0, x1, ys[i], col); c.fill(Rect{ks[i] - 2, ys[i] - 2, ks[i] + 3, ys[i] + 3}, col); }
}

void EditorCore::paintMain(Canvas& c)
{
    const bool on = shown_.on == 1;
    const auto& devs = ctl_->devices();
    const std::string devName = (shown_.dev >= 0 && shown_.dev < int(devs.size())) ? devs[size_t(shown_.dev)].name : "Device unavailable";

    stamp(c, rOutput, "OUTPUT"); groove(c, kPad, kRight, rOutput.b);
    stamp(c, rLevel, "LEVEL");   groove(c, kPad, kRight, rLevel.b);
    stamp(c, rSignal, "SIGNAL"); groove(c, 0, kW, rSignal.b);
    for (double db : {-36.0, -24.0, -12.0, -6.0, 0.0}) {        // dB ticks under the LEDs
        const int x = kMeterX + int(std::lround(meterPos(db) * kDots)) * kPitch - (kPitch - kDot) / 2;
        c.fill(Rect{x, kMeterY0 + 2 * kMeterRowGap - 1, x + 1, kMeterY0 + 2 * kMeterRowGap + 2}, cFaint);
    }
    for (int ch = 0; ch < 2; ++ch) {                             // lit LEDs come from the Lit layer
        const int y = kMeterY0 + ch * kMeterRowGap - 1;
        const int n = std::clamp(shown_.lit[ch], 0, kDots);
        if (n > 0) c.copyLayerRect(Layer::Lit, Rect{kMeterX - 1, y, kMeterX - 1 + n * kPitch, y + kDot + 2});
        if (shown_.hold[ch] >= n) {
            const int hx = kMeterX + shown_.hold[ch] * kPitch - 1;
            c.copyLayerRect(Layer::Lit, Rect{hx, y, hx + kDot + 2, y + kDot + 2});
        }
    }
    c.text(Rect{kRight - 40, rSignal.t, kRight, rSignal.b},
           (!live_ || shown_.peak10 <= -480) ? std::string("\xE2\x80\x93") : fmt("%.1f", shown_.peak10 / 10.0),
           shown_.peak10 > 0 && live_ ? cRed : cDim, Font::Num, Right | VCenter);

    if (shown_.hover == kHoverOutput && !listOpen_) c.fill(Rect{kCol - 10, rOutput.t + 8, kRight + 4, rOutput.b - 7}, cPanel);
    c.text(Rect{kCol, rOutput.t, kRight - 22, rOutput.b}, devName, cInk, Font::Value, Left | VCenter | Ellipsis);
    {
        const int cx = kRight - 6, cy = (rOutput.t + rOutput.b) / 2;
        for (int i = 0; i < 4; ++i) hline(c, cx - 4 + i, cx + 4 - i, cy - 1 + i, cDim);
    }

    const double v = shown_.levelPos / 660.0;
    const int cy = (rFader.t + rFader.b) / 2;
    const int x = rFader.l + int(std::lround(v * (rFader.r - rFader.l)));
    hline(c, rFader.l, rFader.r, cy - 1, rgb(8, 8, 9));
    hline(c, rFader.l, rFader.r, cy, cFaint);
    hline(c, rFader.l, rFader.r, cy + 1, rgb(38, 38, 40));
    c.fill(Rect{rFader.l, cy - 1, x, cy + 1}, on ? accent() : cDim);
    const int zx = rFader.l + int(std::lround(60.0 / 66.0 * (rFader.r - rFader.l)));
    c.fill(Rect{zx, cy - 7, zx + 1, cy - 4}, cDim);
    const bool hot = dragging_ || shown_.hover == kHoverFader;
    c.fill(Rect{x - 4, cy - 9, x + 4, cy + 9}, rgb(10, 10, 11));
    c.fill(Rect{x - 3, cy - 8, x + 3, cy + 8}, hot ? rgb(250, 250, 246) : rgb(206, 206, 202));
    hline(c, x - 3, x + 3, cy, rgb(70, 70, 72));
    c.text(Rect{kRight - 64, rLevel.t, kRight, rLevel.b},
           shown_.levelPos <= 0 ? std::string("-\xE2\x88\x9E dB") : fmt("%+.1f dB", levelDb(v)), cInk, Font::Num, Right | VCenter);
}

void EditorCore::paintFooter(Canvas& c)
{
    const bool on = shown_.on == 1;
    const auto status = static_cast<StreamStatus>(shown_.status < 0 ? 0 : shown_.status);
    Color dc = cFaint; std::string head = "Off", sub;
    switch (status) {
    case StreamStatus::Streaming:
        dc = accent(); head = "Streaming";
        if (live_) sub = fmt("%d Hz  \xC2\xB7  %d ms  \xC2\xB7  %llu drops", shown_.rateHz, shown_.latMs, (unsigned long long)shown_.drops);
        break;
    case StreamStatus::Connecting: dc = cAmber; head = "Connecting"; break;
    case StreamStatus::Error: dc = cRed; head = "Can't open device"; sub = "Retrying every second"; break;
    default:
        if (on) { dc = cAmber; head = "Waiting for audio"; sub = "Play something in your DAW"; }
        else sub = "Your DAW's output is untouched";
        break;
    }
    const int cy = (rButton.t + rButton.b) / 2;
    c.ellipse(float(kPad), float(cy - 12), 9.f, 9.f, dc);
    c.text(Rect{kPad + 16, cy - 19, rButton.l - 12, cy + 2}, head, cInk, Font::Value, Left | VCenter);
    c.text(Rect{kPad + 16, cy + 1, rButton.l - 12, cy + 19}, sub, cDim, Font::Small, Left | VCenter | Ellipsis);

    const bool hot = shown_.hover == kHoverButton;
    if (on) {
        c.fill(rButton, hot ? lighten(accent(), 20) : accent());
        c.text(rButton, "STREAMING", cOnAccent, Font::Button, Center | VCenter, 2);
    } else {
        c.fill(rButton, hot ? rgb(34, 34, 36) : cPanel);
        frame(c, rButton, hot ? cDim : cFaint);
        hline(c, rButton.l + 1, rButton.r - 1, rButton.t + 1, rgb(48, 48, 50));
        c.text(rButton, "START STREAM", cInk, Font::Button, Center | VCenter, 2);
    }
}

void EditorCore::paintList(Canvas& c)
{
    const auto& devs = ctl_->devices();
    const Rect r = listRect();
    c.fill(r, cPanel);
    frame(c, r, cLine);
    const int rows = std::min(deviceCount() - listScroll_, kListMaxRows);
    for (int i = 0; i < rows; ++i) {
        const int di = i + listScroll_;
        const Rect row{r.l + 1, r.t + 4 + i * kListRowH, r.r - 1, r.t + 4 + (i + 1) * kListRowH};
        const bool sel = di == shown_.dev, hov = di == shown_.hover;
        if (hov) c.fill(row, rgb(38, 38, 40));
        if (sel) c.fill(Rect{row.l, row.t + 6, row.l + 2, row.b - 6}, accent());
        c.text(Rect{row.l + 12, row.t, row.r - 12, row.b}, devs[size_t(di)].name, sel ? cInk : rgb(196, 196, 192),
               Font::Value, Left | VCenter | Ellipsis);
    }
    if (maxScroll() > 0) {
        const int h = r.h() - 8, th = h * kListMaxRows / deviceCount();
        const int ty = r.t + 4 + (h - th) * listScroll_ / maxScroll();
        c.fill(Rect{r.r - 4, ty, r.r - 2, ty + th}, cDim);
    }
}

void EditorCore::paintSettings(Canvas& c)
{
    c.fill(Rect{1, rHeader.b + 2, kW - 1, kH - 1}, cBg);           // covers the panel's LED sockets
    static const char* lat[kLatencyCount] = {"10 ms", "20 ms", "30 ms", "60 ms", "120 ms"};
    static const char* chn[kChannelCount] = {"STEREO", "MONO", "SWAP", "LEFT", "RIGHT"};
    static const char* sz[kScaleCount] = {"100%", "125%", "150%", "200%"};
    static const char* labels[kSetRows] = {"LATENCY", "CHANNELS", "ACCENT", "SIZE", "UPDATES"};
    for (int row = 0; row < kSetRows; ++row) {
        stamp(c, setRow(row), labels[row]);
        if (row < kSetRows - 1) groove(c, kPad, kRight, setRow(row).b);
    }
    for (int i = 0; i < kLatencyCount; ++i) segment(c, segRect(kRowLatency, i), lat[i], shown_.latency == i, shown_.hover == segHover(kRowLatency, i));
    for (int i = 0; i < kChannelCount; ++i) segment(c, segRect(kRowChannels, i), chn[i], shown_.channels == i, shown_.hover == segHover(kRowChannels, i));
    for (int i = 0; i < kAccentCount; ++i) {
        const Rect r = segRect(kRowAccent, i);
        const bool sel = shown_.accent == i, hot = shown_.hover == segHover(kRowAccent, i);
        if (sel || hot) frame(c, Rect{r.l - 3, r.t - 3, r.r + 3, r.b + 3}, sel ? cInk : cDim);
        c.fill(r, kAccents[i]);
    }
    c.text(Rect{kCol + kAccentCount * 30 + 6, setRow(kRowAccent).t, kRight, setRow(kRowAccent).b}, kAccentNames[prefs_.accent],
           cDim, Font::Small, Left | VCenter);
    for (int i = 0; i < kScaleCount; ++i) segment(c, segRect(kRowSize, i), sz[i], shown_.scale == i, shown_.hover == segHover(kRowSize, i));

    const UpdateInfo u = updateCheckResult();
    std::string msg; Color col = cDim;
    if (u.state == UpdateState::Checking) msg = "Checking\xE2\x80\xA6";
    else if (shown_.updAvail == 1) {
        msg = (u.state == UpdateState::Available ? u.latestTag : prefs_.latestTag) + " available \xE2\x80\x94 download";
        col = shown_.hover == kHoverUpdate ? lighten(accent(), 30) : accent();
    }
    else if (u.state == UpdateState::UpToDate) msg = "Up to date";
    else if (u.state == UpdateState::Failed) msg = "Couldn't reach GitHub";
    else msg = prefs_.autoUpdate ? "Checks once a day" : "Automatic checks off";
    c.text(updateLinkRect(), msg, col, Font::Value, Left | VCenter | Ellipsis);
    segment(c, autoRect(), prefs_.autoUpdate ? "AUTO" : "MANUAL", prefs_.autoUpdate, shown_.hover == kHoverAuto);
    segment(c, checkRect(), "CHECK", false, shown_.hover == kHoverCheck);
}

} // namespace asb::ui
