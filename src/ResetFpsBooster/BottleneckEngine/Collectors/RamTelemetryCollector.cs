using System.Diagnostics;
using ResetFpsBooster.Hardware;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

public sealed class RamTelemetrySample
{
    public long? UsedBytes { get; set; }
    public long? TotalBytes { get; set; }
    public double? HardFaultsPerSec { get; set; }
}

/// <summary>
/// Real total/used RAM via the same Win32 call the RAM Cleaner module uses, plus hard page
/// faults/sec (pages actually round-tripped to disk to resolve a fault — the real symptom of
/// memory pressure, not just high usage) from the standard "Memory\Pages/sec" counter.
/// </summary>
public sealed class RamTelemetryCollector : IDisposable
{
    private readonly PerformanceCounter? _pagesPerSecCounter;

    public RamTelemetryCollector()
    {
        try
        {
            _pagesPerSecCounter = new PerformanceCounter("Memory", "Pages/sec");
            _pagesPerSecCounter.NextValue();
        }
        catch
        {
            _pagesPerSecCounter = null;
        }
    }

    public RamTelemetrySample Read()
    {
        var sample = new RamTelemetrySample();

        try
        {
            NativeMemoryStatus.GlobalMemoryStatusEx(out var status);
            sample.TotalBytes = (long)status.ullTotalPhys;
            sample.UsedBytes = (long)(status.ullTotalPhys - status.ullAvailPhys);
        }
        catch { /* leave null */ }

        try { sample.HardFaultsPerSec = _pagesPerSecCounter is null ? null : Math.Round(_pagesPerSecCounter.NextValue(), 1); }
        catch { /* leave null */ }

        return sample;
    }

    public void Dispose() => _pagesPerSecCounter?.Dispose();
}
