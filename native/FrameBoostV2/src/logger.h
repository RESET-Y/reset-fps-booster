#pragma once
#include <string>

// The log file IS the contract with the app: Services/FrameBoostBetaService.cs
// tails framebooost_beta.log and parses the last line containing "Native FPS:".
// Carried over from the V1 engine unchanged, including the 4 MB cap - an
// uncapped one reached 122 MB and the app read it from byte 0 twice a second.
namespace fbv2::Logger {

void Init();
void Log(const std::string& line);

} // namespace fbv2::Logger
