// macOS update check: one asynchronous NSURLSession request to GitHub's "latest release" endpoint.
#import <Foundation/Foundation.h>
#include "vst/Update.h"
#include "vst/Ids.h"
#include "core/Version.h"
#include <mutex>

namespace asb {
namespace {
std::mutex g_m;
UpdateInfo g_info;
}

void updateCheckStart()
{
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_info.state == UpdateState::Checking) return;
        g_info.state = UpdateState::Checking;
    }
    NSString* url = [NSString stringWithFormat:@"https://api.github.com/repos/%s/releases/latest", kReleaseRepo];
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]
                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                   timeoutInterval:6.0];
    [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [req setValue:@"ASB-plugin" forHTTPHeaderField:@"User-Agent"];
    NSURLSession* session = [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration ephemeralSessionConfiguration]];
    NSURLSessionDataTask* task = [session dataTaskWithRequest:req
                                            completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err) {
        UpdateInfo info;
        const NSInteger status = [resp isKindOfClass:[NSHTTPURLResponse class]] ? [(NSHTTPURLResponse*)resp statusCode] : 0;
        if (!err && data && status == 200) {
            const std::string json(static_cast<const char*>(data.bytes), data.length);
            info.latestTag = jsonStringField(json, "tag_name");
            info.pageUrl = jsonStringField(json, "html_url");
        }
        if (info.pageUrl.empty()) info.pageUrl = kReleasesPage;
        if (info.latestTag.empty()) info.state = UpdateState::Failed;
        else info.state = isNewerVersion(info.latestTag, kVersion) ? UpdateState::Available : UpdateState::UpToDate;
        std::lock_guard<std::mutex> lk(g_m);
        g_info = info;
    }];
    [task resume];
    [session finishTasksAndInvalidate];
}

UpdateInfo updateCheckResult() { std::lock_guard<std::mutex> lk(g_m); return g_info; }
void updateCheckShutdown() {}

} // namespace asb
