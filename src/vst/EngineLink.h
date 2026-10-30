// Processor -> Controller message carrying a pointer to the StreamEngine, so the editor can show live stats.
// Only valid in the same process: the controller checks the process id (asb::currentProcessId) before using it.
#pragma once
namespace asb {
constexpr const char* kEngineLinkMsg = "asb.engineLink";
constexpr const char* kEngineLinkPtr = "ptr";
constexpr const char* kEngineLinkPid = "pid";
} // namespace asb
