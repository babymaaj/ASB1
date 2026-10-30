// ASB plugin window. Industrial, quiet: flat panel with a fine perforation texture, an "ASB" wordmark built from
// rounded strokes and cut into the panel, stamped/engraved caps labels, LED-dot meters, one accent colour.
//
// CPU: everything static (texture, wordmark, labels, unlit LED dots, hairlines) and the "all lit" LED layer are
// rendered ONCE (GDI+ for anti-aliasing) into cached bitmaps. Per frame: one blit of the panel, a blit of the lit
// part of each meter row, and plain GDI for the few moving things. 30 Hz timer only while open, and a frame is
// drawn only when something visible changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "vst/Editor.h"
#include "vst/Controller.h"
#include "vst/Ids.h"
#include "core/Utf8.h"
#include "core/Version.h"
#include "vst/Update.h"
#include <shellapi.h>
#include <ctime>
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace G = Gdiplus;

namespace asb {

// Paint-time statistics (editor_preview prints them; used for CPU checks).
static std::atomic<uint64_t> g_paintCount{0}, g_paintNs{0};
double editorAvgPaintUs() { const uint64_t c = g_paintCount.load(); return c ? g_paintNs.load() / 1000.0 / double(c) : 0.0; }
uint64_t editorPaintCount() { return g_paintCount.load(); }

namespace {

constexpr int kW = 460, kH = 286;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kFrameMs = 33;

// layout
constexpr int kPad = 24, kCol = 118, kRight = kW - kPad;
const RECT rHeader{0, 0, kW, 72};
const RECT rOutput{0, 73, kW, 119};
const RECT rLevel{0, 120, kW, 166};
const RECT rSignal{0, 167, kW, 213};
const RECT rFader{kCol, 136, kRight - 70, 150};
const RECT rButton{kRight - 158, 228, kRight, 266};
constexpr int kListRowH = 28, kListMaxRows = 7;

// LED meter: two rows of dots
constexpr int kDot = 5, kPitch = 7;
constexpr int kMeterX = kCol, kMeterSpan = (kRight - 44) - kCol;
constexpr int kDots = (kMeterSpan + (kPitch - kDot)) / kPitch;
constexpr int kMeterY0 = 181, kMeterRowGap = 10;

// palette
const COLORREF cBg     = RGB(20, 20, 21);
const COLORREF cPerf   = RGB(25, 25, 26);              // perforation dots
const COLORREF cLine   = RGB(42, 42, 44);
const COLORREF cInk    = RGB(232, 232, 228);
const COLORREF cDim    = RGB(128, 128, 126);
const COLORREF cFaint  = RGB(66, 66, 68);
const COLORREF cAmber  = RGB(226, 170, 72);
const COLORREF cRed    = RGB(226, 84, 70);
const COLORREF cPanel  = RGB(28, 28, 29);
const COLORREF cDotOff = RGB(36, 36, 38);

HINSTANCE moduleInstance()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleInstance), &m);
    return m;
}
std::wstring wide(const std::string& utf8) { const std::u16string u = utf8ToUtf16(utf8); return std::wstring(u.begin(), u.end()); }
double levelDb(double norm) { return kMinDb + norm * (kMaxDb - kMinDb); }
double dbOf(float lin) { return lin > 1e-6f ? 20.0 * std::log10(double(lin)) : -120.0; }
bool inRect(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
double meterPos(double db) { return std::clamp((db + 48.0) / 51.0, 0.0, 1.0); }   // -48..+3 dB -> 0..1
G::Color gc(COLORREF c, BYTE a = 255) { return G::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }


// GDI+ lifetime: one startup per process while any editor is open (only used to render the cached layers).
std::atomic<int> g_gdipRefs{0};
ULONG_PTR g_gdipToken = 0;
void gdipAcquire() { if (g_gdipRefs.fetch_add(1) == 0) { G::GdiplusStartupInput in; G::GdiplusStartup(&g_gdipToken, &in, nullptr); } }
void gdipRelease() { if (g_gdipRefs.fetch_sub(1) == 1) G::GdiplusShutdown(g_gdipToken); }

// ---------------------------------------------------------------- wordmark
// "ASB" built from rounded strokes on a 24 x 40 unit grid per letter (centre lines; drawn with a thick round pen).
// The A's crossbar floats free of the legs, as a small signature detail.
enum : int { kLetterA = 1, kLetterS = 2, kLetterB = 4 };
void wordmarkStrokes(G::GraphicsPath& p, G::REAL x, G::REAL y, G::REAL s, int letters)
{
    auto L = [&](G::REAL x0, G::REAL y0, G::REAL x1, G::REAL y1) { p.AddLine(x + x0 * s, y + y0 * s, x + x1 * s, y + y1 * s); };
    auto A = [&](G::REAL cx, G::REAL cy, G::REAL r, G::REAL start, G::REAL sweep) {
        p.AddArc(x + (cx - r) * s, y + (cy - r) * s, 2 * r * s, 2 * r * s, start, sweep);
    };
    const G::REAL r = 8;
    // A (x 0..24)
    if (letters & kLetterA) {
    p.StartFigure(); L(0, 40, 0, 12); A(12, 12, 12, 180, 180); L(24, 12, 24, 40);
    p.StartFigure(); L(7, 27, 17, 27);
    }
    // S (x 34..58)
    const G::REAL sx = 34;
    if (letters & kLetterS) {
    p.StartFigure();
    L(sx + 24, 0, sx + r, 0); A(sx + r, r, r, 270, -90); L(sx, r, sx, 20 - r); A(sx + r, 20 - r, r, 180, -90);
    L(sx + r, 20, sx + 24 - r, 20); A(sx + 24 - r, 20 + r, r, 270, 90); L(sx + 24, 20 + r, sx + 24, 40 - r);
    A(sx + 24 - r, 40 - r, r, 0, 90); L(sx + 24 - r, 40, sx, 40);
    }
    // B (x 68..92)
    const G::REAL bx = 68;
    if (letters & kLetterB) {
    p.StartFigure(); L(bx, 0, bx, 40);
    p.StartFigure(); L(bx, 0, bx + 22 - r, 0); A(bx + 22 - r, r, r, 270, 90); L(bx + 22, r, bx + 22, 20 - r);
    A(bx + 22 - r, 20 - r, r, 0, 90); L(bx + 22 - r, 20, bx, 20);
    p.StartFigure(); L(bx, 20, bx + 24 - r, 20); A(bx + 24 - r, 20 + r, r, 270, 90); L(bx + 24, 20 + r, bx + 24, 40 - r);
    A(bx + 24 - r, 40 - r, r, 0, 90); L(bx + 24 - r, 40, bx, 40);
    }
}

// Draw the strokes "cut into" the panel: shadow lip, machined rim, recessed floor. Overlapping strokes merge because
// each layer is a fill of the same shape at a different pen width.
// face: recessed dark floor by default; the S gets a white face (same rim), as the brand accent.
void drawEngravedWordmark(G::Graphics& g, G::REAL x, G::REAL y, G::REAL s, G::REAL stroke, int letters,
                          G::Color floor = G::Color(255, 13, 13, 14), G::Color floorLit = G::Color(255, 17, 17, 18))
{
    G::GraphicsPath p;
    wordmarkStrokes(p, x, y, s, letters);
    // Stroke = widen to an outline, then FILL it (winding fill merges overlapping strokes; fills are anti-aliased
    // on every GDI+ implementation, wide-pen DrawPath is not on some).
    auto strokePath = [&](G::REAL w, const G::Color& c, G::GraphicsPath* path) {
        G::Pen pn(G::Color(255, 0, 0, 0), w); pn.SetStartCap(G::LineCapRound); pn.SetEndCap(G::LineCapRound); pn.SetLineJoin(G::LineJoinRound);
        std::unique_ptr<G::GraphicsPath> o(path->Clone());
        o->Widen(&pn, nullptr, 0.15f);
        o->SetFillMode(G::FillModeWinding);
        G::SolidBrush b(c);
        g.FillPath(&b, o.get());
    };
    G::Matrix down; down.Translate(0, 1.2f);
    G::GraphicsPath lo; lo.AddPath(&p, FALSE); lo.Transform(&down);
    G::Matrix up; up.Translate(0, -1.0f);
    G::GraphicsPath hi; hi.AddPath(&p, FALSE); hi.Transform(&up);
    strokePath(stroke + 3.4f, G::Color(70, 255, 255, 255), &lo);   // lower lip catches light
    strokePath(stroke + 3.4f, G::Color(255, 6, 6, 7), &hi);        // upper lip in shadow
    strokePath(stroke + 2.2f, G::Color(255, 74, 74, 78), &p);      // machined rim
    strokePath(stroke, floor, &p);                                  // face / recessed floor
    G::Matrix flo; flo.Translate(0, 1.4f);
    G::GraphicsPath inner; inner.AddPath(&p, FALSE); inner.Transform(&flo);
    strokePath(stroke - 2.6f, floorLit, &inner);                    // inset shading
}

// ---------------------------------------------------------------- user preferences (global, not per project)
struct Prefs {
    int accent = 0, scale = 0;
    bool autoUpdate = true;
    long long lastCheck = 0;           // unix seconds of the last automatic update check
    std::wstring latestTag;            // newest release seen (cached so the header can show UPDATE offline)
};
constexpr int kAccentCount = 6;
const wchar_t* const kAccentNames[kAccentCount] = {L"Green", L"White", L"Red", L"Blue", L"Violet", L"Amber"};
const COLORREF kAccents[kAccentCount] = {RGB(150, 205, 120), RGB(236, 236, 230), RGB(232, 92, 78),
                                         RGB(110, 170, 240), RGB(172, 142, 240), RGB(236, 180, 80)};
constexpr int kScaleCount = 4;
const double kScales[kScaleCount] = {1.0, 1.25, 1.5, 2.0};

std::wstring prefsPath(bool create)
{
    wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(buf) + L"\\maai";
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\ASB";
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}
Prefs loadPrefs()
{
    Prefs p; const std::wstring f = prefsPath(false);
    if (f.empty()) return p;
    p.accent = std::clamp(int(GetPrivateProfileIntW(L"ui", L"accent", 0, f.c_str())), 0, kAccentCount - 1);
    p.scale = std::clamp(int(GetPrivateProfileIntW(L"ui", L"scale", 0, f.c_str())), 0, kScaleCount - 1);
    p.autoUpdate = GetPrivateProfileIntW(L"updates", L"auto", 1, f.c_str()) != 0;
    wchar_t t[64] = L""; GetPrivateProfileStringW(L"updates", L"lastCheck", L"0", t, 64, f.c_str()); p.lastCheck = _wtoi64(t);
    wchar_t v[64] = L""; GetPrivateProfileStringW(L"updates", L"latest", L"", v, 64, f.c_str()); p.latestTag = v;
    return p;
}
void savePrefs(const Prefs& p)
{
    const std::wstring f = prefsPath(true);
    if (f.empty()) return;
    WritePrivateProfileStringW(L"ui", L"accent", std::to_wstring(p.accent).c_str(), f.c_str());
    WritePrivateProfileStringW(L"ui", L"scale", std::to_wstring(p.scale).c_str(), f.c_str());
    WritePrivateProfileStringW(L"updates", L"auto", p.autoUpdate ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"updates", L"lastCheck", std::to_wstring(p.lastCheck).c_str(), f.c_str());
    WritePrivateProfileStringW(L"updates", L"latest", p.latestTag.c_str(), f.c_str());
}
std::string narrow(const std::wstring& w) { std::string s; for (wchar_t c : w) s += char(c < 128 ? c : '?'); return s; }

// settings page rows (logical coordinates)
constexpr int kSetTop = 73, kSetRowH = 42;
RECT setRow(int i) { return RECT{0, kSetTop + i * kSetRowH, kW, kSetTop + (i + 1) * kSetRowH}; }
enum SetRow { kRowLatency = 0, kRowChannels, kRowAccent, kRowSize, kRowUpdates, kSetRows };
const RECT rGear{kRight - 22, 24, kRight, 46};

class Editor final : public EditorView {
public:
    explicit Editor(Controller* c) : EditorView(c, nullptr), ctl_(c), prefs_(loadPrefs())
    {
        applyPrefs();
        ViewRect r(0, 0, W_, H_); setRect(r);
        wchar_t page[16] = L"";                     // screenshot tooling (CI): open on the settings page
        if (GetEnvironmentVariableW(L"ASB_PREVIEW_PAGE", page, 16) && wcscmp(page, L"settings") == 0) settings_ = true;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override
    {
        return (type && std::strcmp(type, kPlatformTypeHWND) == 0) ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override
    {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent) return kResultFalse;
        gdipAcquire();
        registerClass();
        hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, W_, H_,
                                static_cast<HWND>(parent), nullptr, moduleInstance(), this);
        if (!hwnd_) { gdipRelease(); return kResultFalse; }
        return EditorView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override
    {
        if (hwnd_) { KillTimer(hwnd_, kTimer); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        freeResources();
        gdipRelease();
        return EditorView::removed();
    }
    tresult PLUGIN_API onWheel(float distance) override { nudgeLevel(distance > 0 ? 0.5 : -0.5); return kResultTrue; }

private:
    static constexpr const wchar_t* kClassName = L"ASBPanel";

    void applyPrefs()
    {
        s_ = kScales[prefs_.scale];
        W_ = int(std::lround(kW * s_)); H_ = int(std::lround(kH * s_));
        accent_ = kAccents[prefs_.accent];
    }
    // meter low segment follows the accent, unless the accent would collide with the warning colours
    COLORREF ledLow() const { return (prefs_.accent == 2 || prefs_.accent == 5) ? kAccents[0] : accent_; }
    COLORREF ledColorAt(int i) const
    {
        const double pos = (i + 0.5) / kDots;
        if (pos >= meterPos(0.0)) return cRed;
        if (pos >= meterPos(-6.0)) return cAmber;
        return ledLow();
    }
    static COLORREF lighten(COLORREF c, int d) { return RGB(std::min(255, GetRValue(c) + d), std::min(255, GetGValue(c) + d), std::min(255, GetBValue(c) + d)); }

    static void registerClass()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        if (GetClassInfoExW(moduleInstance(), kClassName, &wc)) return;
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &Editor::wndProc;
        wc.hInstance = moduleInstance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    // ---------------------------------------------------------------- input (all hit tests in logical units)
    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        const int x = int(GET_X_LPARAM(lp) / s_), y = int(GET_Y_LPARAM(lp) / s_);
        switch (msg) {
        case WM_CREATE: hwnd_ = h; buildResources(); SetTimer(h, kTimer, kFrameMs, nullptr); maybeAutoCheck(); return 0;
        case WM_TIMER: tick(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_LBUTTONDOWN: SetFocus(h); onDown(x, y); return 0;
        case WM_LBUTTONDBLCLK:
            if (!settings_ && !listOpen_ && inRect(faderHit(), x, y)) { ctl_->editParam(kParamOutputLevel, (0.0 - kMinDb) / (kMaxDb - kMinDb)); dirty(); }
            else onDown(x, y);
            return 0;
        case WM_MOUSEMOVE: {
            if (dragging_) { setLevelFromX(x, (wp & MK_SHIFT) != 0); return 0; }
            const int hv = hoverAt(x, y);
            if (hv != hover_) { hover_ = hv; dirty(); }
            if (!tracking_) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); tracking_ = true; }
            return 0;
        }
        case WM_MOUSELEAVE: tracking_ = false; if (hover_ != kHoverNone) { hover_ = kHoverNone; dirty(); } return 0;
        case WM_LBUTTONUP:
            if (dragging_) { dragging_ = false; ReleaseCapture(); ctl_->endEdit(kParamOutputLevel); dirty(); }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_ && reinterpret_cast<HWND>(lp) != h) { dragging_ = false; ctl_->endEdit(kParamOutputLevel); }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &p);
            p.x = LONG(p.x / s_); p.y = LONG(p.y / s_);
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (settings_) return 0;
            if (listOpen_) { listScroll_ = std::clamp(listScroll_ - steps, 0, maxScroll()); dirty(); }
            else if (inRect(rOutput, p.x, p.y)) stepDevice(-steps);
            else nudgeLevel(steps * ((GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? 0.1 : 0.5));
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && (listOpen_ || settings_)) { listOpen_ = false; settings_ = false; dirty(); return 0; }
            break;
        case WM_DESTROY: KillTimer(h, kTimer); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    static constexpr int kHoverNone = -100, kHoverOutput = -10, kHoverButton = -11, kHoverFader = -12, kHoverGear = -13,
                         kHoverUpdate = -14, kHoverCheck = -15, kHoverAuto = -16;
    static int segHover(int row, int idx) { return 1000 + row * 16 + idx; }
    RECT faderHit() const { return RECT{rFader.left - 8, rFader.top - 8, rFader.right + 8, rFader.bottom + 8}; }

    // segmented control geometry for settings rows
    static int segCount(int row)
    {
        switch (row) { case kRowLatency: return kLatencyCount; case kRowChannels: return kChannelCount;
                       case kRowAccent: return kAccentCount; case kRowSize: return kScaleCount; default: return 0; }
    }
    static RECT segRect(int row, int i)
    {
        const RECT r = setRow(row);
        const int n = segCount(row);
        if (row == kRowAccent) {                    // swatches
            const int x = kCol + i * 30;
            return RECT{x, r.top + 12, x + 20, r.top + 32};
        }
        const int x0 = kCol, x1 = kRight, gap = 4;
        const int w = (x1 - x0 - gap * (n - 1)) / n;
        return RECT{x0 + i * (w + gap), r.top + 9, x0 + i * (w + gap) + w, r.bottom - 9};
    }
    RECT updateLinkRect() const { const RECT r = setRow(kRowUpdates); return RECT{kCol, r.top + 6, kRight - 150, r.bottom - 6}; }
    RECT checkRect() const { const RECT r = setRow(kRowUpdates); return RECT{kRight - 66, r.top + 9, kRight, r.bottom - 9}; }
    RECT autoRect() const { const RECT r = setRow(kRowUpdates); return RECT{kRight - 140, r.top + 9, kRight - 72, r.bottom - 9}; }
    RECT headerUpdateRect() const { return RECT{kRight - 170, 26, kRight - 112, 44}; }

    bool updateAvailable() const
    {
        const UpdateInfo u = updateCheckResult();
        if (u.state == UpdateState::Available) return true;
        if (u.state == UpdateState::UpToDate) return false;
        return !prefs_.latestTag.empty() && isNewerVersion(narrow(prefs_.latestTag), kVersion);
    }

    int hoverAt(int x, int y) const
    {
        if (inRect(rGear, x, y)) return kHoverGear;
        if (updateAvailable() && inRect(headerUpdateRect(), x, y)) return kHoverUpdate;
        if (settings_) {
            for (int row = 0; row < kRowUpdates; ++row)
                for (int i = 0; i < segCount(row); ++i) if (inRect(segRect(row, i), x, y)) return segHover(row, i);
            if (inRect(checkRect(), x, y)) return kHoverCheck;
            if (inRect(autoRect(), x, y)) return kHoverAuto;
            if (updateAvailable() && inRect(updateLinkRect(), x, y)) return kHoverUpdate;
            return kHoverNone;
        }
        if (listOpen_) { const int r = listRowAt(x, y); return r >= 0 ? r : kHoverNone; }
        if (inRect(rButton, x, y)) return kHoverButton;
        if (inRect(rOutput, x, y)) return kHoverOutput;
        if (inRect(faderHit(), x, y)) return kHoverFader;
        return kHoverNone;
    }

    void openReleasePage()
    {
        const UpdateInfo u = updateCheckResult();
        const std::string url = u.pageUrl.empty() ? kReleasesPage : u.pageUrl;
        ShellExecuteW(nullptr, L"open", std::wstring(url.begin(), url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void onDown(int x, int y)
    {
        if (inRect(rGear, x, y)) { settings_ = !settings_; listOpen_ = false; dirty(); return; }
        if (updateAvailable() && inRect(headerUpdateRect(), x, y)) { openReleasePage(); return; }
        if (settings_) { onSettingsDown(x, y); return; }
        if (listOpen_) {
            const int row = listRowAt(x, y);
            if (row >= 0) selectDevice(row);
            listOpen_ = false; hover_ = kHoverNone; dirty();
            return;
        }
        if (inRect(rButton, x, y)) {
            const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
            ctl_->editParam(kParamStreamEnable, on ? 0.0 : 1.0);
            dirty();
            return;
        }
        if (inRect(rOutput, x, y)) { openList(); return; }
        if (inRect(faderHit(), x, y)) {
            dragging_ = true;
            SetCapture(hwnd_);
            ctl_->beginEdit(kParamOutputLevel);
            setLevelFromX(x, false);
        }
    }

    void onSettingsDown(int x, int y)
    {
        for (int row = 0; row < kRowUpdates; ++row)
            for (int i = 0; i < segCount(row); ++i) {
                if (!inRect(segRect(row, i), x, y)) continue;
                switch (row) {
                case kRowLatency: ctl_->editParam(kParamLatency, latencyNorm(i)); break;
                case kRowChannels: ctl_->editParam(kParamChannels, channelNorm(i)); break;
                case kRowAccent: prefs_.accent = i; savePrefs(prefs_); applyPrefs(); rebuild(false); break;
                case kRowSize:
                    if (prefs_.scale != i) { prefs_.scale = i; savePrefs(prefs_); applyPrefs(); rebuild(true); }
                    break;
                }
                dirty();
                return;
            }
        if (inRect(checkRect(), x, y)) { updateCheckStart(); prefs_.lastCheck = (long long)time(nullptr); savePrefs(prefs_); dirty(); return; }
        if (inRect(autoRect(), x, y)) { prefs_.autoUpdate = !prefs_.autoUpdate; savePrefs(prefs_); dirty(); return; }
        if (updateAvailable() && inRect(updateLinkRect(), x, y)) openReleasePage();
    }

    void maybeAutoCheck()
    {
        const long long now = (long long)time(nullptr);
        if (prefs_.autoUpdate && now - prefs_.lastCheck > 24 * 3600) {
            updateCheckStart();
            prefs_.lastCheck = now;
            savePrefs(prefs_);
        }
    }

    // re-render cached layers (accent change) and, if the size changed, resize the window and tell the host
    void rebuild(bool resized)
    {
        freeResources();
        buildResources();
        if (resized && hwnd_) {
            SetWindowPos(hwnd_, nullptr, 0, 0, W_, H_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ViewRect r(0, 0, W_, H_);
            if (plugFrame) plugFrame->resizeView(this, &r);
            else setRect(r);
        }
    }

    void setLevelFromX(int x, bool fine)
    {
        double v;
        if (fine) v = std::clamp(lastDragV_ + (x - lastDragX_) / 2000.0, 0.0, 1.0);   // shift = fine
        else v = std::clamp(double(x - rFader.left) / double(rFader.right - rFader.left), 0.0, 1.0);
        lastDragX_ = x; lastDragV_ = v;
        ctl_->setParamNormalized(kParamOutputLevel, v);
        ctl_->performEdit(kParamOutputLevel, v);
        dirty();
    }
    void nudgeLevel(double db)
    {
        const double v = std::clamp(ctl_->getParamNormalized(kParamOutputLevel) + db / (kMaxDb - kMinDb), 0.0, 1.0);
        ctl_->editParam(kParamOutputLevel, v);
        dirty();
    }

    int deviceIndex() const { return int(ctl_->getParamNormalized(kParamDevice) * (kMaxDevices - 1) + 0.5); }
    int deviceCount() const { return int(ctl_->devices().size()); }
    void selectDevice(int i) { if (i >= 0 && i < deviceCount()) { ctl_->editParam(kParamDevice, double(i) / double(kMaxDevices - 1)); dirty(); } }
    void stepDevice(int d) { const int n = deviceCount(); if (n > 0) selectDevice(((deviceIndex() + d) % n + n) % n); }

    RECT listRect() const
    {
        const int rows = std::min(deviceCount(), kListMaxRows);
        return RECT{kCol - 10, rOutput.bottom - 6, kRight + 4, rOutput.bottom - 6 + rows * kListRowH + 8};
    }
    int maxScroll() const { return std::max(0, deviceCount() - kListMaxRows); }
    void openList()
    {
        listOpen_ = true;
        listScroll_ = std::clamp(deviceIndex() - kListMaxRows / 2, 0, maxScroll());
        hover_ = kHoverNone;
        dirty();
    }
    int listRowAt(int x, int y) const
    {
        const RECT r = listRect();
        if (!inRect(r, x, y)) return -1;
        const int row = (y - r.top - 4) / kListRowH;
        if (row < 0 || row >= kListMaxRows) return -1;
        const int i = row + listScroll_;
        return i < deviceCount() ? i : -1;
    }

    // ---------------------------------------------------------------- state
    struct Shown {
        int on = -1, dev = -1, status = -1, levelPos = -1, hover = -1000, list = -1, scroll = -1;
        int lit[2]{-1, -1}, hold[2]{-1, -1}, peak10 = 0, rateHz = 0, latMs = -1;
        int settings = -1, latency = -1, channels = -1, accent = -1, scale = -1, upd = -1, updAvail = -1, autoUpd = -1;
        uint64_t drops = ~0ull;
        bool operator==(const Shown& o) const { return std::memcmp(this, &o, sizeof(Shown)) == 0; }
    };
    void dirty() { forceFrame_ = true; tick(); }

    void tick()
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

        // remember the newest release we've seen (so the header can show UPDATE on later sessions)
        const UpdateInfo u = updateCheckResult();
        if (u.state == UpdateState::Available && narrow(prefs_.latestTag) != u.latestTag) {
            prefs_.latestTag = std::wstring(u.latestTag.begin(), u.latestTag.end()); savePrefs(prefs_);
        } else if (u.state == UpdateState::UpToDate && !prefs_.latestTag.empty()) {
            prefs_.latestTag.clear(); savePrefs(prefs_);
        }

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
        if (settings_) { s.lit[0] = s.lit[1] = s.hold[0] = s.hold[1] = 0; }   // meters hidden: don't repaint for them
        if (forceFrame_ || !(s == shown_)) { shown_ = s; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    // ---------------------------------------------------------------- cached layers (device-sized, logical drawing)
    void prepDc(HDC dc)
    {
        SetGraphicsMode(dc, GM_ADVANCED);
        XFORM x{float(s_), 0, 0, float(s_), 0, 0};
        SetWorldTransform(dc, &x);
        SetBkMode(dc, TRANSPARENT);
    }
    void makeDib(HBITMAP& bmp, HDC& dc, HGDIOBJ& old, void** bitsOut = nullptr)
    {
        HDC screen = GetDC(hwnd_);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W_, -H_, 1, 32, BI_RGB};
        void* bits = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitsOut) *bitsOut = bits;
        dc = CreateCompatibleDC(screen); old = SelectObject(dc, bmp);
        ReleaseDC(hwnd_, screen);
        prepDc(dc);
    }

    void buildResources()
    {
        makeDib(bgDib_, bgDc_, bgOld_, &bgBits_);
        makeDib(litDib_, litDc_, litOld_, &litBits_);
        makeDib(backDib_, dc_, oldBmp_);
        const bool din = hasFont(L"Bahnschrift");
        auto font = [&](int px, int weight, bool display, bool condensed = false) {
            const wchar_t* face = !display || !din ? L"Segoe UI" : (condensed ? L"Bahnschrift SemiCondensed" : L"Bahnschrift");
            return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
        };
        fLabel_  = font(11, FW_BOLD, true);
        fBrand_  = font(13, FW_NORMAL, true);
        fValue_  = font(15, FW_NORMAL, true);
        fSmall_  = font(12, FW_NORMAL, true);
        fButton_ = font(13, FW_BOLD, true);
        fNum_    = font(14, FW_NORMAL, true, true);
        renderPanel();
    }

    // Anti-aliased drawing onto a GDI DIB: copy the pixels into a GDI+-owned bitmap, draw there (scaled), copy back.
    template <class F> void aaLayer(void* bits, F&& draw)
    {
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        G::Bitmap layer(W_, H_, PixelFormat32bppPARGB);
        G::Rect all(0, 0, W_, H_);
        G::BitmapData d;
        if (layer.LockBits(&all, G::ImageLockModeWrite, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < H_; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d.Scan0) + y * d.Stride);
                for (int x = 0; x < W_; ++x) row[x] = px[y * W_ + x] | 0xFF000000u;
            }
            layer.UnlockBits(&d);
        }
        {
            G::Graphics g(&layer);
            g.SetSmoothingMode(G::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
            g.ScaleTransform(G::REAL(s_), G::REAL(s_));
            draw(g);
        }
        if (layer.LockBits(&all, G::ImageLockModeRead, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < H_; ++y)
                std::memcpy(px + y * W_, static_cast<uint8_t*>(d.Scan0) + y * d.Stride, size_t(W_) * 4);
            layer.UnlockBits(&d);
        }
    }

    // Static panel + lit LED layer. Called when the window opens (and on accent / size change).
    void renderPanel()
    {
        HDC prev = dc_;
        dc_ = bgDc_;
        fill(RECT{0, 0, kW, kH}, cBg);
        {   // fine perforation, in device pixels so it stays crisp at any size
            ModifyWorldTransform(dc_, nullptr, MWT_IDENTITY);
            const int step = std::max(4, int(std::lround(4 * s_)));
            for (int y = 2; y < H_; y += step)
                for (int x = ((y / step) % 2) ? step : step / 2; x < W_; x += step) SetPixelV(dc_, x, y, cPerf);
            prepDc(dc_);
        }
        frame(RECT{0, 0, kW, kH}, RGB(10, 10, 11));

        aaLayer(bgBits_, [&](G::Graphics& g) {
            const G::REAL wx = G::REAL(kPad + 4), wy = 15.f, ws = 1.0f, wst = 7.5f;
            drawEngravedWordmark(g, wx, wy, ws, wst, kLetterA | kLetterB);
            drawEngravedWordmark(g, wx, wy, ws, wst, kLetterS, G::Color(255, 214, 214, 210), G::Color(255, 240, 240, 236));
        });
        engrave(RECT{kPad + 112, 16, kW, 56}, L"by maai", cDim, fBrand_, DT_LEFT | DT_BOTTOM, 1);
        groove(0, kW, rHeader.bottom);

        // main-page grooves and labels are drawn per frame (the settings page replaces them)
        aaLayer(bgBits_, [&](G::Graphics& g) { drawLeds(g, false); });
        GdiFlush();
        BitBlt(litDc_, 0, 0, kW, kH, bgDc_, 0, 0, SRCCOPY);
        aaLayer(litBits_, [&](G::Graphics& g) { drawLeds(g, true); });
        dc_ = prev;
    }

    void drawLeds(G::Graphics& g, bool lit)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kDots; ++i) {
                const G::REAL x = G::REAL(kMeterX + i * kPitch), y = G::REAL(kMeterY0 + ch * kMeterRowGap);
                if (!lit) {
                    G::SolidBrush rim(G::Color(255, 12, 12, 13)); g.FillEllipse(&rim, x - 0.6f, y - 0.6f, kDot + 1.2f, kDot + 1.2f);
                    G::SolidBrush off(gc(cDotOff)); g.FillEllipse(&off, x, y + 0.3f, G::REAL(kDot), G::REAL(kDot));
                } else {
                    G::SolidBrush on(gc(ledColorAt(i))); g.FillEllipse(&on, x, y, G::REAL(kDot), G::REAL(kDot));
                }
            }
    }

    bool hasFont(const wchar_t* face)
    {
        LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET; wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
        bool found = false;
        HDC dc = GetDC(hwnd_);
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *reinterpret_cast<bool*>(p) = true; return 0; },
                            reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(hwnd_, dc);
        return found;
    }
    void freeResources()
    {
        auto freeDib = [](HBITMAP& b, HDC& dc, HGDIOBJ old) {
            if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
            if (b) { DeleteObject(b); b = nullptr; }
        };
        freeDib(bgDib_, bgDc_, bgOld_); freeDib(litDib_, litDc_, litOld_); freeDib(backDib_, dc_, oldBmp_);
        for (HFONT* f : {&fLabel_, &fBrand_, &fValue_, &fSmall_, &fButton_, &fNum_}) if (*f) { DeleteObject(*f); *f = nullptr; }
    }

    // ---------------------------------------------------------------- GDI helpers (operate on dc_, logical units)
    void fill(const RECT& r, COLORREF c) { SetDCBrushColor(dc_, c); FillRect(dc_, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
    void hline(int x0, int x1, int y, COLORREF c) { fill(RECT{x0, y, x1, y + 1}, c); }
    void groove(int x0, int x1, int y) { hline(x0, x1, y, RGB(9, 9, 10)); hline(x0, x1, y + 1, RGB(36, 36, 38)); }
    void frame(const RECT& r, COLORREF c)
    {
        hline(r.left, r.right, r.top, c); hline(r.left, r.right, r.bottom - 1, c);
        fill(RECT{r.left, r.top, r.left + 1, r.bottom}, c); fill(RECT{r.right - 1, r.top, r.right, r.bottom}, c);
    }
    // Text with optional letter-spacing. DrawText's own centring ignores the extra spacing, so with tracking we
    // measure the string ourselves (minus the trailing space after the last letter) and place it exactly.
    void text(RECT r, const wchar_t* t, COLORREF c, HFONT f, UINT fmt, int tracking = 0)
    {
        SetTextColor(dc_, c);
        SetTextCharacterExtra(dc_, tracking);
        HGDIOBJ of = SelectObject(dc_, f);
        if (tracking != 0 && (fmt & (DT_CENTER | DT_RIGHT))) {
            SIZE sz{}; const int len = int(wcslen(t));
            GetTextExtentPoint32W(dc_, t, len, &sz);
            const int w = sz.cx - (len > 0 ? tracking : 0);
            const int left = (fmt & DT_CENTER) ? (r.left + r.right - w) / 2 : r.right - w;
            r.left = left; r.right = left + w + tracking + 2;
            fmt = (fmt & ~UINT(DT_CENTER | DT_RIGHT)) | DT_LEFT | DT_NOCLIP;
        }
        DrawTextW(dc_, t, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc_, of);
        SetTextCharacterExtra(dc_, 0);
    }
    void engrave(RECT r, const wchar_t* t, COLORREF c, HFONT f, UINT fmt, int tracking)
    {
        RECT a = r; OffsetRect(&a, 0, -1); text(a, t, RGB(6, 6, 7), f, fmt, tracking);
        RECT b = r; OffsetRect(&b, 0, 1);  text(b, t, RGB(46, 46, 48), f, fmt, tracking);
        text(r, t, c, f, fmt, tracking);
    }
    void stamp(const RECT& row, const wchar_t* t) { engrave(RECT{kPad, row.top, kCol - 8, row.bottom}, t, RGB(150, 150, 148), fLabel_, DT_LEFT | DT_VCENTER, 3); }
    void dot(int cx, int cy, int r, COLORREF c)
    {
        SetDCBrushColor(dc_, c); SetDCPenColor(dc_, c);
        HGDIOBJ ob = SelectObject(dc_, GetStockObject(DC_BRUSH)), op = SelectObject(dc_, GetStockObject(DC_PEN));
        Ellipse(dc_, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc_, ob); SelectObject(dc_, op);
    }
    // flat segmented button
    void segment(const RECT& r, const wchar_t* t, bool sel, bool hot)
    {
        if (sel) fill(r, accent_);
        else { fill(r, hot ? RGB(36, 36, 38) : cPanel); frame(r, hot ? cDim : RGB(48, 48, 50)); }
        text(r, t, sel ? RGB(16, 18, 14) : (hot ? cInk : RGB(190, 190, 186)), fButton_, DT_CENTER | DT_VCENTER);
    }
    void gearIcon(bool active, bool hot)
    {
        const COLORREF c = active ? accent_ : (hot ? cInk : cDim);
        const int x0 = rGear.left + 3, x1 = rGear.right - 3;
        const int ys[3] = {rGear.top + 5, rGear.top + 11, rGear.top + 17}, ks[3] = {x0 + 12, x0 + 4, x0 + 9};
        for (int i = 0; i < 3; ++i) {      // three slider lines with knobs
            hline(x0, x1, ys[i], c);
            fill(RECT{ks[i] - 2, ys[i] - 2, ks[i] + 3, ys[i] + 3}, c);
        }
    }

    // ---------------------------------------------------------------- frame
    void paint()
    {
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd_, &ps);

        const bool on = shown_.on == 1;
        const auto status = static_cast<StreamStatus>(shown_.status < 0 ? 0 : shown_.status);

        BitBlt(dc_, 0, 0, kW, kH, bgDc_, 0, 0, SRCCOPY);

        // header (dynamic part): update tag, version, settings button
        {
            const bool upd = shown_.updAvail == 1;
            const UpdateInfo u = updateCheckResult();
            if (upd) {
                const RECT r = headerUpdateRect();
                const bool hot = shown_.hover == kHoverUpdate;
                fill(r, hot ? lighten(accent_, 20) : accent_);
                text(r, L"UPDATE", RGB(16, 18, 14), fLabel_, DT_CENTER | DT_VCENTER, 2);
            }
            wchar_t ver[48]; swprintf(ver, 48, L"v%hs", kVersion);
            engrave(RECT{0, 16, rGear.left - 10, 56}, ver, cFaint, fSmall_, DT_RIGHT | DT_VCENTER, 0);
            gearIcon(settings_, shown_.hover == kHoverGear);
            (void)u;
        }

        if (settings_) paintSettings();
        else paintMain(on, status);

        // footer status (main page only)
        if (!settings_) paintFooter(on, status);
        if (!settings_ && listOpen_) paintList();

        ModifyWorldTransform(dc_, nullptr, MWT_IDENTITY);          // device-pixel copy to the window
        BitBlt(wdc, 0, 0, W_, H_, dc_, 0, 0, SRCCOPY);
        prepDc(dc_);
        EndPaint(hwnd_, &ps);
        forceFrame_ = false;
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        g_paintNs.fetch_add(uint64_t((t1.QuadPart - t0.QuadPart) * 1'000'000'000.0 / fq.QuadPart));
        g_paintCount.fetch_add(1);
    }

    void paintMain(bool on, StreamStatus)
    {
        const auto& devs = ctl_->devices();
        const std::wstring devName = (shown_.dev >= 0 && shown_.dev < int(devs.size())) ? wide(devs[size_t(shown_.dev)].name) : L"Device unavailable";

        stamp(rOutput, L"OUTPUT");  groove(kPad, kRight, rOutput.bottom);
        stamp(rLevel, L"LEVEL");    groove(kPad, kRight, rLevel.bottom);
        stamp(rSignal, L"SIGNAL");  groove(0, kW, rSignal.bottom);
        for (double db : {-36.0, -24.0, -12.0, -6.0, 0.0}) {        // dB ticks under the LEDs
            const int x = kMeterX + int(std::lround(meterPos(db) * kDots)) * kPitch - (kPitch - kDot) / 2;
            fill(RECT{x, kMeterY0 + 2 * kMeterRowGap - 1, x + 1, kMeterY0 + 2 * kMeterRowGap + 2}, cFaint);
        }

        // LEDs: copy the lit part of each row from the lit layer, plus the peak-hold LED
        for (int ch = 0; ch < 2; ++ch) {
            const int y = kMeterY0 + ch * kMeterRowGap - 1;
            const int n = std::clamp(shown_.lit[ch], 0, kDots);
            if (n > 0) BitBlt(dc_, kMeterX - 1, y, n * kPitch, kDot + 2, litDc_, kMeterX - 1, y, SRCCOPY);
            if (shown_.hold[ch] >= n) {
                const int hx = kMeterX + shown_.hold[ch] * kPitch - 1;
                BitBlt(dc_, hx, y, kDot + 2, kDot + 2, litDc_, hx, y, SRCCOPY);
            }
        }
        {
            wchar_t pk[16];
            if (!live_ || shown_.peak10 <= -480) swprintf(pk, 16, L"–"); else swprintf(pk, 16, L"%.1f", shown_.peak10 / 10.0);
            text(RECT{kRight - 40, rSignal.top, kRight, rSignal.bottom}, pk, shown_.peak10 > 0 && live_ ? cRed : cDim, fNum_, DT_RIGHT | DT_VCENTER);
        }

        if (shown_.hover == kHoverOutput && !listOpen_) fill(RECT{kCol - 10, rOutput.top + 8, kRight + 4, rOutput.bottom - 7}, cPanel);
        text(RECT{kCol, rOutput.top, kRight - 22, rOutput.bottom}, devName.c_str(), cInk, fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        {
            const int cx = kRight - 6, cy = (rOutput.top + rOutput.bottom) / 2;
            for (int i = 0; i < 4; ++i) hline(cx - 4 + i, cx + 4 - i, cy - 1 + i, cDim);
        }

        {   // LEVEL fader
            const double v = shown_.levelPos / 660.0;
            const int cy = (rFader.top + rFader.bottom) / 2;
            const int x = rFader.left + int(std::lround(v * (rFader.right - rFader.left)));
            hline(rFader.left, rFader.right, cy - 1, RGB(8, 8, 9));
            hline(rFader.left, rFader.right, cy, cFaint);
            hline(rFader.left, rFader.right, cy + 1, RGB(38, 38, 40));
            fill(RECT{rFader.left, cy - 1, x, cy + 1}, on ? accent_ : cDim);
            const int zx = rFader.left + int(std::lround(60.0 / 66.0 * (rFader.right - rFader.left)));
            fill(RECT{zx, cy - 7, zx + 1, cy - 4}, cDim);
            const bool hot = dragging_ || shown_.hover == kHoverFader;
            fill(RECT{x - 4, cy - 9, x + 4, cy + 9}, RGB(10, 10, 11));
            fill(RECT{x - 3, cy - 8, x + 3, cy + 8}, hot ? RGB(250, 250, 246) : RGB(206, 206, 202));
            hline(x - 3, x + 3, cy, RGB(70, 70, 72));
            wchar_t vt[32];
            if (shown_.levelPos <= 0) swprintf(vt, 32, L"-∞ dB"); else swprintf(vt, 32, L"%+.1f dB", levelDb(v));
            text(RECT{kRight - 64, rLevel.top, kRight, rLevel.bottom}, vt, cInk, fNum_, DT_RIGHT | DT_VCENTER);
        }
    }

    void paintFooter(bool on, StreamStatus status)
    {
        COLORREF dc = cFaint; const wchar_t* head = L"Off";
        wchar_t sub[160] = L"";
        switch (status) {
        case StreamStatus::Streaming:
            dc = accent_; head = L"Streaming";
            if (live_) swprintf(sub, 160, L"%d Hz  ·  %d ms  ·  %llu drops", shown_.rateHz, shown_.latMs, (unsigned long long)shown_.drops);
            break;
        case StreamStatus::Connecting: dc = cAmber; head = L"Connecting"; break;
        case StreamStatus::Error: dc = cRed; head = L"Can't open device"; swprintf(sub, 160, L"Retrying every second"); break;
        default:
            if (on) { dc = cAmber; head = L"Waiting for audio"; swprintf(sub, 160, L"Play something in FL"); }
            else swprintf(sub, 160, L"FL output is untouched");
            break;
        }
        const int cy = (rButton.top + rButton.bottom) / 2;       // status block centred on the button
        dot(kPad + 4, cy - 8, 4, dc);
        text(RECT{kPad + 16, cy - 19, rButton.left - 12, cy + 2}, head, cInk, fValue_, DT_LEFT | DT_VCENTER);
        text(RECT{kPad + 16, cy + 1, rButton.left - 12, cy + 19}, sub, cDim, fSmall_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        const bool hot = shown_.hover == kHoverButton;
        if (on) {
            fill(rButton, hot ? lighten(accent_, 20) : accent_);
            text(rButton, L"STREAMING", RGB(16, 18, 14), fButton_, DT_CENTER | DT_VCENTER, 2);
        } else {
            fill(rButton, hot ? RGB(34, 34, 36) : cPanel);
            frame(rButton, hot ? cDim : cFaint);
            hline(rButton.left + 1, rButton.right - 1, rButton.top + 1, RGB(48, 48, 50));
            text(rButton, L"START STREAM", cInk, fButton_, DT_CENTER | DT_VCENTER, 2);
        }
    }

    void paintList()
    {
        const auto& devs = ctl_->devices();
        const RECT r = listRect();
        fill(r, cPanel);
        frame(r, cLine);
        const int rows = std::min(deviceCount() - listScroll_, kListMaxRows);
        for (int i = 0; i < rows; ++i) {
            const int di = i + listScroll_;
            const RECT row{r.left + 1, r.top + 4 + i * kListRowH, r.right - 1, r.top + 4 + (i + 1) * kListRowH};
            const bool sel = di == shown_.dev, hov = di == shown_.hover;
            if (hov) fill(row, RGB(38, 38, 40));
            if (sel) fill(RECT{row.left, row.top + 6, row.left + 2, row.bottom - 6}, accent_);
            text(RECT{row.left + 12, row.top, row.right - 12, row.bottom}, wide(devs[size_t(di)].name).c_str(),
                 sel ? cInk : RGB(196, 196, 192), fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
        if (maxScroll() > 0) {
            const int h = r.bottom - r.top - 8, th = h * kListMaxRows / deviceCount();
            const int ty = r.top + 4 + (h - th) * listScroll_ / maxScroll();
            fill(RECT{r.right - 4, ty, r.right - 2, ty + th}, cDim);
        }
    }

    void paintSettings()
    {
        // the settings page covers the meter area; hide the panel's unlit LEDs under a plain fill
        fill(RECT{1, rHeader.bottom + 2, kW - 1, kH - 1}, cBg);
        static const wchar_t* lat[kLatencyCount] = {L"10 ms", L"20 ms", L"30 ms", L"60 ms", L"120 ms"};
        static const wchar_t* chn[kChannelCount] = {L"STEREO", L"MONO", L"SWAP", L"LEFT", L"RIGHT"};
        static const wchar_t* sz[kScaleCount] = {L"100%", L"125%", L"150%", L"200%"};
        const wchar_t* labels[kSetRows] = {L"LATENCY", L"CHANNELS", L"ACCENT", L"SIZE", L"UPDATES"};
        for (int row = 0; row < kSetRows; ++row) {
            const RECT r = setRow(row);
            stamp(r, labels[row]);
            if (row < kSetRows - 1) groove(kPad, kRight, r.bottom);
        }
        for (int i = 0; i < kLatencyCount; ++i) segment(segRect(kRowLatency, i), lat[i], shown_.latency == i, shown_.hover == segHover(kRowLatency, i));
        for (int i = 0; i < kChannelCount; ++i) segment(segRect(kRowChannels, i), chn[i], shown_.channels == i, shown_.hover == segHover(kRowChannels, i));
        for (int i = 0; i < kAccentCount; ++i) {
            const RECT r = segRect(kRowAccent, i);
            const bool sel = shown_.accent == i, hot = shown_.hover == segHover(kRowAccent, i);
            if (sel || hot) frame(RECT{r.left - 3, r.top - 3, r.right + 3, r.bottom + 3}, sel ? cInk : cDim);
            fill(r, kAccents[i]);
        }
        text(RECT{kCol + kAccentCount * 30 + 6, setRow(kRowAccent).top, kRight, setRow(kRowAccent).bottom},
             kAccentNames[prefs_.accent], cDim, fSmall_, DT_LEFT | DT_VCENTER);
        for (int i = 0; i < kScaleCount; ++i) segment(segRect(kRowSize, i), sz[i], shown_.scale == i, shown_.hover == segHover(kRowSize, i));

        // updates row
        {
            const UpdateInfo u = updateCheckResult();
            wchar_t msg[160];
            COLORREF c = cDim;
            if (u.state == UpdateState::Checking) swprintf(msg, 160, L"Checking…");
            else if (shown_.updAvail == 1) {
                const std::wstring tag = u.state == UpdateState::Available ? std::wstring(u.latestTag.begin(), u.latestTag.end()) : prefs_.latestTag;
                swprintf(msg, 160, L"%ls available — download", tag.c_str());
                c = shown_.hover == kHoverUpdate ? lighten(accent_, 30) : accent_;
            }
            else if (u.state == UpdateState::UpToDate) swprintf(msg, 160, L"Up to date");
            else if (u.state == UpdateState::Failed) swprintf(msg, 160, L"Couldn't reach GitHub");
            else swprintf(msg, 160, prefs_.autoUpdate ? L"Checks once a day" : L"Automatic checks off");
            text(updateLinkRect(), msg, c, fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            segment(autoRect(), prefs_.autoUpdate ? L"AUTO" : L"MANUAL", prefs_.autoUpdate, shown_.hover == kHoverAuto);
            segment(checkRect(), L"CHECK", false, shown_.hover == kHoverCheck);
        }
    }

    Controller* ctl_;
    Prefs prefs_;
    double s_ = 1.0;
    int W_ = kW, H_ = kH;
    COLORREF accent_ = RGB(150, 205, 120);
    HWND hwnd_ = nullptr;
    HBITMAP bgDib_ = nullptr, litDib_ = nullptr, backDib_ = nullptr;
    void* bgBits_ = nullptr; void* litBits_ = nullptr;
    HDC bgDc_ = nullptr, litDc_ = nullptr, dc_ = nullptr;
    HGDIOBJ bgOld_ = nullptr, litOld_ = nullptr, oldBmp_ = nullptr;
    HFONT fLabel_ = nullptr, fBrand_ = nullptr, fValue_ = nullptr, fSmall_ = nullptr, fButton_ = nullptr, fNum_ = nullptr;
    Shown shown_{};
    bool live_ = false, forceFrame_ = true, dragging_ = false, listOpen_ = false, tracking_ = false, settings_ = false;
    int listScroll_ = 0, hover_ = kHoverNone, lastDragX_ = 0;
    double lastDragV_ = 0;
    double meter_[2]{-120, -120}, hold_[2]{-120, -120}, holdAge_[2]{0, 0}, peak_ = -120, peakAge_ = 0;
};

} // namespace

IPlugView* createEditor(Controller* c) { return new Editor(c); }

} // namespace asb
