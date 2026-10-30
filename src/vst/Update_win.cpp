#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include "vst/Update.h"
#include "vst/Ids.h"
#include "core/Version.h"
#include <mutex>

namespace asb {
namespace {
std::mutex g_m;
UpdateInfo g_info;

std::string httpsGet(const wchar_t* host, const std::wstring& path)
{
    std::string body;
    HINTERNET s = WinHttpOpen(L"ASB-plugin", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ASB-plugin", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 6000);
    if (HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(r, hdr, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 512 * 1024) {
                        std::string chunk(avail, '\0'); DWORD got = 0;
                        if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
                        body.append(chunk.data(), got);
                    }
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return body;
}

void run()
{
    std::string repo = kReleaseRepo;
    const std::wstring path = L"/repos/" + std::wstring(repo.begin(), repo.end()) + L"/releases/latest";
    const std::string json = httpsGet(L"api.github.com", path);
    UpdateInfo info;
    info.latestTag = jsonStringField(json, "tag_name");
    info.pageUrl = jsonStringField(json, "html_url");
    if (info.pageUrl.empty()) info.pageUrl = kReleasesPage;
    if (info.latestTag.empty()) info.state = UpdateState::Failed;
    else info.state = isNewerVersion(info.latestTag, kVersion) ? UpdateState::Available : UpdateState::UpToDate;
    std::lock_guard<std::mutex> lk(g_m);
    g_info = info;
}
} // namespace

// The worker holds a reference on this DLL and releases it with FreeLibraryAndExitThread, so the plugin can't be
// unloaded under a running check and nothing has to be joined at unload (joining in DllMain/ExitDll can deadlock,
// and a joinable std::thread destroyed at unload would call std::terminate).
static DWORD WINAPI threadProc(LPVOID mod)
{
    run();
    FreeLibraryAndExitThread(static_cast<HMODULE>(mod), 0);
}

void updateCheckStart()
{
    std::lock_guard<std::mutex> lk(g_m);
    if (g_info.state == UpdateState::Checking) return;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&threadProc), &self)) return;
    g_info.state = UpdateState::Checking;
    if (HANDLE t = CreateThread(nullptr, 0, threadProc, self, 0, nullptr)) CloseHandle(t);
    else { g_info.state = UpdateState::Failed; FreeLibrary(self); }
}

UpdateInfo updateCheckResult() { std::lock_guard<std::mutex> lk(g_m); return g_info; }

void updateCheckShutdown() {}   // nothing to join (see threadProc)

} // namespace asb
