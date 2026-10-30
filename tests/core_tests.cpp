// Deterministic + threaded tests for the platform-independent core.
#include "core/DriftResampler.h"
#include "core/SpscFrameRing.h"
#include "engine/StreamEngine.h"
#include "core/Utf8.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace asb;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static void testRingBasic()
{
    std::printf("ring basic/wrap/overflow\n");
    SpscFrameRing r(64);
    CHECK(r.capacity() == 64);
    std::vector<float> l(100), rr(100), out(200);
    for (int i = 0; i < 100; ++i) { l[i] = float(i); rr[i] = -float(i); }
    CHECK(r.writePlanar(l.data(), rr.data(), 100) == 64);   // overflow -> 36 dropped
    CHECK(r.overflowFrames() == 36);
    CHECK(r.read(out.data(), 40) == 40);
    CHECK(out[0] == 0.f && out[2 * 39] == 39.f && out[2 * 39 + 1] == -39.f);
    CHECK(r.writePlanar(l.data(), rr.data(), 30) == 30);    // wraps
    CHECK(r.read(out.data(), 100) == 54);
    CHECK(out[2 * 24] == 0.f);                              // first of the wrapped write
    const double dl[2] = {1.0, 2.0};
    CHECK(r.writePlanar(dl, dl, 2) == 2);                   // double input path
}

static void testRingThreaded()
{
    std::printf("ring threaded order/integrity (2M frames)\n");
    SpscFrameRing r(4096);
    const uint32_t total = 2'000'000;
    std::thread prod([&] {
        std::vector<float> a(128), b(128);
        uint32_t next = 0;
        while (next < total) {
            const uint32_t n = std::min<uint32_t>(128, total - next);
            for (uint32_t i = 0; i < n; ++i) { a[i] = float(next + i); b[i] = float(next + i) + 0.5f; }
            const size_t w = r.writePlanar(a.data(), b.data(), n);
            // producer must never block; retry only the unwritten tail (test harness behaviour)
            next += uint32_t(w);
            if (w < n) std::this_thread::yield();
            // keep stream contiguous: values derive from `next`, so partial writes are fine
        }
    });
    uint32_t expect = 0; bool ok = true;
    std::vector<float> buf(256 * 2);
    while (expect < total) {
        const size_t n = r.read(buf.data(), 256);
        for (size_t i = 0; i < n; ++i) {
            if (buf[i * 2] != float(expect) || buf[i * 2 + 1] != float(expect) + 0.5f) ok = false;
            ++expect;
        }
        if (!n) std::this_thread::yield();
    }
    prod.join();
    CHECK(ok);
}

// Simulate producer (host clock) and consumer (device clock) with a ppm offset.
static void simulateDrift(double hostRate, double devRate, double ppm, double seconds, size_t hostBlock,
                          size_t& minFill, size_t& maxFill, uint64_t& underruns, uint64_t& overflows, uint64_t& resyncs,
                          double& maxJump)
{
    SpscFrameRing ring(16384);
    DriftResampler rs;
    rs.configure(&ring, hostRate, devRate, targetFillFrames(hostRate, int(hostBlock)), resyncAboveFrames(hostRate, int(hostBlock)));
    const double actualHost = hostRate * (1.0 + ppm * 1e-6);
    const size_t devBlock = 480;
    double hostT = 0, devT = 0, phase = 0;
    std::vector<float> l(hostBlock), out(devBlock * 2);
    minFill = ~size_t(0); maxFill = 0; maxJump = 0;
    float last = 0; bool haveLast = false;
    const double f = 1000.0; // 1 kHz sine
    while (devT < seconds) {
        if (hostT <= devT) {
            for (size_t i = 0; i < hostBlock; ++i) { l[i] = 0.5f * float(std::sin(phase)); phase += 2 * M_PI * f / actualHost; }
            ring.writePlanar(l.data(), l.data(), hostBlock);
            hostT += hostBlock / actualHost;
        } else {
            const size_t got = rs.render(out.data(), devBlock, 1.0f);
            if (devT > 1.0 && rs.primed()) { // after settling
                size_t fl = rs.fillFrames(); minFill = std::min(minFill, fl); maxFill = std::max(maxFill, fl);
            }
            for (size_t i = 0; i < got; ++i) {
                if (haveLast && devT > 1.0) maxJump = std::max(maxJump, double(std::fabs(out[i * 2] - last)));
                last = out[i * 2]; haveLast = true;
            }
            if (got < devBlock) haveLast = false;
            devT += devBlock / devRate;
        }
    }
    underruns = rs.underruns(); overflows = ring.overflowFrames(); resyncs = rs.resyncs();
}

static void testResampler()
{
    struct Case { double host, dev, ppm; size_t block; const char* name; };
    const Case cases[] = {
        {48000, 48000, 0,    256,  "48k->48k  0ppm"},
        {44100, 48000, 0,    256,  "44.1k->48k 0ppm"},
        {48000, 44100, 0,    256,  "48k->44.1k 0ppm"},
        {48000, 48000, 300,  256,  "48k->48k +300ppm host fast"},
        {48000, 48000, -300, 256,  "48k->48k -300ppm host slow"},
        {44100, 48000, 500,  256,  "44.1k->48k +500ppm"},
        {48000, 48000, 200,  1024, "48k->48k +200ppm block 1024"},
        {44100, 48000, -200, 2048, "44.1k->48k -200ppm block 2048"},
    };
    for (const auto& c : cases) {
        size_t mn, mx; uint64_t ur, ov, rsy; double jump;
        simulateDrift(c.host, c.dev, c.ppm, 120.0, c.block, mn, mx, ur, ov, rsy, jump);
        std::printf("resampler %-30s fill[min=%zu max=%zu] underruns=%llu overflow=%llu resyncs=%llu maxStep=%.4f\n",
                    c.name, mn, mx, (unsigned long long)ur, (unsigned long long)ov, (unsigned long long)rsy, jump);
        CHECK(ur == 0);
        CHECK(ov == 0);
        CHECK(rsy == 0);            // steady state must never hard-resync
        CHECK(mx < 8192);            // bounded (no unbounded growth)
        CHECK(mn > 64);              // never close to dry
        CHECK(jump < 0.20);          // 1 kHz @ 0.5 amp: max natural step ~0.065-0.07; no clicks
    }
}

// Consumer stalls (device startup slack, USB hiccup, sleep): the ring backs up far past target.
// Drift control alone (+-0.5%) would take minutes to drain it; the resampler must hard-resync.
static void testStallRecovery()
{
    std::printf("stall recovery (consumer stalls 1 s, then runs)\n");
    const double rate = 48000;
    SpscFrameRing ring(32768);
    DriftResampler rs;
    const size_t target = size_t(rate * 0.03);
    rs.configure(&ring, rate, rate, target);
    std::vector<float> l(256, 0.1f), out(480 * 2);
    double hostT = 0, devT = 0;
    auto run = [&](double untilHost, bool consumerRuns) {
        while (hostT < untilHost) {
            if (!consumerRuns || hostT <= devT) { ring.writePlanar(l.data(), l.data(), 256); hostT += 256 / rate; }
            else { rs.render(out.data(), 480, 1.f); devT += 480 / rate; }
        }
        if (!consumerRuns) devT = hostT;  // device clock keeps going while stalled
    };
    run(1.0, true);
    run(2.0, false);                      // 1 s stall: ~48k frames pile up (ring overflows)
    run(2.5, true);
    const size_t fillAfter = rs.fillFrames();
    run(5.0, true);
    std::printf("  fill 0.5 s after stall=%zu (target %zu) resyncs=%llu underruns=%llu\n", fillAfter, target,
                (unsigned long long)rs.resyncs(), (unsigned long long)rs.underruns());
    CHECK(rs.resyncs() >= 1);
    CHECK(fillAfter < target * 3);   // back near target, not ~0.7 s behind
    CHECK(rs.fillFrames() < target * 3);
}

static void testUtf8()
{
    std::printf("utf8 -> utf16 device names\n");
    CHECK(utf8ToUtf16("Speakers (Focusrite USB)") == u"Speakers (Focusrite USB)");
    CHECK(utf8ToUtf16("Haut-parleurs (Réaltek)") == u"Haut-parleurs (Réaltek)");
    CHECK(utf8ToUtf16("\xe3\x82\xb9\xe3\x83\x94\xe3\x83\xbc\xe3\x82\xab\xe3\x83\xbc") == u"スピーカー");
    CHECK(utf8ToUtf16("cable \xf0\x9f\x8e\xa7") == u"cable \U0001F3A7");          // surrogate pair
    CHECK(utf8ToUtf16("bad\xff\xc0\x80x") == u"bad\uFFFD\uFFFD\uFFFDx");          // invalid + overlong
    CHECK(utf8ToUtf16("cut\xe3\x82") == u"cut\uFFFD");                               // truncated
}

// Interpolation kernel quality at the worst fractional position (0.5).
static void testInterpHf()
{
    std::printf("interpolator HF response @ frac 0.5 (48 kHz)\n");
    for (double f : {5000.0, 10000.0, 15000.0}) {
        const double w = 2 * M_PI * f / 48000.0;
        double peak = 0;
        for (int n = 0; n < 4800; ++n) {   // output sample n sits halfway between input n and n+1
            const float s[4] = {float(std::sin(w * (n - 1))), float(std::sin(w * n)), float(std::sin(w * (n + 1))), float(std::sin(w * (n + 2)))};
            peak = std::max(peak, double(std::fabs(DriftResampler::interp(s, 0.5f))));
        }
        const double db = 20 * std::log10(peak);
        std::printf("  %5.0f Hz: %.2f dB\n", f, db);
        CHECK(db > (f <= 10000 ? -0.8 : -3.0));     // linear interpolation was -2.0 dB @10k, -5.1 dB @15k
    }
}

// Level changes must ramp (no zipper), and audio must fade in after (re)priming (no click).
static void testGainSmoothing()
{
    std::printf("gain smoothing + fade-in\n");
    SpscFrameRing ring(16384);
    DriftResampler rs;
    rs.configure(&ring, 48000, 48000, 1440);
    std::vector<float> dc(256, 0.5f), out(480 * 2);
    float maxStep = 0, last = 0, maxOut = 0;
    auto feedAndRender = [&](float gain, int blocks) {
        for (int b = 0; b < blocks; ++b) {
            while (ring.readableFrames() < 4000) ring.writePlanar(dc.data(), dc.data(), 256);
            const size_t got = rs.render(out.data(), 480, gain);
            for (size_t i = 0; i < got; ++i) { maxStep = std::max(maxStep, std::fabs(out[i * 2] - last)); last = out[i * 2]; maxOut = std::max(maxOut, out[i * 2]); }
        }
    };
    feedAndRender(1.f, 1);                 // prime
    feedAndRender(1.f, 50);                // includes the fade-in from silence
    const float stepFadeIn = maxStep;
    feedAndRender(0.f, 50);                // hard mute request
    feedAndRender(1.f, 50);                // and back
    std::printf("  max per-sample step: fade-in %.4f, overall %.4f (DC 0.5 input)\n", stepFadeIn, maxStep);
    CHECK(stepFadeIn < 0.01f);             // starts from silence without a click
    CHECK(maxStep < 0.01f);                // an instant 0.5 jump would be a zipper/click
    CHECK(std::fabs(last - 0.5f) < 1e-3f); // settles exactly back at full level
}

int main()
{
    testRingBasic();
    testRingThreaded();
    testResampler();
    testStallRecovery();
    testUtf8();
    testInterpHf();
    testGainSmoothing();
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
