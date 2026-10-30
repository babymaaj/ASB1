// Deterministic + threaded tests for the platform-independent core.
#include "core/DriftResampler.h"
#include "core/SpscFrameRing.h"
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
static void simulateDrift(double hostRate, double devRate, double ppm, double seconds,
                          size_t& minFill, size_t& maxFill, uint64_t& underruns, uint64_t& overflows,
                          double& maxJump)
{
    SpscFrameRing ring(16384);
    DriftResampler rs;
    const size_t target = size_t(hostRate * 0.03); // 30 ms
    rs.configure(&ring, hostRate, devRate, target);
    const double actualHost = hostRate * (1.0 + ppm * 1e-6);
    const size_t hostBlock = 256, devBlock = 480;
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
    underruns = rs.underruns(); overflows = ring.overflowFrames();
}

static void testResampler()
{
    struct Case { double host, dev, ppm; const char* name; };
    const Case cases[] = {
        {48000, 48000, 0,    "48k->48k  0ppm"},
        {44100, 48000, 0,    "44.1k->48k 0ppm"},
        {48000, 44100, 0,    "48k->44.1k 0ppm"},
        {48000, 48000, 300,  "48k->48k +300ppm host fast"},
        {48000, 48000, -300, "48k->48k -300ppm host slow"},
        {44100, 48000, 500,  "44.1k->48k +500ppm"},
    };
    for (const auto& c : cases) {
        size_t mn, mx; uint64_t ur, ov; double jump;
        simulateDrift(c.host, c.dev, c.ppm, 120.0, mn, mx, ur, ov, jump);
        std::printf("resampler %-30s fill[min=%zu max=%zu] underruns=%llu overflow=%llu maxStep=%.4f\n",
                    c.name, mn, mx, (unsigned long long)ur, (unsigned long long)ov, jump);
        CHECK(ur == 0);
        CHECK(ov == 0);
        CHECK(mx < 8192);            // bounded (no unbounded growth)
        CHECK(mn > 128);             // never close to dry
        CHECK(jump < 0.20);          // 1 kHz @ 0.5 amp: max natural step ~0.065-0.07; no clicks
    }
}

int main()
{
    testRingBasic();
    testRingThreaded();
    testResampler();
    std::printf(g_fail ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
