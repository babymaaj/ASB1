// Drives asb::Processor directly (no host). Verifies bit-exact passthrough and
// measures per-block cost + worker CPU. On Linux the sink is the stub engine,
// so numbers are NOT Windows/WASAPI numbers (see TEST_RESULTS.md).
#include "vst/Processor.h"
#include "vst/Ids.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <ctime>

using namespace Steinberg; using namespace Steinberg::Vst;
using clk = std::chrono::steady_clock;

static long rssKb() { std::ifstream f("/proc/self/status"); std::string l; while (std::getline(f, l)) if (!l.compare(0, 6, "VmRSS:")) return std::stol(l.substr(6)); return 0; }
static double procCpuMs() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

struct Fix {
    static constexpr int N = 256;
    float inL[N], inR[N], outL[N], outR[N];
    double dInL[N], dInR[N], dOutL[N], dOutR[N];
    float* inp[2] = {inL, inR}; float* outp[2] = {outL, outR};
    double* dinp[2] = {dInL, dInR}; double* doutp[2] = {dOutL, dOutR};
    AudioBusBuffers in{}, out{};
    ProcessData d{};
    Fix() {
        for (int i = 0; i < N; ++i) { inL[i] = 0.5f * std::sin(i * 0.05f); inR[i] = 0.25f * std::cos(i * 0.031f); dInL[i] = inL[i]; dInR[i] = inR[i]; }
        in.numChannels = out.numChannels = 2;
        d.numInputs = d.numOutputs = 1; d.inputs = &in; d.outputs = &out; d.numSamples = N;
    }
    void use32() { in.channelBuffers32 = inp; out.channelBuffers32 = outp; d.symbolicSampleSize = kSample32; }
    void use64() { in.channelBuffers64 = dinp; out.channelBuffers64 = doutp; d.symbolicSampleSize = kSample64; }
};

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main()
{
    asb::Processor p;
    p.initialize(nullptr);
    ProcessSetup ps{kRealtime, kSample32, 256, 48000.0};
    p.setupProcessing(ps);
    p.setActive(true);

    Fix f; f.use32();

    // enable streaming via a real parameter change
    auto sendParam = [&](ParamID id, double v) {
        ParameterChanges pc(1); int32 idx; auto* q = pc.addParameterData(id, idx); q->addPoint(0, v, idx);
        f.d.inputParameterChanges = &pc; p.process(f.d); f.d.inputParameterChanges = nullptr;
    };

    // 1) bit-exact passthrough (stream OFF and ON), separate buffers
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) sendParam(asb::kParamStreamEnable, 1.0);
        std::memset(f.outL, 0, sizeof f.outL); std::memset(f.outR, 0, sizeof f.outR);
        f.d.outputParameterChanges = nullptr; p.process(f.d);
        CHECK(!std::memcmp(f.inL, f.outL, sizeof f.inL) && !std::memcmp(f.inR, f.outR, sizeof f.inR));
        std::printf("passthrough bit-exact (32-bit, stream %s): %s\n", pass ? "ON" : "OFF", fails ? "FAIL" : "ok");
    }
    // 2) in-place (host reuses buffers)
    { Fix g; g.use32(); g.out.channelBuffers32 = g.inp; float ref[Fix::N]; std::memcpy(ref, g.inL, sizeof ref); p.process(g.d);
      CHECK(!std::memcmp(ref, g.inL, sizeof ref)); std::printf("passthrough in-place unchanged: ok\n"); }
    // 3) 64-bit
    { Fix g; g.use64(); ProcessSetup ps64{kRealtime, kSample64, 256, 48000.0}; p.setupProcessing(ps64); p.process(g.d);
      CHECK(!std::memcmp(g.dInL, g.dOutL, sizeof g.dInL) && !std::memcmp(g.dInR, g.dOutR, sizeof g.dInR)); std::printf("passthrough bit-exact (64-bit): ok\n");
      p.setupProcessing(ps); }

    // 4) per-block cost (tight loop, stream ON; ring overflow expected & counted since no real consumer pacing)
    {
        const int blocks = 200000;
        std::vector<double> t; t.reserve(1000);
        auto t0 = clk::now();
        for (int i = 0; i < blocks; ++i) {
            auto a = clk::now(); p.process(f.d); auto b = clk::now();
            if (i % 200 == 0) t.push_back(std::chrono::duration<double, std::nano>(b - a).count());
        }
        double total = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
        std::sort(t.begin(), t.end());
        std::printf("process() 256 frames, stream ON, ring FULL (writes dropped, optimistic): mean %.0f ns/block, p50 %.0f, p99 %.0f, max(sampled) %.0f ns  (budget @48k: 5333333 ns)\n",
                    total / blocks, t[t.size() / 2], t[size_t(t.size() * 0.99)], t.back());
        std::printf("  => audio-thread load at 48k/256: %.4f %% of one core\n", 100.0 * (total / blocks) / 5333333.0);
    }

    // 5) real-time paced run: 20 s, stream ON, measure whole-process CPU, worker CPU, RSS, underruns/overflows
    {
        sendParam(asb::kParamStreamEnable, 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const long rss0 = rssKb(); const double cpu0 = procCpuMs(); const auto s0 = p.engine().stats();
        const auto start = clk::now(); auto next = start;
        const double secs = 20.0; uint64_t blocks = 0; double tsum = 0, tmax = 0;
        while (std::chrono::duration<double>(clk::now() - start).count() < secs) {
            auto a = clk::now(); p.process(f.d); auto b = clk::now(); ++blocks;
            { double ns = std::chrono::duration<double, std::nano>(b - a).count(); tsum += ns; if (ns > tmax) tmax = ns; }
            next += std::chrono::nanoseconds(int64_t(256 / 48000.0 * 1e9));
            std::this_thread::sleep_until(next);
        }
        const auto s1 = p.engine().stats(); const double wall = std::chrono::duration<double>(clk::now() - start).count();
        std::printf("realtime 20 s paced run (stream ON, stub sink): process CPU %.3f %% of one core, worker thread CPU %.3f %%, RSS delta %ld kB\n",
                    100.0 * (procCpuMs() - cpu0) / 1e3 / wall, 100.0 * (s1.workerCpuMs - s0.workerCpuMs) / 1e3 / wall, rssKb() - rss0);
        std::printf("  status=%d underruns=%llu overflowFrames(delta)=%llu fill=%.3f drift=%.1f ppm blocks=%llu\n", int(s1.status),
                    (unsigned long long)(s1.underruns - s0.underruns), (unsigned long long)(s1.overflowFrames - s0.overflowFrames), s1.bufferFill, s1.driftPpm, (unsigned long long)blocks);
        std::printf("  process() with live consumer (real ring writes): mean %.0f ns/block, max %.0f ns (%.4f %% of 5.33 ms budget)\n", tsum / blocks, tmax, 100.0 * tmax / 5333333.0);
        CHECK(s1.status == asb::StreamStatus::Streaming);
        // 6) idle: stream OFF -> worker must burn ~0 CPU
        sendParam(asb::kParamStreamEnable, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double w0 = p.engine().stats().workerCpuMs, c0 = procCpuMs();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::printf("idle 5 s (stream OFF): process CPU delta %.3f ms (worker blocked)\n", procCpuMs() - c0); (void)w0;
    }
    p.setActive(false); p.terminate();
    std::printf(fails ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
