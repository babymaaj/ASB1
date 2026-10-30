// asb_probe: Windows WASAPI smoke test, no DAW needed.
//   asb_probe list            -> prints output devices with the indices the plugin uses
//   asb_probe <index> [sec]   -> plays a quiet 440 Hz tone through StreamEngine for sec (default 8) and prints stats
#include "engine/StreamEngine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
using namespace asb;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    auto devs = enumerateRenderDevices();
    if (argc < 2 || !std::strcmp(argv[1], "list")) {
        for (size_t i = 0; i < devs.size(); ++i) std::printf("[%zu] %s\n     id=%s\n", i, devs[i].name.c_str(), devs[i].id.c_str());
        std::printf("usage: asb_probe <index> [seconds]\n");
        return 0;
    }
    const int idx = std::atoi(argv[1]);
    const double secs = argc > 2 ? std::atof(argv[2]) : 8.0;
    const double sr = 48000.0;
    std::printf("device [%d] %s, host rate %.0f, %.0f s\n", idx, idx >= 0 && size_t(idx) < devs.size() ? devs[idx].name.c_str() : "?", sr, secs);

    StreamEngine e;
    e.setHostSampleRate(sr); e.setGain(0.25f); e.setDeviceIndex(idx); e.setEnabled(true); e.start();

    const char* names[] = {"Off", "Connecting", "Streaming", "Error"};
    const auto t0 = clk::now(); uint64_t sent = 0; double ph = 0; std::vector<float> b(2048);
    auto lastPrint = t0;
    while (std::chrono::duration<double>(clk::now() - t0).count() < secs) {
        const uint64_t due = uint64_t(std::chrono::duration<double>(clk::now() - t0).count() * sr);
        while (sent + 256 <= due) { // push whole 256-frame blocks to track wall clock like a DAW would
            for (int i = 0; i < 256; ++i) { b[i] = float(std::sin(ph)); ph += 2 * M_PI * 440.0 / sr; }
            e.pushPlanar(b.data(), b.data(), 256); sent += 256;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (clk::now() - lastPrint > std::chrono::seconds(1)) {
            lastPrint = clk::now(); auto s = e.stats();
            std::printf("  status=%-10s devRate=%.0f fill=%.3f underruns=%llu overflowFrames=%llu drift=%.1fppm workerCpu=%.0fms\n",
                        names[int(s.status)], s.deviceRate, s.bufferFill, (unsigned long long)s.underruns, (unsigned long long)s.overflowFrames, s.driftPpm, s.workerCpuMs);
        }
    }
    e.stop();
    std::printf("done. Did you hear a quiet steady 440 Hz tone with no crackles? Report status/underruns above.\n");
    return 0;
}
