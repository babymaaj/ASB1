// Platform-neutral drawing surface for the ASB window. All coordinates are LOGICAL (100 % size); the backend applies
// the UI scale and the display's pixel density. Backends: GDI/GDI+ (Windows), CoreGraphics (macOS), recorder (tests).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace asb::ui {

using Color = uint32_t;                                    // 0xRRGGBB
constexpr Color rgb(int r, int g, int b) { return Color((r & 255) << 16 | (g & 255) << 8 | (b & 255)); }
constexpr int red(Color c) { return int(c >> 16 & 255); }
constexpr int green(Color c) { return int(c >> 8 & 255); }
constexpr int blue(Color c) { return int(c & 255); }

struct Rect {
    int l = 0, t = 0, r = 0, b = 0;
    bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
    int w() const { return r - l; }
    int h() const { return b - t; }
};

struct PointF { float x, y; };
using Polyline = std::vector<PointF>;                      // one open stroke figure

enum class Font { Label, Brand, Value, Small, Button, Num };
enum Align : unsigned { Left = 0, Center = 1, Right = 2, VCenter = 4, Bottom = 8, Ellipsis = 16 };

// Cached layers (backend-owned, rebuilt when the host is told the layers are dirty):
//   Panel = static panel (texture, wordmark, unlit LEDs)    Lit = Panel + all LEDs lit
enum class Layer { Panel, Lit };

class Canvas {
public:
    virtual ~Canvas() = default;
    virtual void fill(const Rect& r, Color c) = 0;
    // text: UTF-8; tracking = extra spacing between letters (logical px), alignment applied to the tracked width
    virtual void text(const Rect& r, const std::string& utf8, Color c, Font f, unsigned align, int tracking = 0) = 0;
    virtual void ellipse(float x, float y, float w, float h, Color c, int alpha = 255) = 0;   // anti-aliased
    // thick strokes with round caps/joins; overlapping figures merge (no double blending), offset by (dx,dy)
    virtual void strokes(const std::vector<Polyline>& figs, float width, Color c, int alpha, float dx = 0, float dy = 0) = 0;
    virtual void perforation(int step, Color c) = 0;        // fine dot texture over the whole surface, pixel-crisp
    // smooth drawing group (GDI+ needs an offscreen round trip; others ignore)
    virtual void beginSmooth() {}
    virtual void endSmooth() {}
    // layers (only during frame painting)
    virtual void drawLayer(Layer) {}
    virtual void copyLayerRect(Layer, const Rect&) {}
};

} // namespace asb::ui
