// Non-Windows stand-in. Consumes the ring at wall-clock rate through the real
// DriftResampler and discards the audio. Exists so the plugin + tests build and
// run on Linux/macOS CI. It is NOT an audio output.
#include "engine/StreamEngine.h"
#include "core/DriftResampler.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <ctime>

namespace asb {

std::vector<DeviceInfo> enumerateRenderDevices() { return { {"", "System default (stub)"} }; }

struct StreamEngine::Impl {
    std::thread th;
    std::mutex m;                       // control<->worker wakeups only, never the audio thread
    std::condition_variable cv;
    bool quit = false;
};

StreamEngine::StreamEngine() : impl_(new Impl) {}
StreamEngine::~StreamEngine() { stop(); }

void StreamEngine::bump() noexcept
{
    configGen_.fetch_add(1, std::memory_order_release);
    if (impl_) { std::lock_guard<std::mutex> lk(impl_->m); impl_->cv.notify_all(); }
}

int StreamEngine::indexOfDeviceId(const std::string& id) const noexcept
{
    if (id.empty()) return 0;
    for (size_t i = 0; i < devices_.size(); ++i)
        if (devices_[i].id == id) return int(i);
    return -1;
}

std::string StreamEngine::resolveDeviceId(int idx)
{
    if (idx <= 0) return {};
    if (devices_.empty()) {                 // no snapshot given (e.g. asb_probe): enumerate here
        auto d = enumerateRenderDevices();
        return size_t(idx) < d.size() ? d[size_t(idx)].id : std::string("\x01missing");
    }
    return size_t(idx) < devices_.size() ? devices_[size_t(idx)].id : std::string("\x01missing");
}

bool StreamEngine::running() const noexcept { return impl_ && impl_->th.joinable(); }

void StreamEngine::start()
{
    if (impl_->th.joinable()) return;
    impl_->quit = false;
    impl_->th = std::thread([this] { workerMain(); });
}

void StreamEngine::stop()
{
    if (!impl_ || !impl_->th.joinable()) return;
    { std::lock_guard<std::mutex> lk(impl_->m); impl_->quit = true; impl_->cv.notify_all(); }
    impl_->th.join();
    status_.store(0);
}

StreamStats StreamEngine::stats() const noexcept
{
    StreamStats s;
    s.status = static_cast<StreamStatus>(status_.load());
    s.bufferFill = fill_.load();
    s.underruns = underruns_.load();
    s.overflowFrames = ring_.overflowFrames();
    s.deviceRate = devRate_.load();
    s.driftPpm = driftPpm_.load();
    s.workerCpuMs = cpuMs_.load();
    s.deviceFrames = devFrames_.load();
    s.resyncs = resyncs_.load();
    return s;
}

static double threadCpuMs()
{
    timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

void StreamEngine::workerMain()
{
    using clk = std::chrono::steady_clock;
    DriftResampler rs;
    std::vector<float> out(2 * 1024);
    const double devRate = 48000.0;
    bool active = false;
    uint32_t seenGen = ~0u;
    auto next = clk::now();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(impl_->m);
            if (impl_->quit) return;
            if (!enabled_.load()) {                       // idle: block, zero CPU
                active = false; status_.store(0);
                // poll: the audio thread never notifies (no syscalls on the RT thread)
                impl_->cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return impl_->quit || enabled_.load(); });
                continue;
            }
        }
        if (!active || seenGen != configGen_.load(std::memory_order_acquire)) {
            seenGen = configGen_.load(std::memory_order_acquire);
            const double host = hostRate_.load();
            const int block = hostBlock_.load();
            rs.configure(&ring_, host, devRate, targetFillFrames(host, block), resyncAboveFrames(host, block));
            ring_.discardAll();
            devRate_.store(devRate);
            status_.store(2);
            active = true;
            next = clk::now();
        }
        const size_t frames = 480;                         // 10 ms
        rs.render(out.data(), frames, gain_.load(std::memory_order_relaxed));
        devFrames_.fetch_add(frames, std::memory_order_relaxed);
        underruns_.store(rs.underruns());
        resyncs_.store(rs.resyncs());
        driftPpm_.store(rs.adjust() * 1e6);
        fill_.store(float(ring_.readableFrames()) / float(ring_.capacity()));
        cpuMs_.store(float(threadCpuMs()));
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
}

} // namespace asb
