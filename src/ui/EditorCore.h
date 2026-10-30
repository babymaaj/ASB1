// ASB window logic shared by every platform: layout, state, meters, settings page, preferences, input.
// Painting goes through ui::Canvas; windowing/timers/mouse capture through EditorHost.
#pragma once
#include "ui/Canvas.h"
#include "engine/StreamEngine.h"
#include <string>

namespace asb {
class Controller;

namespace ui {

constexpr int kLogicalW = 460, kLogicalH = 286;
constexpr int kFrameMs = 33;                       // ~30 fps while open

class EditorHost {
public:
    virtual ~EditorHost() = default;
    virtual void repaint() = 0;                    // schedule a frame
    virtual void layersChanged() = 0;              // accent/size changed: rebuild cached layers (and resize if needed)
    virtual void openUrl(const std::string& url) = 0;
    virtual void captureMouse(bool on) = 0;
};

// Preferences file I/O is platform specific (location, encoding); parsing is shared.
std::string readPrefsFile();                       // whole file, empty if none
void writePrefsFile(const std::string& contents);

struct Prefs {
    int accent = 0, scale = 0;
    bool autoUpdate = true;
    long long lastCheck = 0;                       // unix seconds of the last automatic update check
    std::string latestTag;                         // newest release seen (header can show UPDATE offline)
};
Prefs parsePrefs(const std::string& ini);
std::string formatPrefs(const Prefs& p);

constexpr int kAccentCount = 6, kScaleCount = 4;
extern const Color kAccents[kAccentCount];
extern const char* const kAccentNames[kAccentCount];
extern const double kScales[kScaleCount];

class EditorCore {
public:
    EditorCore(Controller* c, EditorHost* host);

    double scale() const { return kScales[prefs_.scale]; }
    int pixelWidth() const;                        // logical size x UI scale (before display pixel density)
    int pixelHeight() const;

    void opened();                                 // window is up: maybe start the daily update check
    void tick();                                   // 30 Hz: poll state, request a frame if anything visible changed

    void paintPanel(Canvas& c);                    // -> Layer::Panel
    void paintLit(Canvas& c);                      // -> Layer::Lit (drawn on top of a copy of Panel)
    void paintFrame(Canvas& c);

    // input, logical coordinates
    void mouseDown(int x, int y, bool shift, bool doubleClick);
    void mouseMove(int x, int y, bool shift);
    void mouseUp();
    void mouseLeave();
    void wheel(int x, int y, int steps, bool shift);
    void escape();
    void captureLost();

    void setPage(bool settings) { settings_ = settings; dirty(); }   // tooling (screenshots)
    Color accent() const { return kAccents[prefs_.accent]; }

private:
    struct Shown {
        int on = -1, dev = -1, status = -1, levelPos = -1, hover = -1000, list = -1, scroll = -1;
        int lit[2]{-1, -1}, hold[2]{-1, -1}, peak10 = 0, rateHz = 0, latMs = -1;
        int settings = -1, latency = -1, channels = -1, accent = -1, scale = -1, upd = -1, updAvail = -1, autoUpd = -1;
        uint64_t drops = ~0ull;
        bool operator==(const Shown& o) const;
    };

    void dirty();
    void savePrefs();
    void nudgeLevel(double db);
    void setLevelFromX(int x, bool fine);
    int deviceIndex() const;
    int deviceCount() const;
    void selectDevice(int i);
    void stepDevice(int d);
    Rect listRect() const;
    int maxScroll() const;
    void openList();
    int listRowAt(int x, int y) const;
    int hoverAt(int x, int y) const;
    bool updateAvailable() const;
    void openReleasePage();
    void settingsDown(int x, int y);
    Color ledColorAt(int i) const;

    // painting helpers
    void hline(Canvas& c, int x0, int x1, int y, Color col);
    void groove(Canvas& c, int x0, int x1, int y);
    void frame(Canvas& c, const Rect& r, Color col);
    void engrave(Canvas& c, const Rect& r, const std::string& t, Color col, Font f, unsigned align, int tracking);
    void stamp(Canvas& c, const Rect& row, const std::string& t);
    void segment(Canvas& c, const Rect& r, const std::string& t, bool sel, bool hot);
    void leds(Canvas& c, bool lit);
    void wordmark(Canvas& c);
    void paintMain(Canvas& c);
    void paintFooter(Canvas& c);
    void paintList(Canvas& c);
    void paintSettings(Canvas& c);
    void paintHeader(Canvas& c);

    Controller* ctl_;
    EditorHost* host_;
    Prefs prefs_;
    Shown shown_{};
    bool live_ = false, forceFrame_ = true, dragging_ = false, listOpen_ = false, settings_ = false;
    int listScroll_ = 0, hover_ = -100, lastDragX_ = 0;
    double lastDragV_ = 0;
    double meter_[2]{-120, -120}, hold_[2]{-120, -120}, holdAge_[2]{0, 0}, peak_ = -120, peakAge_ = 0;
};

} // namespace ui
} // namespace asb
