#pragma once
#include <string>

// Separate log file from the (Watch-Dogs-specific) native/FrameBoost proxy
// DLLs - this is the system-level Beta engine, a different code path
// entirely, so its own log keeps the two efforts from being confused when
// reading %LOCALAPPDATA%\ResetFpsBooster\Logs.
namespace FrameBoostBeta::Logger {

void Init();
void Log(const std::string& line);

} // namespace FrameBoostBeta::Logger
