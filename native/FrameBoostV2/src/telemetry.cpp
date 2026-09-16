#include "telemetry.h"
#include "capture.h"   // NowMs
#include "logger.h"

#include <algorithm>
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

void Telemetry::NotePresentInterval(double deltaMs) {
    if (deltaMs <= 0.0 || deltaMs > 2000.0) return;
    // Bounded: a second at 288 fps is under 300 samples, and this stops a
    // stalled report from growing without limit.
    if (m_presentIntervals.size() < 4096) m_presentIntervals.push_back(deltaMs);
}

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
    if (m_sequenceBuffer.size() > kMaxSequenceBytes) { ++m_sequenceDropped; return; }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "[FrameBoostV2][seq] Frame " << r.frameId << ' ';
    if (r.generated) {
        oss << "Generated A=" << r.sourceA << " B=" << r.sourceB
            << " phase=" << std::setprecision(3) << r.phase << std::setprecision(2);
    } else {
        oss << "Native   ";
    }
    oss << " tA=" << r.sourceAMs << " tB=" << r.sourceBMs
        << " content=" << r.contentMs << " presented=" << r.presentedMs << '\n';
    m_sequenceBuffer += oss.str();
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

    // ---- the presentation side, which is what the eye actually gets ---------
    //
    // Everything above describes frames we made. These describe frames that
    // went out, and the difference between the two is the whole question.
    {
        auto& v = m_presentIntervals;
        double mn = 0, mx = 0, mean = 0, p50 = 0, p95 = 0, p99 = 0;
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            mn = v.front();
            mx = v.back();
            double sum = 0.0;
            for (double d : v) sum += d;
            mean = sum / v.size();
            auto at = [&v](double q) {
                size_t i = static_cast<size_t>(q * (v.size() - 1) + 0.5);
                return v[i];
            };
            p50 = at(0.50); p95 = at(0.95); p99 = at(0.99);
        }
        oss << " | Presented native/s: " << nativeFps
            << " | Presented generated/s: " << generatedFps
            << " | Generated produced/s: " << (m_generatedProduced / elapsed)
            << " | Present interval: min " << mn << ", p50 " << p50
            << ", mean " << mean << ", p95 " << p95 << ", p99 " << p99
            << ", max " << mx << " ms over " << v.size() << " presents";
    }

    // ---- the capture stage --------------------------------------------------
    //
    // Acquired counts every delivery. Unique is what survived both duplicate
    // tests and became a source frame - the number to compare against the
    // game's own counter, and the one that read 50 against Apex's 72 for a
    // week.
    // FOUR SEPARATE NUMBERS, because they answer four different questions.
    //
    //   acquired     what WGC handed over
    //   ts-equal     how often the compositor stamp repeated - OBSERVATION
    //                only; it discards nothing and never has any effect on
    //                the stream
    //   fp-dup       frames the content fingerprint found identical to the
    //                one before. The only thing that discards.
    //   unique       acquired minus fp-dup: what became a source frame
    //
    // Reading ts-equal against fp-dup is the point of the pair. If they track
    // each other, a repeated stamp really does mean a repeated picture. If
    // ts-equal is large and fp-dup is zero, the stamp was never evidence -
    // which is what the last run suggested, after it had already thrown away
    // 5 to 12 real frames a second on that assumption.
    {
        const double acq = m_capAcquired / elapsed;
        const double tsEqual = m_capDupTs / elapsed;
        const double fpDup = m_capDupContent / elapsed;
        oss << " | Acquire/s: " << acq
            << " | Timestamp-equal/s: " << tsEqual
            << " | Fingerprint-duplicate/s: " << fpDup
            << " | Unique/s: " << (acq - fpDup)
            << " | Fingerprint GPU: " << m_capFpMs << " ms";
    }

    // ---- the pairing stage --------------------------------------------------
    {
        oss << " | Valid pairs/s: " << (m_pairValid / elapsed)
            << " | Invalid pairs/s: " << ((m_pairDtZero + m_pairNoMotion) / elapsed)
            << " (dt<=0 " << (m_pairDtZero / elapsed)
            << ", no motion field " << (m_pairNoMotion / elapsed) << ")";
    }

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

    if (!m_sequenceBuffer.empty()) {
        // One write for the whole second, trailing newline trimmed so the
        // log does not gain a blank line every second.
        if (m_sequenceBuffer.back() == '\n') m_sequenceBuffer.pop_back();
        Logger::Log("[FrameBoostV2][seq] " + std::to_string(m_native + m_generated)
                    + " frames this second:\n" + m_sequenceBuffer);
        if (m_sequenceDropped) {
            Logger::Log("[FrameBoostV2][seq] " + std::to_string(m_sequenceDropped)
                        + " sequence lines dropped - buffer cap reached.");
            m_sequenceDropped = 0;
        }
        m_sequenceBuffer.clear();
    }

    m_windowStartMs = now;
    m_native = m_generated = m_source = 0;
    m_generatedProduced = 0;
    m_presentIntervals.clear();
    m_dropped = m_overflow = m_missedDeadline = 0;
    m_captureLatencySum = 0.0;
    m_ageSum = 0.0; m_ageMax = 0.0; m_ageCount = 0;
    m_pairIntervalSum = 0.0; m_pairIntervalCount = 0;
    m_pairIntervalMin = m_pairIntervalMax = 0.0;
    m_pairValid = m_pairDtZero = m_pairNoMotion = 0;
    m_pipelineLatencySum = 0.0; m_pipelineLatencyCount = 0;
    return true;
}

} // namespace fbv2
