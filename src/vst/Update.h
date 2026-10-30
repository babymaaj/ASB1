// Update check against GitHub Releases (public repo). Background thread, never on the audio thread.
// Only reads https://api.github.com/repos/<repo>/releases/latest; sends nothing about the user.
#pragma once
#include <string>

namespace asb {

constexpr const char* kReleaseRepo = "babymaaj/ASB1";
constexpr const char* kReleasesPage = "https://github.com/babymaaj/ASB1/releases/latest";

enum class UpdateState { Idle, Checking, UpToDate, Available, Failed };
struct UpdateInfo { UpdateState state = UpdateState::Idle; std::string latestTag, pageUrl; };

void updateCheckStart();          // starts a check if one isn't running (UI thread)
UpdateInfo updateCheckResult();   // thread-safe snapshot
void updateCheckShutdown();       // joins the worker (module unload)

} // namespace asb
