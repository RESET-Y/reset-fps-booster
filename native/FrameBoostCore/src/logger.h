#pragma once
#include <string>

// Writes to %LOCALAPPDATA%\ResetFpsBooster\Logs\frameboost_native.log - the
// same Logs folder the C# app already uses, so this is visible from the
// existing RESET Logs page (Phase 11) without inventing a new location.
namespace FrameBoost::Logger {

void Init();
void Log(const std::string& line);

} // namespace FrameBoost::Logger
