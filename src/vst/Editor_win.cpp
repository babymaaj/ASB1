// Native Win32 plugin window. Flat, quiet, typographic: solid colours, 1 px hairlines, one accent that only
// appears when something is live. No gradients, glows or skeuomorphic hardware.
//
// CPU: plain GDI (rectangles + text) into a back buffer, blitted once per frame. A 30 Hz timer runs only while
// the window is open, and a frame is drawn only when something on screen changed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include "vst/Editor.h"
#include "vst/Controller.h"
#include "vst/Ids.h"
#include "core/Utf8.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace asb {

// Paint-time statistics (editor_preview prints them; used for CPU checks).
static std::atomic<uint64_t> g_paintCount{0}, g_paintNs{0};
double editorAvgPaintUs() { const uint64_t c = g_paintCount.load(); return c ? g_paintNs.load() / 1000.0 / double(c) : 0.0; }
uint64_t editorPaintCount() { return g_paintCount.load(); }

namespace {

constexpr int kW = 460, kH = 272;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kFrameMs = 33;

// layout
constexpr int kPad = 24, kCol = 118, kRight = kW - kPad;
const RECT rHeader{0, 0, kW, 58};
const RECT rOutput{0, 59, kW, 105};
const RECT rLevel{0, 106, kW, 152};
const RECT rSignal{0, 153, kW, 199};
const RECT rFader{kCol, 122, kRight - 70, 136};          // hit area; track is drawn centred in it
const RECT rButton{kRight - 132, 216, kRight, 254};
constexpr int kListRowH = 28, kListMaxRows = 7;

// palette: warm near-black, off-white ink, a single accent
const COLORREF cBg     = RGB(19, 18, 17);
const COLORREF cLine   = RGB(44, 42, 39);
const COLORREF cInk    = RGB(236, 232, 224);
const COLORREF cDim    = RGB(132, 127, 119);
const COLORREF cFaint  = RGB(70, 67, 63);
const COLORREF cAccent = RGB(150, 205, 120);              // live
const COLORREF cAmber  = RGB(226, 170, 72);
const COLORREF cRed    = RGB(226, 84, 70);
const COLORREF cPanel  = RGB(27, 26, 24);

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
        registerClass();
        hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, kW, kH,
                                static_cast<HWND>(parent), nullptr, moduleInstance(), this);
        if (!hwnd_) return kResultFalse;
        return EditorView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override
    {
        if (hwnd_) { KillTimer(hwnd_, kTimer); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        freeResources();
        return EditorView::removed();
    }
    tresult PLUGIN_API onWheel(float distance) override { nudgeLevel(distance > 0 ? 0.5 : -0.5); return kResultTrue; }

private:
    static constexpr const wchar_t* kClassName = L"ASIOStreamBridgeFlat";

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
        int barPx[2]{-1, -1}, holdPx[2]{-1, -1}, peak10 = 0, rateHz = 0, latMs = -1;
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
        const int span = kRight - 44 - kCol;
        for (int ch = 0; ch < 2; ++ch) {
            s.barPx[ch] = int(std::lround(meterPos(meter_[ch]) * span));
            s.holdPx[ch] = hold_[ch] > -48.0 ? int(std::lround(meterPos(hold_[ch]) * span)) : -1;
        }
        s.peak10 = int(std::lround(std::max(peak_, -99.0) * 10));
        if (live_) { s.rateHz = int(st.deviceRate); s.latMs = int(std::lround(st.latencyMs)); s.drops = st.underruns; }
        if (forceFrame_ || !(s == shown_)) { shown_ = s; InvalidateRect(hwnd_, nullptr, FALSE); }
    }

    // ---------------------------------------------------------------- drawing (GDI only)
    void buildResources()
    {
        HDC screen = GetDC(hwnd_);
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
        void* bits = nullptr;
        backDib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        dc_ = CreateCompatibleDC(screen);
        oldBmp_ = SelectObject(dc_, backDib_);
        ReleaseDC(hwnd_, screen);
        // Bahnschrift (Windows 10+, DIN-like) reads as "instrument panel"; Segoe UI as fallback.
        const bool din = hasFont(L"Bahnschrift");
        auto font = [&](int px, int weight, bool display) {
            return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               display && din ? L"Bahnschrift" : L"Segoe UI");
        };
        fTitle_ = font(19, FW_SEMIBOLD, true);
        fLabel_ = font(11, FW_SEMIBOLD, true);
        fValue_ = font(15, FW_NORMAL, false);
        fSmall_ = font(12, FW_NORMAL, false);
        fButton_ = font(13, FW_SEMIBOLD, true);
        fNum_ = font(13, FW_NORMAL, true);
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
        if (dc_) { SelectObject(dc_, oldBmp_); DeleteDC(dc_); dc_ = nullptr; }
        if (backDib_) { DeleteObject(backDib_); backDib_ = nullptr; }
        for (HFONT* f : {&fTitle_, &fLabel_, &fValue_, &fSmall_, &fButton_, &fNum_}) if (*f) { DeleteObject(*f); *f = nullptr; }
    }

    void fill(const RECT& r, COLORREF c) { SetDCBrushColor(dc_, c); FillRect(dc_, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); }
    void hline(int x0, int x1, int y, COLORREF c) { fill(RECT{x0, y, x1, y + 1}, c); }
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
    void label(const RECT& row, const wchar_t* t) { text(RECT{kPad, row.top, kCol - 8, row.bottom}, t, cDim, fLabel_, DT_LEFT | DT_VCENTER, 2); }
    void dot(int cx, int cy, int r, COLORREF c)
    {
        SetDCBrushColor(dc_, c); SetDCPenColor(dc_, c);
        HGDIOBJ ob = SelectObject(dc_, GetStockObject(DC_BRUSH)), op = SelectObject(dc_, GetStockObject(DC_PEN));
        Ellipse(dc_, cx - r, cy - r, cx + r + 1, cy + r + 1);
        SelectObject(dc_, ob); SelectObject(dc_, op);
    }

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

        fill(RECT{0, 0, kW, kH}, cBg);

        // header: wordmark + version
        text(RECT{kPad, 0, kW, rHeader.bottom}, L"STREAM BRIDGE", cInk, fTitle_, DT_LEFT | DT_VCENTER, 3);
        wchar_t ver[48]; swprintf(ver, 48, L"v%hs", kVersion);
        text(RECT{0, 0, kRight, rHeader.bottom}, ver, cFaint, fSmall_, DT_RIGHT | DT_VCENTER);
        hline(0, kW, rHeader.bottom, cLine);

        // OUTPUT
        label(rOutput, L"OUTPUT");
        if (shown_.hover == kHoverOutput && !listOpen_) fill(RECT{kCol - 10, rOutput.top + 7, kRight + 4, rOutput.bottom - 7}, cPanel);
        text(RECT{kCol, rOutput.top, kRight - 22, rOutput.bottom}, devName.c_str(), cInk, fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        {   // chevron
            const int cx = kRight - 6, cy = (rOutput.top + rOutput.bottom) / 2;
            for (int i = 0; i < 4; ++i) hline(cx - 4 + i, cx + 4 - i, cy - 1 + i, cDim);
        }
        hline(kPad, kRight, rOutput.bottom, cLine);

        // LEVEL: flat fader
        label(rLevel, L"LEVEL");
        {
            const double v = shown_.levelPos / 660.0;
            const int cy = (rFader.top + rFader.bottom) / 2;
            const int x = rFader.left + int(std::lround(v * (rFader.right - rFader.left)));
            fill(RECT{rFader.left, cy - 1, rFader.right, cy + 1}, cFaint);
            fill(RECT{rFader.left, cy - 1, x, cy + 1}, on ? cAccent : cDim);
            const int zx = rFader.left + int(std::lround(60.0 / 66.0 * (rFader.right - rFader.left)));   // 0 dB mark
            fill(RECT{zx, cy - 6, zx + 1, cy - 3}, cDim);
            const bool hot = dragging_ || shown_.hover == kHoverFader;
            fill(RECT{x - 3, cy - 8, x + 3, cy + 8}, hot ? RGB(255, 252, 245) : cInk);
            wchar_t vt[32];
            if (shown_.levelPos <= 0) swprintf(vt, 32, L"-∞ dB"); else swprintf(vt, 32, L"%+.1f dB", levelDb(v));
            text(RECT{kRight - 64, rLevel.top, kRight, rLevel.bottom}, vt, cInk, fNum_, DT_RIGHT | DT_VCENTER);
        }
        hline(kPad, kRight, rLevel.bottom, cLine);

        // SIGNAL: what goes to the stream, L/R
        label(rSignal, L"SIGNAL");
        {
            const int span = kRight - 44 - kCol;
            const int warnX = kCol + int(std::lround(meterPos(-6.0) * span)), clipX = kCol + int(std::lround(meterPos(0.0) * span));
            for (int ch = 0; ch < 2; ++ch) {
                const int y = rSignal.top + 16 + ch * 9;
                fill(RECT{kCol, y, kCol + span, y + 4}, cPanel);
                const int px = shown_.barPx[ch];
                if (px > 0) {
                    fill(RECT{kCol, y, std::min(kCol + px, warnX), y + 4}, cAccent);
                    if (kCol + px > warnX) fill(RECT{warnX, y, std::min(kCol + px, clipX), y + 4}, cAmber);
                    if (kCol + px > clipX) fill(RECT{clipX, y, kCol + px, y + 4}, cRed);
                }
                if (shown_.holdPx[ch] > 0) fill(RECT{kCol + shown_.holdPx[ch] - 1, y, kCol + shown_.holdPx[ch] + 1, y + 4}, cInk);
            }
            wchar_t pk[16];
            if (!live_ || shown_.peak10 <= -480) swprintf(pk, 16, L"–"); else swprintf(pk, 16, L"%.1f", shown_.peak10 / 10.0);
            text(RECT{kRight - 40, rSignal.top, kRight, rSignal.bottom}, pk, shown_.peak10 > 0 && live_ ? cRed : cDim, fNum_, DT_RIGHT | DT_VCENTER);
        }
        hline(0, kW, rSignal.bottom, cLine);

        // footer: status (left), stream button (right)
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
            dot(kPad + 4, 229, 4, dc);
            text(RECT{kPad + 16, 218, rButton.left - 12, 240}, head, cInk, fValue_, DT_LEFT | DT_VCENTER);
            text(RECT{kPad + 16, 238, rButton.left - 12, 256}, sub, cDim, fSmall_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

            const bool hot = shown_.hover == kHoverButton;
            if (on) {
                fill(rButton, hot ? RGB(170, 222, 140) : cAccent);
                text(rButton, L"STREAMING", RGB(16, 22, 12), fButton_, DT_CENTER | DT_VCENTER, 2);
            } else {
                if (hot) fill(rButton, cPanel);
                frame(rButton, hot ? cDim : cFaint);
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
                if (hov) fill(row, RGB(38, 36, 33));
                if (sel) fill(RECT{row.left, row.top + 6, row.left + 2, row.bottom - 6}, cAccent);
                text(RECT{row.left + 12, row.top, row.right - 12, row.bottom}, wide(devs[size_t(di)].name).c_str(),
                     sel ? cInk : RGB(200, 195, 186), fValue_, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
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
    HBITMAP backDib_ = nullptr;
    HDC dc_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    HFONT fTitle_ = nullptr, fLabel_ = nullptr, fValue_ = nullptr, fSmall_ = nullptr, fButton_ = nullptr, fNum_ = nullptr;
    Shown shown_{};
    bool live_ = false, forceFrame_ = true, dragging_ = false, listOpen_ = false, tracking_ = false;
    int listScroll_ = 0, hover_ = kHoverNone, lastDragX_ = 0;
    double lastDragV_ = 0;
    double meter_[2]{-120, -120}, hold_[2]{-120, -120}, holdAge_[2]{0, 0}, peak_ = -120, peakAge_ = 0;
};

} // namespace

IPlugView* createEditor(Controller* c) { return new Editor(c); }

} // namespace asb
