// THE LOG LINE IS A CONTRACT, not a debug aid.
//
// The C# side (Services/FrameBoostBetaService.cs) tails
// %LOCALAPPDATA%\ResetFpsBooster\Logs\framebooost_beta.log, takes the last
// line containing "Native FPS:", and reads eleven named fields out of it plus
// two literal substrings. Rename one and the app's readout goes blank.
//
// The exact set it parses, in "Name: value" form:
//
//   Source FPS, Native FPS, Generated FPS, Output FPS, Poll time,
//   Capture latency (real, avg), On-screen age, Display Hz,
//   Duplicate frames skipped/s, Motion estimation GPU, Interpolation GPU
//
// plus "Doubling: on" and "no GPU room" as plain text.
//
// Everything after those is V2's own and free to change.
//
// WHAT THE NUMBERS MAY NOT DO. Native counts real frames presented once each.
// Generated counts interpolated frames that were actually handed to the
// presenter. Output is their sum and nothing else - no repeats, no keep-alive,
// no duplicate passthrough. A picture shown twice is one frame. This is the
// rule V1 broke three separate times, each time making the counter look better
// and the screen no different.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbv2 {

struct FrameRecord {
    uint64_t frameId = 0;
    bool     generated = false;
    uint64_t sourceA = 0;     // generated only
    uint64_t sourceB = 0;     // generated only
    double   contentMs = 0.0; // the moment this picture represents
    double   phase = 0.0;     // generated only
    double   presentedMs = 0.0;
    double   sourceAMs = 0.0;   // tA
    double   sourceBMs = 0.0;   // tB
};

class Telemetry {
public:
    void Init(double displayHz);

    // Per-second accumulators.
    void NoteNative(double captureLatencyMs, double onScreenAgeMs);
    void NoteGenerated(double onScreenAgeMs);

    // PRODUCED IS NOT PRESENTED, and conflating them is how a counter can read
    // 52 while the screen shows 30. NoteGenerated above counts frames that
    // reached the presenter; this counts frames the interpolator built. When
    // the two disagree, the loss is between generation and presentation and
    // nowhere else.
    void NoteGeneratedProduced() { ++m_generatedProduced; }

    // The gap between one present and the one before it, measured at the
    // present itself. Output FPS is an average and an average hides exactly
    // the thing that ruins the feel: at 100 fps a steady 10 ms and an
    // alternating 2/18 ms both average to 10. The percentiles separate them.
    void NotePresentInterval(double deltaMs);
    void NoteSourceArrival();
    void NoteDropped()  { ++m_dropped; }
    void NoteOverflow() { ++m_overflow; }
    void NoteMissedDeadline() { ++m_missedDeadline; }

    // WHY A PAIR DID OR DID NOT PRODUCE A FRAME, counted apart.
    //
    // dt <= 0 is its own category because it is not a fault and not a dropped
    // frame: two captured frames can carry the same compositor stamp, measured
    // at up to 14 a second. There is no interval to place a midpoint in, so no
    // frame is generated and none is invented. The count says how often the
    // source costs us a generated frame that way.
    void NoteValidPair()   { ++m_pairValid; }
    void NotePairDtZero()  { ++m_pairDtZero; }
    void NotePairNoMotion(){ ++m_pairNoMotion; }
    void NoteGpu(double motionMs, double interpMs);

    // TWO CLOCKS, MEASURED SIDE BY SIDE, so the choice between them stays a
    // measurement. NotePairIntervalMs is the QPC arrival delta - the one the
    // cadence is now built on. NoteSrtIntervalMs is the SystemRelativeTime
    // delta for the same pair, which decides nothing and is only reported.
    //
    // m_contentDtZero counts the pairs whose compositor stamps were equal or
    // went backwards. Under the old cadence every one of those cost a
    // generated frame; now it costs nothing, and the count is what proves it.
    void NotePairIntervalMs(double ms);
    void NoteSrtIntervalMs(double ms);
    void NoteContentDtZero() { ++m_contentDtZero; }
    void NotePresentWaitMs(double sumMs, double callSumMs, double callMaxMs, uint64_t presents);
    void NoteQueue(int depth, int overflowCount);

    // THE CAPTURE STAGE, SEPARATED. Acquired is what WGC handed over; the two
    // duplicate counts say why some of it is not a source frame. Unique falls
    // out as acquired minus both, and unique is the only number that answers
    // "does the engine see what the game drew".
    void NoteCapture(uint64_t acquired, uint64_t dupTimestamp, uint64_t dupContent,
                     double fingerprintMsAvg) {
        m_capAcquired = acquired;
        m_capDupTs = dupTimestamp;
        m_capDupContent = dupContent;
        m_capFpMs = fingerprintMsAvg;
    }
    void NotePipelineLatencyMs(double ms);

    // THE FRAME SEQUENCE, one line per presented frame.
    //
    // Asked for directly: a counter cannot tell N G N G apart from N N G G, and
    // the second is what "feels like 15 fps" looks like from inside. Off by
    // default because it writes a line per frame; "sequence" on the command
    // line turns it on.
    void EnableSequenceLog(bool on) { m_sequenceLog = on; }
    void NoteSequence(const FrameRecord& r);

    // Emits the contract line if a second has passed. Returns true if it did.
    bool ReportIfDue();

private:
    double   m_displayHz = 0.0;
    double   m_windowStartMs = 0.0;
    bool     m_sequenceLog = false;

    uint64_t m_native = 0, m_generated = 0, m_source = 0;
    uint64_t m_generatedProduced = 0;
    std::vector<double> m_presentIntervals;
    uint64_t m_dropped = 0, m_overflow = 0, m_missedDeadline = 0;
    double   m_captureLatencySum = 0.0;
    double   m_ageSum = 0.0; double m_ageMax = 0.0; uint64_t m_ageCount = 0;
    double   m_motionGpuMs = 0.0, m_interpGpuMs = 0.0;
    double   m_pairIntervalSum = 0.0; uint64_t m_pairIntervalCount = 0;
    double   m_pairIntervalMin = 0.0, m_pairIntervalMax = 0.0;
    double   m_presentWaitSum = 0.0, m_presentCallSum = 0.0, m_presentCallMax = 0.0;
    uint64_t m_presents = 0;
    int      m_queueDepth = 0, m_queueOverflow = 0;
    uint64_t m_pairValid = 0, m_pairDtZero = 0, m_pairNoMotion = 0;
    double   m_srtIntervalSum = 0.0; uint64_t m_srtIntervalCount = 0;
    double   m_srtIntervalMin = 0.0, m_srtIntervalMax = 0.0;
    uint64_t m_contentDtZero = 0;
    uint64_t m_capAcquired = 0, m_capDupTs = 0, m_capDupContent = 0;
    double   m_capFpMs = 0.0;
    double   m_pipelineLatencySum = 0.0; uint64_t m_pipelineLatencyCount = 0;

    // BUFFERED, because writing it per frame would distort what it measures.
    //
    // Logger::Log stats the file for its size cap and then opens, writes and
    // closes it. That is fine once a second. At 288 presented frames a second
    // it is 288 file operations a second inside the engine loop - and the
    // thing this log exists to diagnose is stutter, so a diagnostic that
    // causes stutter proves nothing.
    //
    // Lines accumulate here and go out with the telemetry line. Bounded, so a
    // long run cannot turn the buffer into the leak the log file used to be.
    std::string m_sequenceBuffer;
    uint64_t    m_sequenceDropped = 0;
    static constexpr size_t kMaxSequenceBytes = 256 * 1024;
};

} // namespace fbv2
