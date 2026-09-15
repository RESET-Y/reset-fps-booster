#include "logger.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fbv2::Logger {

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

// KEEP THE FILE SMALL, BECAUSE SOMETHING ELSE READS ALL OF IT.
//
// The telemetry line is about 2.5 KB and is written every second, so this file
// grows roughly 9 MB an hour and never stopped. It reached 122 MB in a single
// evening of testing.
//
// That would be harmless if nothing read it. The app.s telemetry panel reads
// the WHOLE file every 500 ms, from the first byte, to find the last line
// mentioning "Native FPS:" - and it does so on the UI thread. At 122 MB that
// is a quarter of a gigabyte of text parsing per second, on the thread that
// also draws the window, beside a game that needs the machine.
//
// The real fix is on the other side - read the tail, not the file - and that
// is done too, but the C# cannot be rebuilt on this machine. Bounding the size
// here is worth doing regardless: an unbounded log is a defect on its own.
//
// A rename rather than a copy, and one generation kept, so the last hour or so
// survives for diagnosis.
constexpr std::uintmax_t kMaxLogBytes = 4 * 1024 * 1024;
void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logPath.empty()) return;

    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(g_logPath, ec);
        if (!ec && size > kMaxLogBytes) {
            const std::wstring previous = g_logPath + L".old";
            std::filesystem::remove(previous, ec);
            std::filesystem::rename(g_logPath, previous, ec);
        }
    }

    std::wofstream file(g_logPath, std::ios::app);
    if (!file.is_open()) return;
    file << Timestamp() << L"  " << std::wstring(line.begin(), line.end()) << L"\n";
}

} // namespace fbv2::Logger
