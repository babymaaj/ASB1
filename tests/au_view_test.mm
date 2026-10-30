// Loads ASB as an Audio Unit the way Logic/GarageBand do (AudioComponent + AU Cocoa UI factory), opens its view
// in a window and saves a PNG. Proves the AU wrapper hosts our editor.   au_view_test out.png
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IONBF, 0);
        const char* out = argc > 1 ? argv[1] : "au.png";
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        AudioComponentDescription d{kAudioUnitType_Effect, 'Asb1', 'Maai', 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp) { std::printf("FAIL: ASB Audio Unit not registered\n"); return 1; }
        CFStringRef name = nullptr; AudioComponentCopyName(comp, &name);
        std::printf("found AU: %s\n", name ? [(__bridge NSString*)name UTF8String] : "?");
        std::printf("step: instantiate\n");
        AudioUnit au = nullptr;
        if (AudioComponentInstanceNew(comp, &au) != noErr || !au) { std::printf("FAIL: instantiate\n"); return 1; }
        std::printf("step: initialize\n");
        if (AudioUnitInitialize(au) != noErr) { std::printf("FAIL: initialize\n"); return 1; }

        std::printf("step: cocoa ui info\n");
        UInt32 size = 0; Boolean writable = false;
        if (AudioUnitGetPropertyInfo(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, &size, &writable) != noErr || !size) {
            std::printf("FAIL: no Cocoa UI\n"); return 1;
        }
        auto* info = static_cast<AudioUnitCocoaViewInfo*>(std::malloc(size));
        AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0, info, &size);
        NSURL* url = (__bridge NSURL*)info->mCocoaAUViewBundleLocation;
        NSString* cls = (__bridge NSString*)info->mCocoaAUViewClass[0];
        std::printf("step: view factory %s in %s\n", cls.UTF8String, url.path.UTF8String);
        Class factoryClass = [[NSBundle bundleWithURL:url] classNamed:cls];
        id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
        std::printf("step: create view\n");
        NSView* view = [factory uiViewForAudioUnit:au withSize:NSZeroSize];
        if (!view) { std::printf("FAIL: view factory returned nil\n"); return 1; }
        std::printf("AU view: %.0f x %.0f\n", view.frame.size.width, view.frame.size.height);

        NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, view.frame.size.width, view.frame.size.height)
                                                    styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        win.title = @"ASB (Audio Unit)";
        std::printf("step: show window\n");
        [win.contentView addSubview:view];
        [win makeKeyAndOrderFront:nil];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.5]];
        NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        const bool ok = [png writeToFile:[NSString stringWithUTF8String:out] atomically:YES];
        std::printf(ok ? "saved %s\n" : "FAIL: save %s\n", out);

        std::printf("step: close\n");
        [view removeFromSuperview];
        [win close];
        std::printf("step: dispose\n");
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        std::free(info);
        if (name) CFRelease(name);
        return ok ? 0 : 1;
    }
}
