// editor_preview (Windows): opens the plugin window without a DAW, with a real Processor streaming a test tone.
//   editor_preview                -> window stays open until you close it
//   editor_preview shot.bmp [ms]  -> also saves a screenshot after ms (default 2500) and exits
//   editor_preview --stream       -> turn Streaming on at start (device = system default)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "vst/Controller.h"
#include "vst/Processor.h"
#include "vst/Ids.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace asb { double editorAvgPaintUs(); uint64_t editorPaintCount(); }

static bool saveBmp(HWND h, const char* path)
{
    RECT r; GetClientRect(h, &r);
    const int w = r.right, ht = r.bottom;
    HDC wdc = GetDC(h), dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, ht);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(h, dc, PW_CLIENTONLY);
    BITMAPINFOHEADER bi{sizeof(bi), w, -ht, 1, 24, BI_RGB};
    const int stride = ((w * 3 + 3) & ~3);
    std::vector<unsigned char> px(size_t(stride) * ht);
    GetDIBits(dc, bmp, 0, ht, px.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); ReleaseDC(h, wdc);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, DWORD(sizeof(fh) + sizeof(bi))};
    std::fwrite(&fh, sizeof(fh), 1, f); std::fwrite(&bi, sizeof(bi), 1, f); std::fwrite(px.data(), px.size(), 1, f);
    std::fclose(f);
    return true;
}

// Minimal IPlugFrame so the preview window follows the plugin's size changes (like a DAW does).
struct PreviewFrame : public IPlugFrame {
    HWND hwnd = nullptr;
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override
    {
        RECT wr{0, 0, r->getWidth(), r->getHeight()};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
        return view->onSize(r);
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) { *obj = this; return kResultOk; }
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

int main(int argc, char** argv)
{
    const char* shot = nullptr; int shotMs = 2500; bool stream = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--stream")) stream = true;
        else if (!shot) shot = argv[i];
        else shotMs = std::atoi(argv[i]);
    }

    HostApplication host;
    auto* proc = new asb::Processor;
    auto* ctl = new asb::Controller;
    proc->initialize(&host);
    ctl->initialize(&host);
    proc->connect(ctl);       // processor -> controller (engine link message)
    ctl->connect(proc);

    ProcessSetup ps{kRealtime, kSample32, 256, 48000.0};
    proc->setupProcessing(ps);
    proc->setActive(true);

    // audio thread stand-in: 256-frame blocks of a quiet 440 Hz tone, paced to the wall clock
    std::atomic<bool> run{true};
    std::atomic<int> pendingEnable{stream ? 1 : -1};
    std::thread audio([&] {
        float l[256], r[256], ol[256], orr[256];
        float* in[2] = {l, r}; float* out[2] = {ol, orr};
        AudioBusBuffers ib{}, ob{}; ib.numChannels = ob.numChannels = 2; ib.channelBuffers32 = in; ob.channelBuffers32 = out;
        ProcessData d{}; d.numSamples = 256; d.numInputs = d.numOutputs = 1; d.inputs = &ib; d.outputs = &ob; d.symbolicSampleSize = kSample32;
        double ph = 0;
        auto next = std::chrono::steady_clock::now();
        while (run) {
            static double t = 0;
            for (int i = 0; i < 256; ++i) {   // tone with a pumping envelope so the meters move like music
                const float env = float(0.05 + 0.6 * std::pow(0.5 + 0.5 * std::sin(t * 2 * 3.14159265358979 * 1.7), 3.0));
                l[i] = env * float(std::sin(ph)); r[i] = 0.8f * l[i];
                ph += 2 * 3.14159265358979 * 440.0 / 48000.0; t += 1.0 / 48000.0;
            }
            ParameterChanges pc(4);
            // mirror UI edits into the processor like a host does (here: poll the controller's values)
            static double lastEn = -1, lastLvl = -1, lastDev = -1;
            const double en = pendingEnable >= 0 ? double(pendingEnable.exchange(-1)) : ctl->getParamNormalized(asb::kParamStreamEnable);
            const double lvl = ctl->getParamNormalized(asb::kParamOutputLevel), dev = ctl->getParamNormalized(asb::kParamDevice);
            int32 idx;
            if (en != lastEn)  { pc.addParameterData(asb::kParamStreamEnable, idx)->addPoint(0, en, idx); lastEn = en; ctl->setParamNormalized(asb::kParamStreamEnable, en); }
            if (lvl != lastLvl){ pc.addParameterData(asb::kParamOutputLevel, idx)->addPoint(0, lvl, idx); lastLvl = lvl; }
            if (dev != lastDev){ pc.addParameterData(asb::kParamDevice, idx)->addPoint(0, dev, idx); lastDev = dev; }
            d.inputParameterChanges = &pc;
            proc->process(d);
            next += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(next);
        }
    });

    IPlugView* view = ctl->createView(ViewType::kEditor);
    if (!view) { std::printf("no editor\n"); return 1; }
    ViewRect vr; view->getSize(&vr);
    RECT wr{0, 0, vr.getWidth(), vr.getHeight()};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND frame = CreateWindowExW(0, L"STATIC", L"ASB (preview)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 100, 100, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    static PreviewFrame pf; pf.hwnd = frame;
    view->setFrame(&pf);
    if (view->attached(frame, kPlatformTypeHWND) != kResultOk) { std::printf("attach failed\n"); return 1; }

    const ULONGLONG t0 = GetTickCount64();
    MSG m;
    for (;;) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (!IsWindow(frame)) break;
        if (shot && GetTickCount64() - t0 > ULONGLONG(shotMs)) {
            HWND child = GetWindow(frame, GW_CHILD);
            std::printf(saveBmp(child ? child : frame, shot) ? "saved %s\n" : "save failed %s\n", shot);
            break;
        }
        Sleep(10);
    }
    std::printf("editor frames painted: %llu, avg paint %.0f us\n", (unsigned long long)asb::editorPaintCount(), asb::editorAvgPaintUs());
    view->removed();
    view->release();
    run = false; audio.join();
    proc->setActive(false);
    ctl->disconnect(proc); proc->disconnect(ctl);
    ctl->terminate(); proc->terminate();
    ctl->release(); proc->release();
    return 0;
}
