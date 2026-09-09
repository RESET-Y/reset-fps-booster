#include "logger.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace FrameBoostBeta::Logger {

namespace {
std::mutex g_mutex;
std::wstring g_logPath;

std::wstring Timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    std::wstringstream ss;
    ss << std::put_time(&tmBuf, L"%Y-%m-%d %H:%M:%S");
    return ss.str();
}
} // namespace

void Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logPath.empty()) return;

    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        std::wstring dir = std::wstring(localAppData) + L"\\ResetFpsBooster\\Logs";
        CoTaskMemFree(localAppData);
        CreateDirectoryW(dir.c_str(), nullptr);
        g_logPath = dir + L"\\framebooost_beta.log";
    }
}

void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logPath.empty()) return;

    std::wofstream file(g_logPath, std::ios::app);
    if (!file.is_open()) return;
    file << Timestamp() << L"  " << std::wstring(line.begin(), line.end()) << L"\n";
}

} // namespace FrameBoostBeta::Logger
