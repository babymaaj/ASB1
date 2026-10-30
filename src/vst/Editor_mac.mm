// macOS host for the ASB window: NSView + CoreGraphics/CoreText canvas around the shared ui::EditorCore.
// The panel and the "all LEDs lit" layer are rendered once into bitmaps (Retina-aware) and blitted per frame.
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>

#include "vst/Editor.h"
#include "vst/Controller.h"
#include "ui/EditorCore.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <mach/mach_time.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Objective-C class names are process-global: make ours unique so another build/plugin can't collide.
#define ASBView ASBView_maai_asb_v060

namespace asb {

static std::atomic<uint64_t> g_paintCount{0}, g_paintNs{0};
double editorAvgPaintUs() { const uint64_t c = g_paintCount.load(); return c ? g_paintNs.load() / 1000.0 / double(c) : 0.0; }
uint64_t editorPaintCount() { return g_paintCount.load(); }

namespace ui {
// ---- preferences file: ~/Library/Application Support/maai/ASB/settings.ini
static std::string prefsPath(bool create)
{
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::string dir = std::string(home) + "/Library/Application Support/maai";
    if (create) mkdir(dir.c_str(), 0755);
    dir += "/ASB";
    if (create) mkdir(dir.c_str(), 0755);
    return dir + "/settings.ini";
}
std::string readPrefsFile()
{
    std::ifstream f(prefsPath(false), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
void writePrefsFile(const std::string& s)
{
    std::ofstream f(prefsPath(true), std::ios::binary | std::ios::trunc);
    f << s;
}
} // namespace ui

namespace {
using namespace asb::ui;

CGColorRef cgColor(Color c, int a = 255)
{
    return CGColorCreateGenericRGB(red(c) / 255.0, green(c) / 255.0, blue(c) / 255.0, a / 255.0);   // caller releases
}

struct MacFonts {
    CTFontRef f[6]{};
    MacFonts()
    {
        auto mk = [](CFStringRef name, CGFloat size) { return CTFontCreateWithName(name, size, nullptr); };   // falls back if missing
        f[int(Font::Label)]  = mk(CFSTR("DINAlternate-Bold"), 11);
        f[int(Font::Brand)]  = mk(CFSTR("AvenirNext-Regular"), 13);
        f[int(Font::Value)]  = mk(CFSTR("AvenirNext-Regular"), 15);
        f[int(Font::Small)]  = mk(CFSTR("AvenirNext-Regular"), 12);
        f[int(Font::Button)] = mk(CFSTR("DINAlternate-Bold"), 13);
        f[int(Font::Num)]    = mk(CFSTR("AvenirNextCondensed-Regular"), 14);
    }
    ~MacFonts() { for (auto* x : f) if (x) CFRelease(x); }
};

// Draw an upright CGImage into a y-down context.
void drawImageYDown(CGContextRef ctx, CGImageRef img, CGRect r)
{
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width, r.size.height), img);
    CGContextRestoreGState(ctx);
}

class CgCanvas final : public Canvas {
public:
    // ctx must already be in LOGICAL y-down units; devScale = device pixels per logical unit (UI scale x Retina)
    CgCanvas(CGContextRef ctx, double devScale, MacFonts& fonts, CGImageRef panel, CGImageRef lit)
        : ctx_(ctx), dev_(devScale), fonts_(fonts), panel_(panel), lit_(lit) {}

    void fill(const Rect& r, Color c) override
    {
        CGContextSetShouldAntialias(ctx_, false);           // crisp rectangles / hairlines
        CGContextSetRGBFillColor(ctx_, red(c) / 255.0, green(c) / 255.0, blue(c) / 255.0, 1);
        CGContextFillRect(ctx_, CGRectMake(r.l, r.t, r.w(), r.h()));
        CGContextSetShouldAntialias(ctx_, true);
    }

    void text(const Rect& r, const std::string& utf8, Color c, Font f, unsigned align, int tracking) override
    {
        CFStringRef str = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.data()), CFIndex(utf8.size()), kCFStringEncodingUTF8, false);
        if (!str) return;
        CGColorRef col = cgColor(c);
        CFNumberRef kern = nullptr;
        const double kv = tracking;
        kern = CFNumberCreate(nullptr, kCFNumberDoubleType, &kv);
        const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
        const void* vals[] = {fonts_.f[int(f)], col, kern};
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGFloat asc = 0, desc = 0, lead = 0;
        double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead);
        if (tracking && !utf8.empty()) w -= tracking;       // kern is added after the last glyph too
        if ((align & Ellipsis) && w > r.w()) {
            CFAttributedStringRef dots = CFAttributedStringCreate(nullptr, CFSTR("…"), attrs);
            CTLineRef token = CTLineCreateWithAttributedString(dots);
            CTLineRef cut = CTLineCreateTruncatedLine(line, r.w(), kCTLineTruncationEnd, token);
            if (cut) { CFRelease(line); line = cut; w = CTLineGetTypographicBounds(line, &asc, &desc, &lead); }
            CFRelease(token); CFRelease(dots);
        }
        double x = r.l;
        if (align & Center) x = (r.l + r.r - w) / 2; else if (align & Right) x = r.r - w;
        double base = r.t + asc;
        if (align & VCenter) base = (r.t + r.b) / 2.0 + (asc - desc) / 2.0;
        else if (align & Bottom) base = r.b - desc;
        CGContextSaveGState(ctx_);
        CGContextSetTextMatrix(ctx_, CGAffineTransformMakeScale(1, -1));   // y-down context
        CGContextSetTextPosition(ctx_, x, base);
        CTLineDraw(line, ctx_);
        CGContextRestoreGState(ctx_);
        CFRelease(line); CFRelease(as); CFRelease(attrs); CFRelease(kern); CGColorRelease(col); CFRelease(str);
    }

    void ellipse(float x, float y, float w, float h, Color c, int alpha) override
    {
        CGContextSetRGBFillColor(ctx_, red(c) / 255.0, green(c) / 255.0, blue(c) / 255.0, alpha / 255.0);
        CGContextFillEllipseInRect(ctx_, CGRectMake(x, y, w, h));
    }

    void strokes(const std::vector<Polyline>& figs, float width, Color c, int alpha, float dx, float dy) override
    {
        CGContextSaveGState(ctx_);
        CGContextSetAlpha(ctx_, alpha / 255.0);
        CGContextBeginTransparencyLayer(ctx_, nullptr);     // overlapping strokes merge before the alpha is applied
        CGMutablePathRef p = CGPathCreateMutable();
        for (const auto& f : figs) {
            if (f.size() < 2) continue;
            CGPathMoveToPoint(p, nullptr, f[0].x + dx, f[0].y + dy);
            for (size_t i = 1; i < f.size(); ++i) CGPathAddLineToPoint(p, nullptr, f[i].x + dx, f[i].y + dy);
        }
        CGContextAddPath(ctx_, p);
        CGContextSetLineWidth(ctx_, width);
        CGContextSetLineCap(ctx_, kCGLineCapRound);
        CGContextSetLineJoin(ctx_, kCGLineJoinRound);
        CGContextSetRGBStrokeColor(ctx_, red(c) / 255.0, green(c) / 255.0, blue(c) / 255.0, 1);
        CGContextStrokePath(ctx_);
        CGPathRelease(p);
        CGContextEndTransparencyLayer(ctx_);
        CGContextRestoreGState(ctx_);
    }

    void perforation(int step, Color c) override
    {
        // one dot every `step` logical px, one UI pixel big (same density as Windows)
        const double px = std::max(1.0, dev_ / 2.0) / dev_;
        std::vector<CGRect> rs;
        rs.reserve(size_t(kLogicalW / step + 1) * size_t(kLogicalH / step + 1));
        for (int y = 2; y < kLogicalH; y += step)
            for (int x = ((y / step) % 2) ? step : step / 2; x < kLogicalW; x += step) rs.push_back(CGRectMake(x, y, px, px));
        CGContextSetShouldAntialias(ctx_, false);
        CGContextSetRGBFillColor(ctx_, red(c) / 255.0, green(c) / 255.0, blue(c) / 255.0, 1);
        CGContextFillRects(ctx_, rs.data(), rs.size());
        CGContextSetShouldAntialias(ctx_, true);
    }

    void drawLayer(Layer l) override
    {
        if (CGImageRef img = l == Layer::Panel ? panel_ : lit_) drawImageYDown(ctx_, img, CGRectMake(0, 0, kLogicalW, kLogicalH));
    }
    void copyLayerRect(Layer l, const Rect& r) override
    {
        CGImageRef img = l == Layer::Panel ? panel_ : lit_;
        if (!img) return;
        CGContextSaveGState(ctx_);
        CGContextClipToRect(ctx_, CGRectMake(r.l, r.t, r.w(), r.h()));
        CGContextSetInterpolationQuality(ctx_, kCGInterpolationNone);
        drawImageYDown(ctx_, img, CGRectMake(0, 0, kLogicalW, kLogicalH));
        CGContextRestoreGState(ctx_);
    }

private:
    CGContextRef ctx_;
    double dev_;
    MacFonts& fonts_;
    CGImageRef panel_, lit_;
};

} // namespace
class MacEditor;
} // namespace asb

static bool shiftOf(NSEvent* e) { return (e.modifierFlags & NSEventModifierFlagShift) != 0; }

// ---------------------------------------------------------------- NSView
@interface ASBView : NSView {
@public
    asb::MacEditor* owner;
}
@end

namespace asb {
using namespace asb::ui;

class MacEditor final : public EditorView, public EditorHost {
public:
    explicit MacEditor(Controller* c) : EditorView(c, nullptr), core_(c, this)
    {
        ViewRect r(0, 0, core_.pixelWidth(), core_.pixelHeight()); setRect(r);
        if (const char* p = std::getenv("ASB_PREVIEW_PAGE"); p && std::string(p) == "settings") core_.setPage(true);
    }
    ~MacEditor() override { freeLayers(); }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override
    {
        return (type && std::strcmp(type, kPlatformTypeNSView) == 0) ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override
    {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent) return kResultFalse;
        view_ = [[ASBView alloc] initWithFrame:NSMakeRect(0, 0, core_.pixelWidth(), core_.pixelHeight())];
        view_->owner = this;
        [static_cast<NSView*>(parent) addSubview:view_];
        __weak ASBView* weakView = view_;
        timer_ = [NSTimer timerWithTimeInterval:kFrameMs / 1000.0 repeats:YES block:^(NSTimer*) {
            ASBView* v = weakView;
            if (v && v->owner) v->owner->core().tick();
        }];
        [[NSRunLoop currentRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];   // keeps ticking during drags/menus
        core_.opened();
        return EditorView::attached(parent, type);
    }
    tresult PLUGIN_API removed() override
    {
        [timer_ invalidate]; timer_ = nil;
        if (view_) { view_->owner = nullptr; [view_ removeFromSuperview]; view_ = nil; }
        freeLayers();
        return EditorView::removed();
    }
    tresult PLUGIN_API onWheel(float distance) override { core_.wheel(-1, -1, distance > 0 ? 1 : -1, false); return kResultTrue; }

    // EditorHost
    void repaint() override { [view_ setNeedsDisplay:YES]; }
    void layersChanged() override
    {
        freeLayers();
        const int w = core_.pixelWidth(), h = core_.pixelHeight();
        if (view_ && (int(view_.frame.size.width) != w || int(view_.frame.size.height) != h)) {
            [view_ setFrameSize:NSMakeSize(w, h)];
            ViewRect r(0, 0, w, h);
            if (plugFrame) plugFrame->resizeView(this, &r); else setRect(r);
        }
        repaint();
    }
    void openUrl(const std::string& url) override
    {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (u) [[NSWorkspace sharedWorkspace] openURL:u];
    }
    void captureMouse(bool) override {}           // AppKit routes drags to the view that got mouseDown

    EditorCore& core() { return core_; }
    void backingChanged() { freeLayers(); repaint(); }

    void draw(CGContextRef ctx, double backing)
    {
        const uint64_t t0 = mach_absolute_time();
        const double s = core_.scale(), dev = s * backing;
        if (!panel_ || dev != layerScale_) buildLayers(dev);
        CGContextSaveGState(ctx);
        CGContextScaleCTM(ctx, s, s);                 // view is flipped (y down); now in logical units
        CgCanvas c(ctx, dev, fonts_, panel_, lit_);
        core_.paintFrame(c);
        CGContextRestoreGState(ctx);
        static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        g_paintNs.fetch_add((mach_absolute_time() - t0) * tb.numer / tb.denom);
        g_paintCount.fetch_add(1);
    }

private:
    // Render a layer into a device-pixel bitmap (y-down logical drawing) and return it as an upright image.
    template <class F> CGImageRef renderLayer(double dev, CGImageRef base, F&& paint)
    {
        const size_t w = size_t(std::lround(kLogicalW * dev)), h = size_t(std::lround(kLogicalH * dev));
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef bc = CGBitmapContextCreate(nullptr, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGColorSpaceRelease(cs);
        if (!bc) return nullptr;
        CGContextTranslateCTM(bc, 0, CGFloat(h));
        CGContextScaleCTM(bc, dev, -dev);             // logical, y down
        CgCanvas c(bc, dev, fonts_, base, nullptr);
        if (base) c.drawLayer(Layer::Panel);
        paint(c);
        CGImageRef img = CGBitmapContextCreateImage(bc);
        CGContextRelease(bc);
        return img;
    }
    void buildLayers(double dev)
    {
        freeLayers();
        layerScale_ = dev;
        panel_ = renderLayer(dev, nullptr, [&](Canvas& c) { core_.paintPanel(c); });
        lit_ = renderLayer(dev, panel_, [&](Canvas& c) { core_.paintLit(c); });
    }
    void freeLayers()
    {
        if (panel_) { CGImageRelease(panel_); panel_ = nullptr; }
        if (lit_) { CGImageRelease(lit_); lit_ = nullptr; }
    }

    EditorCore core_;
    ASBView* view_ = nil;
    NSTimer* timer_ = nil;
    MacFonts fonts_;
    CGImageRef panel_ = nullptr, lit_ = nullptr;
    double layerScale_ = 0;
};

IPlugView* createEditor(Controller* c) { return new MacEditor(c); }

} // namespace asb

@implementation ASBView {
    NSTrackingArea* tracking_;
    double wheelAcc_;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)drawRect:(NSRect)dirty
{
    if (!owner) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    owner->draw(ctx, self.window ? self.window.backingScaleFactor : 2.0);
}
- (void)viewDidChangeBackingProperties { if (owner) owner->backingChanged(); }
- (void)updateTrackingAreas
{
    if (tracking_) [self removeTrackingArea:tracking_];
    tracking_ = [[NSTrackingArea alloc] initWithRect:self.bounds
                                             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                               owner:self userInfo:nil];
    [self addTrackingArea:tracking_];
    [super updateTrackingAreas];
}
- (NSPoint)logical:(NSEvent*)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const double s = owner ? owner->core().scale() : 1.0;
    return NSMakePoint(p.x / s, p.y / s);
}
- (void)mouseDown:(NSEvent*)e
{
    if (!owner) return;
    [self.window makeFirstResponder:self];
    const NSPoint p = [self logical:e];
    owner->core().mouseDown(int(p.x), int(p.y), shiftOf(e), e.clickCount == 2);
}
- (void)mouseDragged:(NSEvent*)e { if (owner) { const NSPoint p = [self logical:e]; owner->core().mouseMove(int(p.x), int(p.y), shiftOf(e)); } }
- (void)mouseMoved:(NSEvent*)e { if (owner) { const NSPoint p = [self logical:e]; owner->core().mouseMove(int(p.x), int(p.y), shiftOf(e)); } }
- (void)mouseUp:(NSEvent*)e { if (owner) owner->core().mouseUp(); }
- (void)mouseExited:(NSEvent*)e { if (owner) owner->core().mouseLeave(); }
- (void)scrollWheel:(NSEvent*)e
{
    if (!owner) return;
    wheelAcc_ += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 12.0 : e.scrollingDeltaY;
    const int steps = int(wheelAcc_);
    if (steps == 0) return;
    wheelAcc_ -= steps;
    const NSPoint p = [self logical:e];
    owner->core().wheel(int(p.x), int(p.y), steps, shiftOf(e));
}
- (void)keyDown:(NSEvent*)e
{
    if (owner && e.keyCode == 53) { owner->core().escape(); return; }   // Esc
    [super keyDown:e];
}
@end
