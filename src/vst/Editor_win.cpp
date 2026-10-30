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
const RECT rButton{kRight - 158, 230, kRight, 268};
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
const COLORREF cAccent = RGB(150, 205, 120);
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

// colour of LED i (green, amber from -6 dB, red from 0 dB)
COLORREF ledColor(int i)
{
    const double pos = (i + 0.5) / kDots;
    if (pos >= meterPos(0.0)) return cRed;
    if (pos >= meterPos(-6.0)) return cAmber;
    return cAccent;
}

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

class Editor final : public EditorView {
public:
    explicit Editor(Controller* c) : EditorView(c, nullptr), ctl_(c) { ViewRect r(0, 0, kW, kH); setRect(r); }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override
    {
        return (type && std::strcmp(type, kPlatformTypeHWND) == 0) ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override
    {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent) return kResultFalse;
        gdipAcquire();
        registerClass();
        hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, kW, kH,
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

    // ---------------------------------------------------------------- input
    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        switch (msg) {
        case WM_CREATE: hwnd_ = h; buildResources(); SetTimer(h, kTimer, kFrameMs, nullptr); return 0;
        case WM_TIMER: tick(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_LBUTTONDOWN: SetFocus(h); onDown(x, y); return 0;
        case WM_LBUTTONDBLCLK:
            if (!listOpen_ && inRect(faderHit(), x, y)) { ctl_->editParam(kParamOutputLevel, (0.0 - kMinDb) / (kMaxDb - kMinDb)); dirty(); }
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
            POINT p{x, y}; ScreenToClient(h, &p);
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (listOpen_) { listScroll_ = std::clamp(listScroll_ - steps, 0, maxScroll()); dirty(); }
            else if (inRect(rOutput, p.x, p.y)) stepDevice(-steps);
            else nudgeLevel(steps * ((GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ? 0.1 : 0.5));
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && listOpen_) { listOpen_ = false; dirty(); return 0; }
            break;
        case WM_DESTROY: KillTimer(h, kTimer); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    static constexpr int kHoverNone = -100, kHoverOutput = -10, kHoverButton = -11, kHoverFader = -12;
    RECT faderHit() const { return RECT{rFader.left - 8, rFader.top - 8, rFader.right + 8, rFader.bottom + 8}; }
    int hoverAt(int x, int y) const
    {
        if (listOpen_) { const int r = listRowAt(x, y); return r >= 0 ? r : kHoverNone; }
        if (inRect(rButton, x, y)) return kHoverButton;
        if (inRect(rOutput, x, y)) return kHoverOutput;
        if (inRect(faderHit(), x, y)) return kHoverFader;
        return kHoverNone;
    }

    void onDown(int x, int y)
    {
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
        if (forceFrame_ || !(s == shown_)) { shown_ = s; InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    // ---------------------------------------------------------------- cached layers
    void makeDib(HBITMAP& bmp, HDC& dc, HGDIOBJ& old, void** bitsOut = nullptr)
    {
        HDC screen = GetDC(hwnd_);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
        void* bits = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitsOut) *bitsOut = bits;
        dc = CreateCompatibleDC(screen); old = SelectObject(dc, bmp);
        ReleaseDC(hwnd_, screen);
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

    // Draw anti-aliased content via an offscreen GDI+ bitmap, then composite onto a GDI DC
    // (reliable AA on every GDI+ implementation, including Wine's).
    // Anti-aliased drawing onto a GDI DIB: copy the pixels into a GDI+-owned bitmap, draw there, copy back.
    // (GDI+ only anti-aliases reliably on bitmaps it owns; drawing onto an HDC or foreign memory is not smoothed by
    // every implementation.) Runs once per window open.
    template <class F> void aaLayer(void* bits, HDC, F&& draw)
    {
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        G::Bitmap layer(kW, kH, PixelFormat32bppPARGB);
        G::Rect all(0, 0, kW, kH);
        G::BitmapData d;
        if (layer.LockBits(&all, G::ImageLockModeWrite, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < kH; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d.Scan0) + y * d.Stride);
                for (int x = 0; x < kW; ++x) row[x] = px[y * kW + x] | 0xFF000000u;   // opaque panel
            }
            layer.UnlockBits(&d);
        }
        {
            G::Graphics g(&layer);
            g.SetSmoothingMode(G::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
            draw(g);
        }
        if (layer.LockBits(&all, G::ImageLockModeRead, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < kH; ++y)
                std::memcpy(px + y * kW, static_cast<uint8_t*>(d.Scan0) + y * d.Stride, size_t(kW) * 4);
            layer.UnlockBits(&d);
        }
    }

    // Static panel + lit LED layer. Called once per window.
    void renderPanel()
    {
        HDC prev = dc_;
        dc_ = bgDc_;                                    // reuse fill/text helpers on the panel DC
        SetBkMode(dc_, TRANSPARENT);
        fill(RECT{0, 0, kW, kH}, cBg);
        for (int y = 2; y < kH; y += 4)                 // fine perforation
            for (int x = (y / 4) % 2 ? 4 : 2; x < kW; x += 4) SetPixelV(dc_, x, y, cPerf);
        frame(RECT{0, 0, kW, kH}, RGB(10, 10, 11));

        // header: wordmark, "by maai", version
        aaLayer(bgBits_, bgDc_, [&](G::Graphics& g) {
            const G::REAL wx = G::REAL(kPad + 4), wy = 15.f, ws = 1.0f, wst = 7.5f;
            drawEngravedWordmark(g, wx, wy, ws, wst, kLetterA | kLetterB);
            drawEngravedWordmark(g, wx, wy, ws, wst, kLetterS, G::Color(255, 214, 214, 210), G::Color(255, 240, 240, 236));
        });
        engrave(RECT{kPad + 112, 16, kW, 56}, L"by maai", cDim, fBrand_, DT_LEFT | DT_BOTTOM, 1);
        wchar_t ver[48]; swprintf(ver, 48, L"v%hs", kVersion);
        engrave(RECT{0, 16, kRight, 56}, ver, cFaint, fSmall_, DT_RIGHT | DT_BOTTOM, 0);
        groove(0, kW, rHeader.bottom);

        stamp(rOutput, L"OUTPUT");
        groove(kPad, kRight, rOutput.bottom);
        stamp(rLevel, L"LEVEL");
        groove(kPad, kRight, rLevel.bottom);
        stamp(rSignal, L"SIGNAL");
        groove(0, kW, rSignal.bottom);
        // dB scale under the LEDs
        for (double db : {-36.0, -24.0, -12.0, -6.0, 0.0}) {
            const int x = kMeterX + int(std::lround(meterPos(db) * kDots)) * kPitch - (kPitch - kDot) / 2;
            fill(RECT{x, kMeterY0 + 2 * kMeterRowGap - 1, x + 1, kMeterY0 + 2 * kMeterRowGap + 2}, cFaint);
        }

        // unlit LEDs on the panel, lit LEDs on the lit layer (both anti-aliased, both drawn once)
        aaLayer(bgBits_, bgDc_, [&](G::Graphics& g) { drawLeds(g, false); });
        BitBlt(litDc_, 0, 0, kW, kH, bgDc_, 0, 0, SRCCOPY);
        GdiFlush();
        aaLayer(litBits_, litDc_, [&](G::Graphics& g) { drawLeds(g, true); });
        dc_ = prev;
    }

    void drawLeds(G::Graphics& g, bool lit)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kDots; ++i) {
                const G::REAL x = G::REAL(kMeterX + i * kPitch), y = G::REAL(kMeterY0 + ch * kMeterRowGap);
                if (!lit) {   // recessed socket
                    G::SolidBrush rim(G::Color(255, 12, 12, 13)); g.FillEllipse(&rim, x - 0.6f, y - 0.6f, kDot + 1.2f, kDot + 1.2f);
                    G::SolidBrush off(gc(cDotOff)); g.FillEllipse(&off, x, y + 0.3f, G::REAL(kDot), G::REAL(kDot));
                } else {
                    G::SolidBrush on(gc(ledColor(i))); g.FillEllipse(&on, x, y, G::REAL(kDot), G::REAL(kDot));
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

    // ---------------------------------------------------------------- GDI helpers (operate on dc_)
    void fill(const RECT& r, COLORREF c) { SetDCBrushColor(dc_, c); FillRect(dc_, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
    void hline(int x0, int x1, int y, COLORREF c) { fill(RECT{x0, y, x1, y + 1}, c); }
    void groove(int x0, int x1, int y) { hline(x0, x1, y, RGB(9, 9, 10)); hline(x0, x1, y + 1, RGB(36, 36, 38)); }   // milled line
    void frame(const RECT& r, COLORREF c)
    {
        hline(r.left, r.right, r.top, c); hline(r.left, r.right, r.bottom - 1, c);
        fill(RECT{r.left, r.top, r.left + 1, r.bottom}, c); fill(RECT{r.right - 1, r.top, r.right, r.bottom}, c);
    }
    void text(RECT r, const wchar_t* t, COLORREF c, HFONT f, UINT fmt, int tracking = 0)
    {
        SetTextColor(dc_, c);
        SetTextCharacterExtra(dc_, tracking);
        HGDIOBJ of = SelectObject(dc_, f);
        DrawTextW(dc_, t, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc_, of);
        SetTextCharacterExtra(dc_, 0);
    }
    // stamped into metal: shadow above, faint highlight below
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

    // ---------------------------------------------------------------- frame
    void paint()
    {
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd_, &ps);
        SetBkMode(dc_, TRANSPARENT);

        const bool on = shown_.on == 1;
        const auto status = static_cast<StreamStatus>(shown_.status < 0 ? 0 : shown_.status);
        const auto& devs = ctl_->devices();
        const std::wstring devName = (shown_.dev >= 0 && shown_.dev < int(devs.size())) ? wide(devs[size_t(shown_.dev)].name) : L"Device unavailable";

        BitBlt(dc_, 0, 0, kW, kH, bgDc_, 0, 0, SRCCOPY);

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

        // OUTPUT
        if (shown_.hover == kHoverOutput && !listOpen_) fill(RECT{kCol - 10, rOutput.top + 8, kRight + 4, rOutput.bottom - 7}, cPanel);
        text(RECT{kCol, rOutput.top, kRight - 22, rOutput.bottom}, devName.c_str(), cInk, fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        {
            const int cx = kRight - 6, cy = (rOutput.top + rOutput.bottom) / 2;
            for (int i = 0; i < 4; ++i) hline(cx - 4 + i, cx + 4 - i, cy - 1 + i, cDim);
        }

        // LEVEL: flat fader with a machined slot
        {
            const double v = shown_.levelPos / 660.0;
            const int cy = (rFader.top + rFader.bottom) / 2;
            const int x = rFader.left + int(std::lround(v * (rFader.right - rFader.left)));
            hline(rFader.left, rFader.right, cy - 1, RGB(8, 8, 9));
            hline(rFader.left, rFader.right, cy, cFaint);
            hline(rFader.left, rFader.right, cy + 1, RGB(38, 38, 40));
            fill(RECT{rFader.left, cy - 1, x, cy + 1}, on ? cAccent : cDim);
            const int zx = rFader.left + int(std::lround(60.0 / 66.0 * (rFader.right - rFader.left)));   // 0 dB mark
            fill(RECT{zx, cy - 7, zx + 1, cy - 4}, cDim);
            const bool hot = dragging_ || shown_.hover == kHoverFader;
            fill(RECT{x - 4, cy - 9, x + 4, cy + 9}, RGB(10, 10, 11));
            fill(RECT{x - 3, cy - 8, x + 3, cy + 8}, hot ? RGB(250, 250, 246) : RGB(206, 206, 202));
            hline(x - 3, x + 3, cy, RGB(70, 70, 72));                       // grip line
            wchar_t vt[32];
            if (shown_.levelPos <= 0) swprintf(vt, 32, L"-∞ dB"); else swprintf(vt, 32, L"%+.1f dB", levelDb(v));
            text(RECT{kRight - 64, rLevel.top, kRight, rLevel.bottom}, vt, cInk, fNum_, DT_RIGHT | DT_VCENTER);
        }

        // footer: status + stream button
        {
            COLORREF dc = cFaint; const wchar_t* head = L"Off";
            wchar_t sub[160] = L"";
            switch (status) {
            case StreamStatus::Streaming:
                dc = cAccent; head = L"Streaming";
                if (live_) swprintf(sub, 160, L"%d Hz  ·  %d ms  ·  %llu drops", shown_.rateHz, shown_.latMs, (unsigned long long)shown_.drops);
                break;
            case StreamStatus::Connecting: dc = cAmber; head = L"Connecting"; break;
            case StreamStatus::Error: dc = cRed; head = L"Can't open device"; swprintf(sub, 160, L"Retrying every second"); break;
            default:
                if (on) { dc = cAmber; head = L"Waiting for audio"; swprintf(sub, 160, L"Play something in FL"); }
                else swprintf(sub, 160, L"FL output is untouched");
                break;
            }
            dot(kPad + 4, 243, 4, dc);
            text(RECT{kPad + 16, 232, rButton.left - 12, 254}, head, cInk, fValue_, DT_LEFT | DT_VCENTER);
            text(RECT{kPad + 16, 252, rButton.left - 12, 270}, sub, cDim, fSmall_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            const bool hot = shown_.hover == kHoverButton;
            if (on) {
                fill(rButton, hot ? RGB(170, 222, 140) : cAccent);
                text(rButton, L"STREAMING", RGB(16, 22, 12), fButton_, DT_CENTER | DT_VCENTER, 2);
            } else {
                fill(rButton, hot ? RGB(34, 34, 36) : cPanel);
                frame(rButton, hot ? cDim : cFaint);
                hline(rButton.left + 1, rButton.right - 1, rButton.top + 1, RGB(48, 48, 50));   // top bevel
                text(rButton, L"START STREAM", cInk, fButton_, DT_CENTER | DT_VCENTER, 2);
            }
        }

        // device list
        if (listOpen_) {
            const RECT r = listRect();
            fill(r, cPanel);
            frame(r, cLine);
            const int rows = std::min(deviceCount() - listScroll_, kListMaxRows);
            for (int i = 0; i < rows; ++i) {
                const int di = i + listScroll_;
                const RECT row{r.left + 1, r.top + 4 + i * kListRowH, r.right - 1, r.top + 4 + (i + 1) * kListRowH};
                const bool sel = di == shown_.dev, hov = di == shown_.hover;
                if (hov) fill(row, RGB(38, 38, 40));
                if (sel) fill(RECT{row.left, row.top + 6, row.left + 2, row.bottom - 6}, cAccent);
                text(RECT{row.left + 12, row.top, row.right - 12, row.bottom}, wide(devs[size_t(di)].name).c_str(),
                     sel ? cInk : RGB(196, 196, 192), fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            }
            if (maxScroll() > 0) {
                const int h = r.bottom - r.top - 8, th = h * kListMaxRows / deviceCount();
                const int ty = r.top + 4 + (h - th) * listScroll_ / maxScroll();
                fill(RECT{r.right - 4, ty, r.right - 2, ty + th}, cDim);
            }
        }

        BitBlt(wdc, 0, 0, kW, kH, dc_, 0, 0, SRCCOPY);
        EndPaint(hwnd_, &ps);
        forceFrame_ = false;
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        g_paintNs.fetch_add(uint64_t((t1.QuadPart - t0.QuadPart) * 1'000'000'000.0 / fq.QuadPart));
        g_paintCount.fetch_add(1);
    }

    Controller* ctl_;
    HWND hwnd_ = nullptr;
    HBITMAP bgDib_ = nullptr, litDib_ = nullptr, backDib_ = nullptr;
    void* bgBits_ = nullptr; void* litBits_ = nullptr;
    HDC bgDc_ = nullptr, litDc_ = nullptr, dc_ = nullptr;
    HGDIOBJ bgOld_ = nullptr, litOld_ = nullptr, oldBmp_ = nullptr;
    HFONT fLabel_ = nullptr, fBrand_ = nullptr, fValue_ = nullptr, fSmall_ = nullptr, fButton_ = nullptr, fNum_ = nullptr;
    Shown shown_{};
    bool live_ = false, forceFrame_ = true, dragging_ = false, listOpen_ = false, tracking_ = false;
    int listScroll_ = 0, hover_ = kHoverNone, lastDragX_ = 0;
    double lastDragV_ = 0;
    double meter_[2]{-120, -120}, hold_[2]{-120, -120}, holdAge_[2]{0, 0}, peak_ = -120, peakAge_ = 0;
};

} // namespace

IPlugView* createEditor(Controller* c) { return new Editor(c); }

} // namespace asb
