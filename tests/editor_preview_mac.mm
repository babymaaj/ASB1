// editor_preview (macOS): opens the plugin window without a DAW, with a real Processor streaming a test tone.
//   editor_preview_mac shot.png [ms] [--stream]    saves a PNG of the plugin view after ms (default 2500) and exits
// ASB_PREVIEW_PAGE=settings opens on the settings page.
#import <Cocoa/Cocoa.h>
#include "vst/Controller.h"
#include "vst/Processor.h"
#include "vst/Ids.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/gui/iplugview.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace asb { double editorAvgPaintUs(); uint64_t editorPaintCount(); }

struct PreviewFrame : public IPlugFrame {
    NSWindow* win = nil;
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* r) override
    {
        [win setContentSize:NSMakeSize(r->getWidth(), r->getHeight())];
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
    @autoreleasepool {
        const char* shot = nullptr; int shotMs = 2500; bool stream = false;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--stream")) stream = true;
            else if (!shot) shot = argv[i];
            else shotMs = std::atoi(argv[i]);
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        HostApplication host;
        auto* proc = new asb::Processor;
        auto* ctl = new asb::Controller;
        proc->initialize(&host);
        ctl->initialize(&host);
        proc->connect(ctl);
        ctl->connect(proc);
        ProcessSetup ps{kRealtime, kSample32, 256, 48000.0};
        proc->setupProcessing(ps);
        proc->setActive(true);

        std::atomic<bool> run{true};
        std::atomic<int> pendingEnable{stream ? 1 : -1};
        std::thread audio([&] {
            float l[256], r[256], ol[256], orr[256];
            float* in[2] = {l, r}; float* out[2] = {ol, orr};
            AudioBusBuffers ib{}, ob{}; ib.numChannels = ob.numChannels = 2; ib.channelBuffers32 = in; ob.channelBuffers32 = out;
            ProcessData d{}; d.numSamples = 256; d.numInputs = d.numOutputs = 1; d.inputs = &ib; d.outputs = &ob; d.symbolicSampleSize = kSample32;
            double ph = 0, t = 0, lastEn = -1, lastLvl = -1, lastDev = -1;
            auto next = std::chrono::steady_clock::now();
            while (run) {
                for (int i = 0; i < 256; ++i) {
                    const float env = float(0.05 + 0.6 * std::pow(0.5 + 0.5 * std::sin(t * 2 * M_PI * 1.7), 3.0));
                    l[i] = env * float(std::sin(ph)); r[i] = 0.8f * l[i];
                    ph += 2 * M_PI * 440.0 / 48000.0; t += 1.0 / 48000.0;
                }
                ParameterChanges pc(4);
                const double en = pendingEnable >= 0 ? double(pendingEnable.exchange(-1)) : ctl->getParamNormalized(asb::kParamStreamEnable);
                const double lvl = ctl->getParamNormalized(asb::kParamOutputLevel), dev = ctl->getParamNormalized(asb::kParamDevice);
                int32 idx;
                if (en != lastEn) { pc.addParameterData(asb::kParamStreamEnable, idx)->addPoint(0, en, idx); lastEn = en; ctl->setParamNormalized(asb::kParamStreamEnable, en); }
                if (lvl != lastLvl) { pc.addParameterData(asb::kParamOutputLevel, idx)->addPoint(0, lvl, idx); lastLvl = lvl; }
                if (dev != lastDev) { pc.addParameterData(asb::kParamDevice, idx)->addPoint(0, dev, idx); lastDev = dev; }
                d.inputParameterChanges = &pc;
                proc->process(d);
                next += std::chrono::microseconds(5333);
                std::this_thread::sleep_until(next);
            }
        });

        int rc = 0;
        IPlugView* view = ctl->createView(ViewType::kEditor);
        if (view) {
            ViewRect vr; view->getSize(&vr);
            NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, vr.getWidth(), vr.getHeight())
                                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                          backing:NSBackingStoreBuffered defer:NO];
            win.title = @"ASB (preview)";
            static PreviewFrame pf; pf.win = win;
            view->setFrame(&pf);
            if (view->attached((__bridge void*)win.contentView, kPlatformTypeNSView) == kResultOk) {
                [win makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:shotMs / 1000.0]];
                if (shot) {
                    NSView* v = win.contentView.subviews.firstObject;
                    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
                    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
                    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                    rc = [png writeToFile:[NSString stringWithUTF8String:shot] atomically:YES] ? 0 : 2;
                    std::printf(rc ? "save failed %s\n" : "saved %s (%dx%d px)\n", shot, int(rep.pixelsWide), int(rep.pixelsHigh));
                }
                std::printf("editor frames painted: %llu, avg paint %.0f us\n", (unsigned long long)asb::editorPaintCount(), asb::editorAvgPaintUs());
                view->removed();
            } else { std::printf("attach failed\n"); rc = 1; }
            view->release();
            [win close];
        } else { std::printf("no editor\n"); rc = 1; }

        run = false; audio.join();
        proc->setActive(false);
        ctl->disconnect(proc); proc->disconnect(ctl);
        ctl->terminate(); proc->terminate();
        ctl->release(); proc->release();
        return rc;
    }
}
