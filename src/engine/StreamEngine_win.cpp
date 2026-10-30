// Windows WASAPI shared-mode event-driven renderer.
// NOT COMPILED OR RUN by the authoring session (Linux sandbox) -- see TEST_RESULTS.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "engine/StreamEngine.h"
#include "core/DriftResampler.h"

#include <windows.h>
#include <initguid.h>   // emits PKEY_Device_FriendlyName in this TU
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>   // DEFINE_PROPERTYKEY: MSVC's devpkey header needs this first (MinGW's includes it itself)
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <chrono>

// Libraries (ole32, avrt, uuid) are linked from CMakeLists.txt so MSVC and MinGW both work.

namespace asb {

namespace {

template <class T> struct ComPtr {
    T* p = nullptr;
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string toUtf8(const wchar_t* w)
{
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring toWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally so we don't depend on initguid.h/ksmedia.h include order.
constexpr GUID kSubtypeIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The audio thread never signals the worker (no syscalls on the RT thread), so an idle
// worker polls the config generation this often. 20 wakeups/s is negligible CPU.
constexpr DWORD kPollMs = 50;

double threadCpuMs()
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return double(a.QuadPart + b.QuadPart) / 1e4; // 100 ns units -> ms
}

} // namespace

std::vector<DeviceInfo> enumerateRenderDevices()
{
    std::vector<DeviceInfo> out;
    out.push_back({"", "System default"});
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {   // every COM object must be released BEFORE CoUninitialize (was a crash on real Windows)
        ComPtr<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void**)en.put()))) {
            ComPtr<IMMDeviceCollection> col;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put()))) {
                UINT n = 0; col->GetCount(&n);
                std::vector<DeviceInfo> devs;
                for (UINT i = 0; i < n; ++i) {
                    ComPtr<IMMDevice> dev;
                    if (FAILED(col->Item(i, dev.put()))) continue;
                    LPWSTR id = nullptr;
                    if (FAILED(dev->GetId(&id))) continue;
                    DeviceInfo di; di.id = toUtf8(id); CoTaskMemFree(id);
                    ComPtr<IPropertyStore> ps;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, ps.put()))) {
                        PROPVARIANT v; PropVariantInit(&v);
                        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                            di.name = toUtf8(v.pwszVal);
                        PropVariantClear(&v);
                    }
                    if (di.name.empty()) di.name = di.id;
                    devs.push_back(std::move(di));
                }
                std::sort(devs.begin(), devs.end(), [](auto& a, auto& b) { return a.id < b.id; });
                for (auto& d : devs) out.push_back(std::move(d));
            }
        }
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return out;
}

struct StreamEngine::Impl {
    HANDLE thread = nullptr;
    HANDLE wakeEvt = nullptr;       // auto-reset: control-thread config change / quit
    std::atomic<bool> quit{false};
};

StreamEngine::StreamEngine() : impl_(new Impl)
{
    impl_->wakeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

StreamEngine::~StreamEngine()
{
    stop();
    if (impl_->wakeEvt) CloseHandle(impl_->wakeEvt);
}

void StreamEngine::bump() noexcept
{
    configGen_.fetch_add(1, std::memory_order_release);
    if (impl_ && impl_->wakeEvt) SetEvent(impl_->wakeEvt);
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

bool StreamEngine::running() const noexcept { return impl_ && impl_->thread; }

void StreamEngine::start()
{
    if (impl_->thread) return;
    impl_->quit = false;
    impl_->thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        static_cast<StreamEngine*>(p)->workerMain();
        return 0;
    }, this, 0, nullptr);
}

void StreamEngine::stop()
{
    if (!impl_ || !impl_->thread) return;
    impl_->quit = true;
    SetEvent(impl_->wakeEvt);
    WaitForSingleObject(impl_->thread, 5000);
    CloseHandle(impl_->thread);
    impl_->thread = nullptr;
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

void StreamEngine::workerMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);

    HANDLE renderEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DriftResampler rs;

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufFrames = 0;
    bool open = false;
    uint32_t seenGen = ~0u;
    ULONGLONG retryAt = 0;

    auto closeDevice = [&] {
        if (open && client) client->Stop();
        render.reset(); client.reset(); open = false;
    };

    auto openDevice = [&]() -> bool {
        if (!enumerator &&
            FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)enumerator.put())))
            return false;
        ComPtr<IMMDevice> dev;
        const std::string id = resolveDeviceId(deviceIndex_.load());
        HRESULT hr;
        if (id.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, dev.put());
        } else {
            if (id[0] == '\x01') return false;   // index not in the list
            hr = enumerator->GetDevice(toWide(id).c_str(), dev.put());
        }
        if (FAILED(hr)) return false;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.put()))) return false;

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix)) || !mix) return false;
        const DWORD devRate = mix->nSamplesPerSec;
        CoTaskMemFree(mix);

        // Ask for float32 stereo at the engine rate; Windows converts channels/format (AUTOCONVERTSRCFORMAT).
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = devRate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = devRate * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = kSubtypeIeeeFloat;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                0, 0, &wf.Format, nullptr);
        if (FAILED(hr)) return false;
        if (FAILED(client->SetEventHandle(renderEvt))) return false;
        if (FAILED(client->GetBufferSize(&bufFrames))) return false;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)render.put()))) return false;

        const double host = hostRate_.load();
        const int block = hostBlock_.load();
        rs.configure(&ring_, host, double(devRate), targetFillFrames(host, block), resyncAboveFrames(host, block));
        ring_.discardAll();
        devRate_.store(double(devRate));

        // pre-fill with silence so the first period is valid, then start
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufFrames, &data)))
            render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(client->Start())) return false;
        open = true;
        return true;
    };

    for (;;) {
        if (impl_->quit.load()) break;

        const bool want = enabled_.load();
        const uint32_t gen = configGen_.load(std::memory_order_acquire);
        if (gen != seenGen) {                       // config changed: reopen (or close if disabled)
            seenGen = gen;
            closeDevice();
            retryAt = 0;
        }
        if (!want) {                                // idle: ~20 wakeups/s to notice audio-thread requests
            status_.store(int(StreamStatus::Off));
            WaitForSingleObject(impl_->wakeEvt, kPollMs);
            continue;
        }
        if (!open) {
            const ULONGLONG now = GetTickCount64();
            if (now < retryAt) { WaitForSingleObject(impl_->wakeEvt, DWORD(std::min<ULONGLONG>(retryAt - now, kPollMs))); continue; }
            status_.store(int(StreamStatus::Connecting));
            if (!openDevice()) {
                closeDevice();
                status_.store(int(StreamStatus::Error));
                retryAt = GetTickCount64() + 1000;  // 1 s reconnect backoff
                continue;
            }
            status_.store(int(StreamStatus::Streaming));
        }

        HANDLE waits[2] = { renderEvt, impl_->wakeEvt };
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0 + 1) continue;        // config/quit: loop re-evaluates
        if (w == WAIT_TIMEOUT) {                     // device stalled / removed silently
            closeDevice(); status_.store(int(StreamStatus::Error)); retryAt = GetTickCount64() + 1000; continue;
        }

        UINT32 pad = 0;
        HRESULT hr = client->GetCurrentPadding(&pad);
        if (FAILED(hr)) { closeDevice(); status_.store(int(StreamStatus::Error)); retryAt = GetTickCount64() + 1000; continue; }
        const UINT32 frames = bufFrames - pad;
        if (frames == 0) continue;
        BYTE* data = nullptr;
        hr = render->GetBuffer(frames, &data);
        if (FAILED(hr)) { closeDevice(); status_.store(int(StreamStatus::Error)); retryAt = GetTickCount64() + 1000; continue; }

        float* dst = reinterpret_cast<float*>(data);
        UINT32 done = 0;
        while (done < frames) {                      // render straight into the device buffer (no temp copy)
            const UINT32 n = std::min<UINT32>(frames - done, 4096);
            rs.render(dst + size_t(done) * 2, n, gain_.load(std::memory_order_relaxed));
            done += n;
        }
        render->ReleaseBuffer(frames, 0);
        devFrames_.fetch_add(frames, std::memory_order_relaxed);

        underruns_.store(rs.underruns());
        resyncs_.store(rs.resyncs());
        driftPpm_.store(rs.adjust() * 1e6);
        fill_.store(float(ring_.readableFrames()) / float(ring_.capacity()));
        cpuMs_.store(float(threadCpuMs()));
    }

    closeDevice();
    enumerator.reset();
    CloseHandle(renderEvt);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

} // namespace asb
