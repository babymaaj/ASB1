// macOS CoreAudio output: AUHAL output unit on the chosen device; its render callback (CoreAudio's real-time IO
// thread) pulls from the lock-free ring through the DriftResampler. A small watcher thread opens/closes the device,
// follows the system default output, and reopens after device loss or a stall.
#include "engine/StreamEngine.h"
#include "core/DriftResampler.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace asb {

namespace {

constexpr AudioObjectPropertyElement kMain = 0;   // kAudioObjectPropertyElementMain (named "Master" before macOS 12)

std::string cfToUtf8(CFStringRef s)
{
    if (!s) return {};
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(size_t(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string deviceString(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a{sel, kAudioObjectPropertyScopeGlobal, kMain};
    CFStringRef s = nullptr; UInt32 sz = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &s) != noErr || !s) return {};
    std::string r = cfToUtf8(s);
    CFRelease(s);
    return r;
}

UInt32 outputChannels(AudioObjectID dev)
{
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &sz) != noErr || sz == 0) return 0;
    std::vector<uint8_t> buf(sz);
    auto* abl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, abl) != noErr) return 0;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) ch += abl->mBuffers[i].mNumberChannels;
    return ch;
}

std::vector<AudioObjectID> allDevices()
{
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return {};
    std::vector<AudioObjectID> ids(sz / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data()) != noErr) return {};
    ids.resize(sz / sizeof(AudioObjectID));
    return ids;
}

AudioObjectID defaultOutputDevice()
{
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kMain};
    AudioObjectID dev = kAudioObjectUnknown; UInt32 sz = sizeof(dev);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, &dev);
    return dev;
}

AudioObjectID deviceByUid(const std::string& uid)
{
    for (AudioObjectID d : allDevices())
        if (deviceString(d, kAudioDevicePropertyDeviceUID) == uid) return d;
    return kAudioObjectUnknown;
}

bool deviceAlive(AudioObjectID dev)
{
    AudioObjectPropertyAddress a{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kMain};
    UInt32 alive = 0, sz = sizeof(alive);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &alive) != noErr) return false;
    return alive != 0;
}

double nominalRate(AudioObjectID dev)
{
    AudioObjectPropertyAddress a{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kMain};
    Float64 r = 0; UInt32 sz = sizeof(r);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &sz, &r) != noErr) return 0;
    return r;
}

double threadCpuMs()
{
    timespec ts{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

} // namespace

int64_t currentProcessId() { return int64_t(getpid()); }

std::vector<DeviceInfo> enumerateRenderDevices()
{
    std::vector<DeviceInfo> out;
    out.push_back({"", "System default"});
    std::vector<DeviceInfo> devs;
    for (AudioObjectID d : allDevices()) {
        if (outputChannels(d) == 0) continue;
        DeviceInfo di;
        di.id = deviceString(d, kAudioDevicePropertyDeviceUID);
        di.name = deviceString(d, kAudioObjectPropertyName);
        if (di.id.empty()) continue;
        if (di.name.empty()) di.name = di.id;
        devs.push_back(std::move(di));
    }
    std::sort(devs.begin(), devs.end(), [](const DeviceInfo& a, const DeviceInfo& b) { return a.id < b.id; });
    for (auto& d : devs) out.push_back(std::move(d));
    return out;
}

struct StreamEngine::Impl {
    std::thread th;
    std::mutex m;                      // control <-> watcher wakeups only; never touched by audio threads
    std::condition_variable cv;
    bool quit = false;

    AudioUnit au = nullptr;
    AudioObjectID dev = kAudioObjectUnknown;
    bool followDefault = false;
    DriftResampler rs;                 // used by the IO callback while running; configured only while stopped

    // CoreAudio IO thread: real-time, no locks/allocations.
    static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io)
    {
        auto* e = static_cast<StreamEngine*>(ref);
        Impl& im = *e->impl_;
        if (!io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
        float* out = static_cast<float*>(io->mBuffers[0].mData);
        const UInt32 n = std::min<UInt32>(frames, io->mBuffers[0].mDataByteSize / (2 * sizeof(float)));
        im.rs.render(out, n, e->gain_.load(std::memory_order_relaxed));
        e->devFrames_.fetch_add(n, std::memory_order_relaxed);
        e->underruns_.store(im.rs.underruns(), std::memory_order_relaxed);
        e->resyncs_.store(im.rs.resyncs(), std::memory_order_relaxed);
        e->driftPpm_.store(im.rs.adjust() * 1e6, std::memory_order_relaxed);
        e->fill_.store(float(e->ring_.readableFrames()) / float(e->ring_.capacity()), std::memory_order_relaxed);
        return noErr;
    }

    void close()
    {
        if (!au) return;
        AudioOutputUnitStop(au);
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        au = nullptr;
        dev = kAudioObjectUnknown;
    }

    bool open(StreamEngine& e)
    {
        const std::string uid = e.resolveDeviceId(e.deviceIndex_.load());
        if (!uid.empty() && uid[0] == '\x01') return false;          // index not in the list
        followDefault = uid.empty();
        dev = followDefault ? defaultOutputDevice() : deviceByUid(uid);
        if (dev == kAudioObjectUnknown) return false;

        AudioComponentDescription d{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent comp = AudioComponentFindNext(nullptr, &d);
        if (!comp || AudioComponentInstanceNew(comp, &au) != noErr) { au = nullptr; return false; }
        auto fail = [&] { close(); return false; };

        if (AudioUnitSetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr) return fail();
        const double devRate = nominalRate(dev);
        if (devRate < 8000) return fail();

        AudioStreamBasicDescription f{};
        f.mSampleRate = devRate;                 // device rate: no extra conversion; our resampler does host -> device
        f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = 8; f.mFramesPerPacket = 1; f.mBytesPerFrame = 8;
        f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
        if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) != noErr) return fail();

        AURenderCallbackStruct cb{&Impl::render, &e};
        if (AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr) return fail();
        if (AudioUnitInitialize(au) != noErr) return fail();

        const double host = e.hostRate_.load();
        const int block = e.hostBlock_.load(), ms = e.targetMs_.load();
        rs.configure(&e.ring_, host, devRate, targetFillFrames(host, block, ms), resyncAboveFrames(host, block, ms));
        e.ring_.discardAll();
        e.devRate_.store(devRate);
        if (AudioOutputUnitStart(au) != noErr) return fail();
        return true;
    }
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
    if (devices_.empty()) {
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
    { const double hr = hostRate_.load(); s.latencyMs = hr > 0 ? double(ring_.readableFrames()) * 1000.0 / hr : 0.0; }
    return s;
}

void StreamEngine::workerMain()
{
    using clk = std::chrono::steady_clock;
    Impl& im = *impl_;
    uint32_t seenGen = ~0u;
    auto retryAt = clk::time_point{};
    auto lastProgress = clk::now();
    uint64_t lastFrames = 0;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(im.m);
            if (im.quit) break;
        }
        const auto now = clk::now();
        const uint32_t gen = configGen_.load(std::memory_order_acquire);
        if (gen != seenGen) { seenGen = gen; im.close(); retryAt = {}; }

        if (!enabled_.load()) {
            im.close();
            status_.store(int(StreamStatus::Off));
        } else if (!im.au) {
            if (now >= retryAt) {
                status_.store(int(StreamStatus::Connecting));
                if (im.open(*this)) {
                    status_.store(int(StreamStatus::Streaming));
                    lastFrames = devFrames_.load();
                    lastProgress = now;
                } else {
                    im.close();
                    status_.store(int(StreamStatus::Error));
                    retryAt = now + std::chrono::seconds(1);
                }
            }
        } else {
            // health: device gone, default output changed (when following it), or no callbacks for 2 s -> reopen
            const uint64_t f = devFrames_.load();
            if (f != lastFrames) { lastFrames = f; lastProgress = now; }
            const bool gone = !deviceAlive(im.dev);
            const bool defaultMoved = im.followDefault && defaultOutputDevice() != im.dev;
            const bool stalled = now - lastProgress > std::chrono::seconds(2);
            if (gone || defaultMoved || stalled) {
                im.close();
                status_.store(int(defaultMoved && !gone && !stalled ? StreamStatus::Connecting : StreamStatus::Error));
                retryAt = defaultMoved && !gone && !stalled ? clk::time_point{} : now + std::chrono::seconds(1);
            }
        }
        cpuMs_.store(float(threadCpuMs()));

        std::unique_lock<std::mutex> lk(im.m);   // audio thread never notifies: poll (~20x/s)
        im.cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return im.quit; });
        if (im.quit) break;
    }
    im.close();
}

} // namespace asb
