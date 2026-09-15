#include "telemetry.h"
#include "capture.h"   // NowMs
#include "logger.h"

#include <iomanip>
#include <sstream>

namespace fbv2 {

void Telemetry::Init(double displayHz) {
    m_displayHz = displayHz;
    m_windowStartMs = NowMs();
}

void Telemetry::NoteNative(double captureLatencyMs, double onScreenAgeMs) {
    ++m_native;
    m_captureLatencySum += captureLatencyMs;
    m_ageSum += onScreenAgeMs;
    if (onScreenAgeMs > m_ageMax) m_ageMax = onScreenAgeMs;
    ++m_ageCount;
}

void Telemetry::NoteGenerated(double onScreenAgeMs) {
    ++m_generated;
    m_ageSum += onScreenAgeMs;
    if (onScreenAgeMs > m_ageMax) m_ageMax = onScreenAgeMs;
    ++m_ageCount;
}

void Telemetry::NoteSourceArrival() { ++m_source; }

void Telemetry::NoteGpu(double motionMs, double interpMs) {
    if (motionMs >= 0.0) m_motionGpuMs = motionMs;
    if (interpMs >= 0.0) m_interpGpuMs = interpMs;
}

void Telemetry::NotePairIntervalMs(double ms) {
    if (ms <= 0.0 || ms > 1000.0) return;
    m_pairIntervalSum += ms;
    if (m_pairIntervalCount == 0 || ms < m_pairIntervalMin) m_pairIntervalMin = ms;
    if (ms > m_pairIntervalMax) m_pairIntervalMax = ms;
    ++m_pairIntervalCount;
}

void Telemetry::NotePresentWaitMs(double sumMs, double callSumMs,
                                  double callMaxMs, uint64_t presents) {
    m_presentWaitSum = sumMs;
    m_presentCallSum = callSumMs;
    m_presentCallMax = callMaxMs;
    m_presents = presents;
}

void Telemetry::NoteQueue(int depth, int overflowCount) {
    m_queueDepth = depth;
    m_queueOverflow = overflowCount;
}

void Telemetry::NotePipelineLatencyMs(double ms) {
    if (ms < 0.0 || ms > 5000.0) return;
    m_pipelineLatencySum += ms;
    ++m_pipelineLatencyCount;
}

void Telemetry::NoteSequence(const FrameRecord& r) {
    if (!m_sequenceLog) return;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "[FrameBoostV2][seq] Frame " << r.frameId << ' ';
    if (r.generated) {
        oss << "Generated A=" << r.sourceA << " B=" << r.sourceB
            << " phase=" << std::setprecision(3) << r.phase << std::setprecision(2);
    } else {
        oss << "Native   ";
    }
    oss << " content=" << r.contentMs << " presented=" << r.presentedMs;
    Logger::Log(oss.str());
}

bool Telemetry::ReportIfDue() {
    const double now = NowMs();
    const double elapsed = (now - m_windowStartMs) / 1000.0;
    if (elapsed < 1.0) return false;

    const double nativeFps    = m_native / elapsed;
    const double generatedFps = m_generated / elapsed;
    const double sourceFps    = m_source / elapsed;
    // Output is the sum of what was actually shown once each. Nothing else
    // goes in here - see the header.
    const double outputFps    = nativeFps + generatedFps;

    const double captureLatency = m_native ? m_captureLatencySum / m_native : 0.0;
    const double ageAvg = m_ageCount ? m_ageSum / m_ageCount : 0.0;
    const double pairAvg = m_pairIntervalCount ? m_pairIntervalSum / m_pairIntervalCount : 0.0;
    const double pipeline = m_pipelineLatencyCount
                          ? m_pipelineLatencySum / m_pipelineLatencyCount : 0.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);

    // ---- the contract, field names fixed ------------------------------------
    oss << "[FrameBoostV2] Source FPS: " << sourceFps
        << " | Native FPS: " << nativeFps
        << " | Generated FPS: " << generatedFps
        << " | Output FPS: " << outputFps
        << " | Poll time: " << 0.0
        << " ms | Capture latency (real, avg): " << captureLatency
        << " ms | On-screen age: " << ageAvg << " ms avg, " << m_ageMax << " ms max"
        << " | Display Hz: " << m_displayHz
        << " | Duplicate frames skipped/s: " << 0.0
        << " | Doubling: " << (m_generated ? "on" : "off")
        << " | Motion estimation GPU: " << m_motionGpuMs
        << " ms | Interpolation GPU: " << m_interpGpuMs << " ms";

    // ---- V2's own, free to change -------------------------------------------
    oss << " | Pair interval: " << pairAvg << " ms mean, min " << m_pairIntervalMin
        << ", max " << m_pairIntervalMax
        << " | Queue depth: " << m_queueDepth
        << " | Ring overflows/s: " << (m_overflow / elapsed)
        << " | Dropped/s: " << (m_dropped / elapsed)
        << " | Missed deadlines/s: " << (m_missedDeadline / elapsed)
        << " | Present wait: " << (m_presents ? m_presentWaitSum / m_presents : 0.0)
        << " ms avg | Present call: " << (m_presents ? m_presentCallSum / m_presents : 0.0)
        << " ms avg, " << m_presentCallMax << " ms max"
        << " | Pipeline latency: " << pipeline << " ms";

    Logger::Log(oss.str());

    m_windowStartMs = now;
    m_native = m_generated = m_source = 0;
    m_dropped = m_overflow = m_missedDeadline = 0;
    m_captureLatencySum = 0.0;
    m_ageSum = 0.0; m_ageMax = 0.0; m_ageCount = 0;
    m_pairIntervalSum = 0.0; m_pairIntervalCount = 0;
    m_pairIntervalMin = m_pairIntervalMax = 0.0;
    m_pipelineLatencySum = 0.0; m_pipelineLatencyCount = 0;
    return true;
}

} // namespace fbv2
