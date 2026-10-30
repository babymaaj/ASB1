// Windows host for the ASB window: Win32 child window + GDI canvas (GDI+ only for the cached anti-aliased layers).
// CPU: the panel and the "all LEDs lit" layer are rendered once into DIBs; a frame is a few blits + GDI text.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <shellapi.h>

#include "vst/Editor.h"
#include "vst/Controller.h"
#include "ui/EditorCore.h"
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

namespace ui {
// ---- preferences file: %APPDATA%\maai\ASB\settings.ini
static std::wstring prefsPath(bool create)
{
    wchar_t buf[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(buf) + L"\\maai";
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\ASB";
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}
std::string readPrefsFile()
{
    const std::wstring p = prefsPath(false);
    std::string out;
    if (FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"rb")) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 65536) out.append(buf, n);
        fclose(f);
    }
    return out;
}
void writePrefsFile(const std::string& s)
{
    const std::wstring p = prefsPath(true);
    if (FILE* f = p.empty() ? nullptr : _wfopen(p.c_str(), L"wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
}
} // namespace ui

namespace {
using namespace asb::ui;

HINSTANCE moduleInstance()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleInstance), &m);
    return m;
}
std::wstring wide(const std::string& utf8) { const std::u16string u = utf8ToUtf16(utf8); return std::wstring(u.begin(), u.end()); }
COLORREF cr(Color c) { return RGB(red(c), green(c), blue(c)); }
G::Color gc(Color c, int a = 255) { return G::Color(BYTE(a), BYTE(red(c)), BYTE(green(c)), BYTE(blue(c))); }

std::atomic<int> g_gdipRefs{0};
ULONG_PTR g_gdipToken = 0;
void gdipAcquire() { if (g_gdipRefs.fetch_add(1) == 0) { G::GdiplusStartupInput in; G::GdiplusStartup(&g_gdipToken, &in, nullptr); } }
void gdipRelease() { if (g_gdipRefs.fetch_sub(1) == 1) G::GdiplusShutdown(g_gdipToken); }

// A 32-bit DIB with a DC whose world transform applies the UI scale (all drawing in logical units).
struct Surface {
    HBITMAP bmp = nullptr; HDC dc = nullptr; HGDIOBJ old = nullptr; uint32_t* bits = nullptr; int w = 0, h = 0;
    void create(HDC screen, int pw, int ph)
    {
        w = pw; h = ph;
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* p = nullptr;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        bits = static_cast<uint32_t*>(p);
        dc = CreateCompatibleDC(screen); old = SelectObject(dc, bmp);
    }
    void free() { if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; } if (bmp) { DeleteObject(bmp); bmp = nullptr; } }
};

struct Fonts {
    HFONT f[6]{};
    void create(HWND hwnd)
    {
        bool din = false;
        LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET; wcsncpy(lf.lfFaceName, L"Bahnschrift", LF_FACESIZE - 1);
        HDC dc = GetDC(hwnd);
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *reinterpret_cast<bool*>(p) = true; return 0; },
                            reinterpret_cast<LPARAM>(&din), 0);
        ReleaseDC(hwnd, dc);
        auto mk = [&](int px, int weight, bool condensed = false) {
            const wchar_t* face = !din ? L"Segoe UI" : (condensed ? L"Bahnschrift SemiCondensed" : L"Bahnschrift");
            return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
        };
        f[int(Font::Label)] = mk(11, FW_BOLD);  f[int(Font::Brand)] = mk(13, FW_NORMAL); f[int(Font::Value)] = mk(15, FW_NORMAL);
        f[int(Font::Small)] = mk(12, FW_NORMAL); f[int(Font::Button)] = mk(13, FW_BOLD);  f[int(Font::Num)] = mk(14, FW_NORMAL, true);
    }
    void free() { for (HFONT& h : f) if (h) { DeleteObject(h); h = nullptr; } }
};

// ---------------------------------------------------------------- GDI canvas
class GdiCanvas final : public Canvas {
public:
    GdiCanvas(Surface& s, double scale, Fonts& fonts, Surface* panel, Surface* lit)
        : s_(s), scale_(scale), fonts_(fonts), panel_(panel), lit_(lit) { applyScale(); SetBkMode(s_.dc, TRANSPARENT); }
    ~GdiCanvas() override { if (aa_) endSmooth(); }

    void fill(const Rect& r, Color c) override
    {
        if (aa_) { G::SolidBrush b(gc(c)); aa_->g.FillRectangle(&b, r.l, r.t, r.w(), r.h()); return; }
        SetDCBrushColor(s_.dc, cr(c));
        RECT rr{r.l, r.t, r.r, r.b};
        FillRect(s_.dc, &rr, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    // Letter-spacing is laid out by hand: GDI's character-extra is ignored in GM_ADVANCED (used for scaling).
    void text(const Rect& r, const std::string& utf8, Color c, Font f, unsigned align, int tracking) override
    {
        if (aa_) flushSmooth();
        const std::wstring t = wide(utf8);
        SetTextColor(s_.dc, cr(c));
        HGDIOBJ of = SelectObject(s_.dc, fonts_.f[int(f)]);
        if (tracking == 0) {
            UINT fmt = DT_SINGLELINE | DT_NOPREFIX;
            fmt |= (align & Center) ? DT_CENTER : (align & Right) ? DT_RIGHT : DT_LEFT;
            fmt |= (align & VCenter) ? DT_VCENTER : (align & Bottom) ? DT_BOTTOM : DT_TOP;
            if (align & Ellipsis) fmt |= DT_END_ELLIPSIS;
            RECT rr{r.l, r.t, r.r, r.b};
            DrawTextW(s_.dc, t.c_str(), int(t.size()), &rr, fmt);
        } else {
            const int len = int(std::min<size_t>(t.size(), 128));
            int adv[128]; int total = 0;
            for (int i = 0; i < len; ++i) {
                SIZE sz{}; GetTextExtentPoint32W(s_.dc, t.c_str() + i, 1, &sz);
                adv[i] = sz.cx + (i + 1 < len ? tracking : 0);
                total += adv[i];
            }
            TEXTMETRICW tm{}; GetTextMetricsW(s_.dc, &tm);
            int x = r.l, y = r.t;
            if (align & Center) x = (r.l + r.r - total) / 2; else if (align & Right) x = r.r - total;
            if (align & VCenter) y = (r.t + r.b - tm.tmHeight) / 2; else if (align & Bottom) y = r.b - tm.tmHeight;
            SetTextAlign(s_.dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
            for (int i = 0; i < len; ++i) { TextOutW(s_.dc, x, y, t.c_str() + i, 1); x += adv[i]; }
        }
        SelectObject(s_.dc, of);
    }

    void ellipse(float x, float y, float w, float h, Color c, int alpha) override
    {
        if (aa_) { G::SolidBrush b(gc(c, alpha)); aa_->g.FillEllipse(&b, x, y, w, h); return; }
        SetDCBrushColor(s_.dc, cr(c)); SetDCPenColor(s_.dc, cr(c));
        HGDIOBJ ob = SelectObject(s_.dc, GetStockObject(DC_BRUSH)), op = SelectObject(s_.dc, GetStockObject(DC_PEN));
        Ellipse(s_.dc, int(std::lround(x)), int(std::lround(y)), int(std::lround(x + w)), int(std::lround(y + h)));
        SelectObject(s_.dc, ob); SelectObject(s_.dc, op);
    }

    // Stroke = widen each figure to an outline and FILL it with a winding rule (overlaps merge, always anti-aliased).
    void strokes(const std::vector<Polyline>& figs, float width, Color c, int alpha, float dx, float dy) override
    {
        const bool own = !aa_;
        if (own) beginSmooth();
        G::GraphicsPath p;
        for (const auto& f : figs) {
            if (f.size() < 2) continue;
            std::vector<G::PointF> pts; pts.reserve(f.size());
            for (const auto& q : f) pts.emplace_back(q.x + dx, q.y + dy);
            p.StartFigure();
            p.AddLines(pts.data(), INT(pts.size()));
        }
        G::Pen pen(G::Color(255, 0, 0, 0), width);
        pen.SetStartCap(G::LineCapRound); pen.SetEndCap(G::LineCapRound); pen.SetLineJoin(G::LineJoinRound);
        p.Widen(&pen, nullptr, 0.15f);
        p.SetFillMode(G::FillModeWinding);
        G::SolidBrush b(gc(c, alpha));
        aa_->g.FillPath(&b, &p);
        if (own) endSmooth();
    }

    void perforation(int step, Color c) override
    {
        ModifyWorldTransform(s_.dc, nullptr, MWT_IDENTITY);     // device pixels: crisp at any size
        const int st = std::max(step, int(std::lround(step * scale_)));
        for (int y = 2; y < s_.h; y += st)
            for (int x = ((y / st) % 2) ? st : st / 2; x < s_.w; x += st) SetPixelV(s_.dc, x, y, cr(c));
        applyScale();
    }

    // GDI+ only anti-aliases reliably on bitmaps it owns: copy pixels in, draw, copy back.
    void beginSmooth() override
    {
        if (aa_) return;
        GdiFlush();
        aa_ = std::make_unique<Smooth>(s_.w, s_.h);
        G::Rect all(0, 0, s_.w, s_.h); G::BitmapData d;
        if (aa_->bmp.LockBits(&all, G::ImageLockModeWrite, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < s_.h; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(d.Scan0) + y * d.Stride);
                for (int x = 0; x < s_.w; ++x) row[x] = s_.bits[y * s_.w + x] | 0xFF000000u;
            }
            aa_->bmp.UnlockBits(&d);
        }
        aa_->g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        aa_->g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
        aa_->g.ScaleTransform(G::REAL(scale_), G::REAL(scale_));
    }
    void endSmooth() override
    {
        if (!aa_) return;
        aa_->g.Flush(G::FlushIntentionSync);
        G::Rect all(0, 0, s_.w, s_.h); G::BitmapData d;
        if (aa_->bmp.LockBits(&all, G::ImageLockModeRead, PixelFormat32bppPARGB, &d) == G::Ok) {
            for (int y = 0; y < s_.h; ++y) std::memcpy(s_.bits + y * s_.w, static_cast<uint8_t*>(d.Scan0) + y * d.Stride, size_t(s_.w) * 4);
            aa_->bmp.UnlockBits(&d);
        }
        aa_.reset();
    }

    void drawLayer(Layer l) override
    {
        if (Surface* src = l == Layer::Panel ? panel_ : lit_) BitBlt(s_.dc, 0, 0, kLogicalW, kLogicalH, src->dc, 0, 0, SRCCOPY);
    }
    void copyLayerRect(Layer l, const Rect& r) override
    {
        if (Surface* src = l == Layer::Panel ? panel_ : lit_) BitBlt(s_.dc, r.l, r.t, r.w(), r.h(), src->dc, r.l, r.t, SRCCOPY);
    }

private:
    struct Smooth {
        G::Bitmap bmp; G::Graphics g;
        Smooth(int w, int h) : bmp(w, h, PixelFormat32bppPARGB), g(&bmp) {}
    };
    void flushSmooth() { endSmooth(); beginSmooth(); }   // text inside a smooth group (rare): commit AA first
    void applyScale()
    {
        SetGraphicsMode(s_.dc, GM_ADVANCED);
        XFORM x{float(scale_), 0, 0, float(scale_), 0, 0};
        SetWorldTransform(s_.dc, &x);
    }
    Surface& s_;
    double scale_;
    Fonts& fonts_;
    Surface *panel_, *lit_;
    std::unique_ptr<Smooth> aa_;
};

// ---------------------------------------------------------------- Win32 host
class Editor final : public EditorView, public EditorHost {
public:
    explicit Editor(Controller* c) : EditorView(c, nullptr), core_(c, this)
    {
        ViewRect r(0, 0, core_.pixelWidth(), core_.pixelHeight()); setRect(r);
        wchar_t page[16] = L"";                     // screenshot tooling (CI): open on the settings page
        if (GetEnvironmentVariableW(L"ASB_PREVIEW_PAGE", page, 16) && wcscmp(page, L"settings") == 0) core_.setPage(true);
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
        hwnd_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, core_.pixelWidth(), core_.pixelHeight(),
                                static_cast<HWND>(parent), nullptr, moduleInstance(), this);
        if (!hwnd_) { gdipRelease(); return kResultFalse; }
        return EditorView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override
    {
        if (hwnd_) { KillTimer(hwnd_, 1); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        freeSurfaces();
        gdipRelease();
        return EditorView::removed();
    }
    tresult PLUGIN_API onWheel(float distance) override { core_.wheel(-1, -1, distance > 0 ? 1 : -1, false); return kResultTrue; }

    // EditorHost
    void repaint() override { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void layersChanged() override
    {
        if (!hwnd_) return;
        freeSurfaces(); buildSurfaces();
        const int w = core_.pixelWidth(), h = core_.pixelHeight();
        RECT cur; GetClientRect(hwnd_, &cur);
        if (cur.right != w || cur.bottom != h) {
            SetWindowPos(hwnd_, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            ViewRect r(0, 0, w, h);
            if (plugFrame) plugFrame->resizeView(this, &r); else setRect(r);
        }
        repaint();
    }
    void openUrl(const std::string& url) override { ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void captureMouse(bool on) override { if (on) SetCapture(hwnd_); else ReleaseCapture(); }

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

    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        const double s = core_.scale();
        const int x = int(GET_X_LPARAM(lp) / s), y = int(GET_Y_LPARAM(lp) / s);
        switch (msg) {
        case WM_CREATE: hwnd_ = h; buildSurfaces(); SetTimer(h, 1, kFrameMs, nullptr); core_.opened(); return 0;
        case WM_TIMER: core_.tick(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(); return 0;
        case WM_LBUTTONDOWN: SetFocus(h); core_.mouseDown(x, y, (wp & MK_SHIFT) != 0, false); return 0;
        case WM_LBUTTONDBLCLK: core_.mouseDown(x, y, (wp & MK_SHIFT) != 0, true); return 0;
        case WM_MOUSEMOVE:
            core_.mouseMove(x, y, (wp & MK_SHIFT) != 0);
            if (!tracking_) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); tracking_ = true; }
            return 0;
        case WM_MOUSELEAVE: tracking_ = false; core_.mouseLeave(); return 0;
        case WM_LBUTTONUP: core_.mouseUp(); return 0;
        case WM_CAPTURECHANGED: if (reinterpret_cast<HWND>(lp) != h) core_.captureLost(); return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &p);
            core_.wheel(int(p.x / s), int(p.y / s), GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            return 0;
        }
        case WM_KEYDOWN: if (wp == VK_ESCAPE) { core_.escape(); return 0; } break;
        case WM_DESTROY: KillTimer(h, 1); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void buildSurfaces()
    {
        HDC screen = GetDC(hwnd_);
        const int w = core_.pixelWidth(), h = core_.pixelHeight();
        panel_.create(screen, w, h); lit_.create(screen, w, h); back_.create(screen, w, h);
        ReleaseDC(hwnd_, screen);
        fonts_.create(hwnd_);
        { GdiCanvas c(panel_, core_.scale(), fonts_, nullptr, nullptr); core_.paintPanel(c); }
        GdiFlush();
        std::memcpy(lit_.bits, panel_.bits, size_t(w) * h * 4);
        { GdiCanvas c(lit_, core_.scale(), fonts_, nullptr, nullptr); core_.paintLit(c); }
    }
    void freeSurfaces() { panel_.free(); lit_.free(); back_.free(); fonts_.free(); }

    void paint()
    {
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd_, &ps);
        if (back_.dc) {
            { GdiCanvas c(back_, core_.scale(), fonts_, &panel_, &lit_); core_.paintFrame(c); }
            ModifyWorldTransform(back_.dc, nullptr, MWT_IDENTITY);
            BitBlt(wdc, 0, 0, back_.w, back_.h, back_.dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd_, &ps);
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        g_paintNs.fetch_add(uint64_t((t1.QuadPart - t0.QuadPart) * 1'000'000'000.0 / fq.QuadPart));
        g_paintCount.fetch_add(1);
    }

    EditorCore core_;
    HWND hwnd_ = nullptr;
    Surface panel_, lit_, back_;
    Fonts fonts_;
    bool tracking_ = false;
};

} // namespace

IPlugView* createEditor(Controller* c) { return new Editor(c); }

} // namespace asb
