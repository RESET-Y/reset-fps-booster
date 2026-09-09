using System.Diagnostics;
using System.Linq;
using System.Management;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

/// <summary>
/// Reads real per-core CPU usage and an approximate current clock speed. CPU package power and
/// die temperature have no standard, driver-independent Windows API (they need vendor tools like
/// Intel Power Gadget or Ryzen Master, which this app does not bundle) — both are reported as
/// unavailable rather than guessed.
/// </summary>
public sealed class CpuTelemetryCollector : IDisposable
{
    private PerformanceCounter? _totalCounter;
    private readonly List<PerformanceCounter> _coreCounters = new();
    private readonly double _maxClockMhz;

    public CpuTelemetryCollector()
    {
        try
        {
            _totalCounter = new PerformanceCounter("Processor", "% Processor Time", "_Total");
            _totalCounter.NextValue();

            var instanceNames = new PerformanceCounterCategory("Processor").GetInstanceNames()
                .Where(n => n != "_Total")
                .OrderBy(n => int.TryParse(n, out var i) ? i : int.MaxValue);

            foreach (var instance in instanceNames)
            {
                try
                {
                    var counter = new PerformanceCounter("Processor", "% Processor Time", instance);
                    counter.NextValue();
                    _coreCounters.Add(counter);
                }
                catch
                {
                    // Skip a core whose counter instance failed to initialize.
                }
            }
        }
        catch
        {
            _totalCounter = null;
        }

        _maxClockMhz = ReadMaxClockMhz();
    }

    public double? MaxClockMhz => _maxClockMhz > 0 ? _maxClockMhz : null;

    public (double? Total, double[]? PerCore) ReadUsage()
    {
        double? total = null;
        try { total = _totalCounter is null ? null : Math.Round(_totalCounter.NextValue(), 1); }
        catch { /* leave null */ }

        double[]? perCore = null;
        if (_coreCounters.Count > 0)
        {
            var values = new List<double>();
            foreach (var counter in _coreCounters)
            {
                try { values.Add(Math.Round(counter.NextValue(), 1)); }
                catch { values.Add(0); }
            }
            perCore = values.ToArray();
        }

        return (total, perCore);
    }

    /// <summary>Estimates current average clock speed from "% Processor Performance" — the ratio
    /// of current to nominal frequency Windows itself uses for its own power reporting — combined
    /// with the CPU's rated max clock. A sustained ratio well under 100% while usage is high is a
    /// genuine (if soft) signal of frequency scaling, not necessarily throttling by itself.</summary>
    public double? ReadAverageClockMhz()
    {
        if (_maxClockMhz <= 0) return null;

        try
        {
            using var searcher = new ManagementObjectSearcher(
                "root\\CIMV2",
                "SELECT PercentProcessorPerformance FROM Win32_PerfFormattedData_Counters_ProcessorInformation WHERE Name='_Total'");

            foreach (ManagementObject obj in searcher.Get())
            {
                var percent = Convert.ToDouble(obj["PercentProcessorPerformance"] ?? 0);
                if (percent <= 0) continue;
                return Math.Round(_maxClockMhz * percent / 100.0, 0);
            }
        }
        catch
        {
            // WMI class unavailable on this system — leave unresolved.
        }

        return null;
    }

    private static double ReadMaxClockMhz()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT MaxClockSpeed FROM Win32_Processor");
            foreach (ManagementObject obj in searcher.Get())
            {
                return Convert.ToDouble(obj["MaxClockSpeed"] ?? 0);
            }
        }
        catch
        {
            // leave 0 -> unavailable
        }

        return 0;
    }

    public void Dispose()
    {
        _totalCounter?.Dispose();
        foreach (var c in _coreCounters) c.Dispose();
        _coreCounters.Clear();
    }
}
