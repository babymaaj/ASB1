// Native Win32 plugin window: Streaming toggle, Output Device, Level, live status + buffer meter.
// No UI framework (VSTGUI etc.) on purpose: small, fast, nothing extra to ship.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include "vst/Editor.h"
#include "vst/Controller.h"
#include "vst/Ids.h"
#include "core/Utf8.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace asb {
namespace {

constexpr int kW = 470, kH = 236;
enum : int { IdToggle = 101, IdDevice = 102, IdLevel = 103 };
constexpr UINT_PTR kTimer = 1;
constexpr int kLevelSteps = 660;               // 0.1 dB per trackbar step over -60..+6 dB

// Palette (dark, to sit inside FL Studio / most DAWs)
const COLORREF cBg     = RGB(22, 23, 26);
const COLORREF cPanel  = RGB(34, 36, 40);
const COLORREF cText   = RGB(232, 232, 228);
const COLORREF cDim    = RGB(140, 144, 150);
const COLORREF cAccent = RGB(198, 230, 60);    // on / good
const COLORREF cWarn   = RGB(240, 180, 50);
const COLORREF cError  = RGB(235, 85, 75);
const COLORREF cOff    = RGB(70, 73, 80);

const wchar_t* kClassName = L"ASIOStreamBridgeEditor";

HINSTANCE moduleInstance()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleInstance), &m);
    return m;
}

std::wstring wide(const std::string& utf8)
{
    const std::u16string u = utf8ToUtf16(utf8);
    return std::wstring(u.begin(), u.end());   // wchar_t is UTF-16 on Windows
}

double levelDb(double norm) { return kMinDb + norm * (kMaxDb - kMinDb); }

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
        hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, kW, kH,
                                static_cast<HWND>(parent), nullptr, moduleInstance(), this);
        if (!hwnd_) return kResultFalse;
        return EditorView::attached(parent, type);
    }

    tresult PLUGIN_API removed() override
    {
        if (hwnd_) { KillTimer(hwnd_, kTimer); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        freeGdi();
        return EditorView::removed();
    }

private:
    static void registerClass()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        if (GetClassInfoExW(moduleInstance(), kClassName, &wc)) return;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Editor::wndProc;
        wc.hInstance = moduleInstance();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_CREATE: hwnd_ = h; createControls(); syncFromParams(true); SetTimer(h, kTimer, 100, nullptr); return 0;
        case WM_TIMER: syncFromParams(false); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_CTLCOLORSTATIC:   // trackbar background
            SetBkColor(reinterpret_cast<HDC>(wp), cBg);
            return reinterpret_cast<LRESULT>(bgBrush_);
        case WM_DRAWITEM:
            if (wp == IdToggle) { drawToggle(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
            break;
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IdToggle && code == BN_CLICKED) {
                const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
                ctl_->editParam(kParamStreamEnable, on ? 0.0 : 1.0);
                InvalidateRect(toggle_, nullptr, FALSE);
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (id == IdDevice && code == CBN_SELCHANGE) {
                const int i = int(SendMessageW(combo_, CB_GETCURSEL, 0, 0));
                if (i >= 0) ctl_->editParam(kParamDevice, double(i) / double(kMaxDevices - 1));
            }
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == slider_) { onSlider(LOWORD(wp)); return 0; }
            break;
        case WM_DESTROY: KillTimer(h, kTimer); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void createControls()
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
        bgBrush_  = CreateSolidBrush(cBg);
        font_     = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fontBold_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fontTitle_= CreateFontW(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        fontSmall_= CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

        const HINSTANCE hi = moduleInstance();
        toggle_ = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  20, 52, 150, 36, hwnd_, reinterpret_cast<HMENU>(IdToggle), hi, nullptr);
        combo_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 96, 108, 354, 320, hwnd_, reinterpret_cast<HMENU>(IdDevice), hi, nullptr);
        slider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                  90, 148, 280, 30, hwnd_, reinterpret_cast<HMENU>(IdLevel), hi, nullptr);
        SendMessageW(combo_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowTheme(combo_, L"DarkMode_CFD", nullptr);   // dark dropdown on Windows 10 1809+ (ignored elsewhere)
        SendMessageW(slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, kLevelSteps));
        SendMessageW(slider_, TBM_SETPAGESIZE, 0, 10);   // PgUp/PgDn = 1 dB

        const auto& devs = ctl_->devices();
        for (const auto& d : devs) SendMessageW(combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(d.name).c_str()));
    }

    void onSlider(int code)
    {
        const double v = double(SendMessageW(slider_, TBM_GETPOS, 0, 0)) / kLevelSteps;
        if (code == TB_THUMBTRACK) {
            if (!dragging_) { ctl_->beginEdit(kParamOutputLevel); dragging_ = true; }
            ctl_->setParamNormalized(kParamOutputLevel, v);
            ctl_->performEdit(kParamOutputLevel, v);
        } else if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
            if (dragging_) {
                ctl_->setParamNormalized(kParamOutputLevel, v);
                ctl_->performEdit(kParamOutputLevel, v);
                ctl_->endEdit(kParamOutputLevel);
                dragging_ = false;
            } else {
                ctl_->editParam(kParamOutputLevel, v);
            }
        } else {                                   // keyboard / click on the track
            ctl_->editParam(kParamOutputLevel, v);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Pull values from the controller (they change via automation, preset load, the host's generic UI...).
    void syncFromParams(bool force)
    {
        const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
        if (force || on != lastOn_) { lastOn_ = on; InvalidateRect(toggle_, nullptr, FALSE); }

        const double dev = ctl_->getParamNormalized(kParamDevice);
        const int idx = int(dev * (kMaxDevices - 1) + 0.5);
        if ((force || idx != lastDev_) && !SendMessageW(combo_, CB_GETDROPPEDSTATE, 0, 0)) {
            lastDev_ = idx;
            const int count = int(SendMessageW(combo_, CB_GETCOUNT, 0, 0));
            SendMessageW(combo_, CB_SETCURSEL, idx < count ? idx : -1, 0);
        }
        if (!dragging_) {
            const int pos = int(std::lround(ctl_->getParamNormalized(kParamOutputLevel) * kLevelSteps));
            if (force || pos != int(SendMessageW(slider_, TBM_GETPOS, 0, 0))) SendMessageW(slider_, TBM_SETPOS, TRUE, pos);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);    // status + meter
    }

    void drawToggle(const DRAWITEMSTRUCT& di)
    {
        const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
        HDC dc = di.hDC;
        const RECT r = di.rcItem;
        HBRUSH bg = CreateSolidBrush(cBg);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HBRUSH fill = CreateSolidBrush(on ? cAccent : cPanel);
        HPEN pen = CreatePen(PS_SOLID, 1, on ? cAccent : cOff);
        HGDIOBJ ob = SelectObject(dc, fill), op = SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 10, 10);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(fill); DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, on ? RGB(20, 22, 10) : cText);
        HGDIOBJ of = SelectObject(dc, fontBold_);
        RECT tr = r;
        DrawTextW(dc, on ? L"STREAMING ON" : L"STREAMING OFF", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }

    void text(HDC dc, int x, int y, const wchar_t* s, COLORREF c, HFONT f)
    {
        SetTextColor(dc, c);
        HGDIOBJ of = SelectObject(dc, f);
        TextOutW(dc, x, y, s, int(wcslen(s)));
        SelectObject(dc, of);
    }

    void paint()
    {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd_, &ps);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, kW, kH);
        HGDIOBJ obmp = SelectObject(dc, bmp);
        RECT all{0, 0, kW, kH};
        FillRect(dc, &all, bgBrush_);
        SetBkMode(dc, TRANSPARENT);

        // header
        const wchar_t* title = L"ASIO STREAM BRIDGE";
        text(dc, 20, 14, title, cText, fontTitle_);
        SIZE tsz{};
        { HGDIOBJ of = SelectObject(dc, fontTitle_); GetTextExtentPoint32W(dc, title, int(wcslen(title)), &tsz); SelectObject(dc, of); }
        wchar_t ver[64];
        swprintf(ver, 64, L"v%hs", kVersion);
        text(dc, 20 + tsz.cx + 10, 19, ver, cDim, fontSmall_);

        // status (right of the toggle)
        StreamStats st;
        const bool live = ctl_->liveStats(st);
        if (!live) st.status = static_cast<StreamStatus>(int(std::lround(ctl_->getParamNormalized(kParamStatus) * 3.0)));
        const bool on = ctl_->getParamNormalized(kParamStreamEnable) >= 0.5;
        COLORREF dot = cOff;
        wchar_t line1[128] = L"Off", line2[128] = L"";
        switch (st.status) {
        case StreamStatus::Off:
            if (on) { swprintf(line1, 128, L"Waiting for audio"); swprintf(line2, 128, L"press play / make sound in FL"); dot = cWarn; }
            else    { swprintf(line1, 128, L"Off"); swprintf(line2, 128, L"FL output is untouched"); }
            break;
        case StreamStatus::Connecting: swprintf(line1, 128, L"Connecting…"); dot = cWarn; break;
        case StreamStatus::Streaming:
            dot = cAccent;
            swprintf(line1, 128, L"Streaming");
            if (live) swprintf(line2, 128, L"%.0f Hz device · %.0f ms buffered", st.deviceRate, st.latencyMs);
            break;
        case StreamStatus::Error:
            dot = cError;
            swprintf(line1, 128, L"Can't open device");
            swprintf(line2, 128, L"retrying every second");
            break;
        }
        HBRUSH db = CreateSolidBrush(dot);
        HGDIOBJ obr = SelectObject(dc, db);
        HGDIOBJ open = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, 190, 62, 202, 74);
        SelectObject(dc, obr); SelectObject(dc, open); DeleteObject(db);
        text(dc, 210, 54, line1, cText, fontBold_);
        text(dc, 210, 73, line2, cDim, fontSmall_);

        // labels
        text(dc, 20, 112, L"OUTPUT", cDim, fontSmall_);
        text(dc, 20, 155, L"LEVEL", cDim, fontSmall_);
        const double lv = ctl_->getParamNormalized(kParamOutputLevel);
        wchar_t db_[32];
        if (lv <= 0.001) swprintf(db_, 32, L"-inf dB"); else swprintf(db_, 32, L"%+.1f dB", levelDb(lv));
        text(dc, 380, 153, db_, cText, font_);

        // buffer meter: fill relative to 2x target (~middle = healthy)
        text(dc, 20, 196, L"BUFFER", cDim, fontSmall_);
        const RECT track{96, 199, 290, 209};
        HBRUSH tb = CreateSolidBrush(cPanel);
        FillRect(dc, &track, tb);
        DeleteObject(tb);
        // live: buffered ms on a 0..120 ms scale (target ~30 ms = a quarter). Otherwise the processor's meter param.
        const double frac = live ? st.latencyMs / 120.0 : ctl_->getParamNormalized(kParamBufferHealth);
        if (st.status == StreamStatus::Streaming) {
            RECT f = track;
            f.right = f.left + LONG((track.right - track.left) * std::clamp(frac, 0.0, 1.0));
            HBRUSH fb = CreateSolidBrush(st.latencyMs > 150.0 ? cWarn : cAccent);
            FillRect(dc, &f, fb);
            DeleteObject(fb);
        }
        if (live) {
            wchar_t u[64];
            swprintf(u, 64, L"drops %llu · resyncs %llu", (unsigned long long)st.underruns, (unsigned long long)st.resyncs);
            RECT ur{296, 194, kW - 26, 214};
            SetTextColor(dc, cDim);
            HGDIOBJ of = SelectObject(dc, fontSmall_);
            DrawTextW(dc, u, -1, &ur, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }

        BitBlt(wdc, 0, 0, kW, kH, dc, 0, 0, SRCCOPY);
        SelectObject(dc, obmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd_, &ps);
    }

    void freeGdi()
    {
        for (HGDIOBJ* o : {reinterpret_cast<HGDIOBJ*>(&bgBrush_), reinterpret_cast<HGDIOBJ*>(&font_),
                           reinterpret_cast<HGDIOBJ*>(&fontBold_), reinterpret_cast<HGDIOBJ*>(&fontTitle_),
                           reinterpret_cast<HGDIOBJ*>(&fontSmall_)})
            if (*o) { DeleteObject(*o); *o = nullptr; }
    }

    Controller* ctl_;
    HWND hwnd_ = nullptr, toggle_ = nullptr, combo_ = nullptr, slider_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    HFONT font_ = nullptr, fontBold_ = nullptr, fontTitle_ = nullptr, fontSmall_ = nullptr;
    bool dragging_ = false, lastOn_ = false;
    int lastDev_ = -1;
};

} // namespace

IPlugView* createEditor(Controller* c) { return new Editor(c); }

} // namespace asb
