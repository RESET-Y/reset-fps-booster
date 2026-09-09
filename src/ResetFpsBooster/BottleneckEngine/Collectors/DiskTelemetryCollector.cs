using System.Diagnostics;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

public sealed class DiskTelemetrySample
{
    public double? ActivePercent { get; set; }
    public double? ResponseTimeMs { get; set; }
}

/// <summary>Real disk activity via the standard PhysicalDisk performance counters.</summary>
public sealed class DiskTelemetryCollector : IDisposable
{
    private readonly PerformanceCounter? _activeTimeCounter;
    private readonly PerformanceCounter? _avgSecPerTransferCounter;

    public DiskTelemetryCollector()
    {
        try
        {
            _activeTimeCounter = new PerformanceCounter("PhysicalDisk", "% Disk Time", "_Total");
            _activeTimeCounter.NextValue();
            _avgSecPerTransferCounter = new PerformanceCounter("PhysicalDisk", "Avg. Disk sec/Transfer", "_Total");
            _avgSecPerTransferCounter.NextValue();
        }
        catch
        {
            _activeTimeCounter = null;
            _avgSecPerTransferCounter = null;
        }
    }

    public DiskTelemetrySample Read()
    {
        var sample = new DiskTelemetrySample();

        try { sample.ActivePercent = _activeTimeCounter is null ? null : Math.Round(Math.Min(_activeTimeCounter.NextValue(), 100), 1); }
        catch { /* leave null */ }

        try { sample.ResponseTimeMs = _avgSecPerTransferCounter is null ? null : Math.Round(_avgSecPerTransferCounter.NextValue() * 1000, 1); }
        catch { /* leave null */ }

        return sample;
    }

    public void Dispose()
    {
        _activeTimeCounter?.Dispose();
        _avgSecPerTransferCounter?.Dispose();
    }
}
