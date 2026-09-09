using System.Diagnostics;
using System.Linq;
using NvAPIWrapper;
using NvAPIWrapper.GPU;
using NvAPIWrapper.Native.GPU.Structures;
using NvAPIWrapper.Native.Interfaces.GPU;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

public sealed class GpuTelemetrySample
{
    public double? UsagePercent { get; set; }
    public double? GraphicsClockMhz { get; set; }
    public double? MemoryClockMhz { get; set; }
    public double? TemperatureCelsius { get; set; }
    public double? PowerPercentOfLimit { get; set; }
    public bool IsNvidia { get; set; }
}

/// <summary>
/// Reads GPU usage from Windows' built-in, vendor-agnostic "GPU Engine" performance counter
/// (works for NVIDIA/AMD/Intel alike), and layers real clock/temperature/power data from NVIDIA's
/// own NVAPI when an NVIDIA GPU is present. On AMD/Intel systems, everything beyond usage% is
/// honestly reported as unavailable — there is no equivalent public, driver-independent API for
/// those vendors this app can rely on without bundling a third-party sensor library.
/// </summary>
public sealed class GpuTelemetryCollector : IDisposable
{
    private readonly Dictionary<string, PerformanceCounter> _engineCounters = new();
    private readonly bool _gpuEngineCountersAvailable;
    private PhysicalGPU? _nvidiaGpu;
    private bool _nvApiInitialized;

    public GpuTelemetryCollector()
    {
        _gpuEngineCountersAvailable = PerformanceCounterCategory.Exists("GPU Engine");

        try
        {
            NVIDIA.Initialize();
            _nvApiInitialized = true;
            _nvidiaGpu = PhysicalGPU.GetPhysicalGPUs().FirstOrDefault();
        }
        catch
        {
            _nvidiaGpu = null;
        }
    }

    public GpuTelemetrySample Read()
    {
        var sample = new GpuTelemetrySample
        {
            UsagePercent = _gpuEngineCountersAvailable ? ReadUsageFromEngineCounters() : null
        };

        if (_nvidiaGpu is null) return sample;

        sample.IsNvidia = true;

        try
        {
            // NVAPI's own GPU-domain usage is generally more accurate for a dedicated NVIDIA card
            // than the generic engine counters (which sum every 3D-capable engine instance) —
            // prefer it when available.
            var gpuUsage = _nvidiaGpu.UsageInformation.GPU;
            if (gpuUsage is not null) sample.UsagePercent = gpuUsage.Percentage;
        }
        catch { /* keep the engine-counter based value */ }

        try
        {
            var clocks = _nvidiaGpu.CurrentClockFrequencies;
            sample.GraphicsClockMhz = ClockMhz(clocks.GraphicsClock);
            sample.MemoryClockMhz = ClockMhz(clocks.MemoryClock);
        }
        catch { /* leave null */ }

        try
        {
            var sensor = _nvidiaGpu.ThermalInformation.ThermalSensors.FirstOrDefault();
            if (sensor is not null) sample.TemperatureCelsius = sensor.CurrentTemperature;
        }
        catch { /* leave null */ }

        try
        {
            var power = _nvidiaGpu.PowerTopologyInformation.PowerTopologyEntries.FirstOrDefault();
            if (power is not null) sample.PowerPercentOfLimit = Math.Round(power.PowerUsageInPercent, 1);
        }
        catch { /* leave null */ }

        return sample;
    }

    private static double? ClockMhz(ClockDomainInfo info) => info.IsPresent ? info.Frequency / 1000.0 : null;

    private double? ReadUsageFromEngineCounters()
    {
        try
        {
            var currentInstances = new PerformanceCounterCategory("GPU Engine").GetInstanceNames()
                .Where(n => n.Contains("engtype_3D", StringComparison.OrdinalIgnoreCase))
                .ToHashSet();

            foreach (var stale in _engineCounters.Keys.Except(currentInstances).ToList())
            {
                _engineCounters[stale].Dispose();
                _engineCounters.Remove(stale);
            }

            foreach (var instance in currentInstances)
            {
                if (_engineCounters.ContainsKey(instance)) continue;
                try
                {
                    var counter = new PerformanceCounter("GPU Engine", "Utilization Percentage", instance, readOnly: true);
                    counter.NextValue();
                    _engineCounters[instance] = counter;
                }
                catch { /* instance disappeared between enumeration and creation */ }
            }

            if (_engineCounters.Count == 0) return null;

            var total = _engineCounters.Values.Sum(c =>
            {
                try { return c.NextValue(); }
                catch { return 0; }
            });

            return Math.Round(Math.Min(total, 100), 1);
        }
        catch
        {
            return null;
        }
    }

    public void Dispose()
    {
        foreach (var c in _engineCounters.Values) c.Dispose();
        _engineCounters.Clear();

        if (_nvApiInitialized)
        {
            try { NVIDIA.Unload(); } catch { /* best-effort */ }
        }
    }
}
