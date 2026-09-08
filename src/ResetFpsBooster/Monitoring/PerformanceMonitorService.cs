using System.Diagnostics;
using System.IO;
using System.Linq;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Hardware;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.Monitoring;

/// <summary>
/// Wraps real Windows performance counters. Instantiated only while the Performance page is
/// open (see the corresponding ViewModel) so it never costs CPU time in the background.
/// GPU usage/memory come from the built-in "GPU Engine" / "GPU Process Memory" counter
/// categories (Windows 10 2004+). GPU temperature is only available for NVIDIA cards via
/// nvidia-smi, which ships with the driver — if it isn't present, the sample honestly reports
/// no temperature rather than fabricating one. In-game FPS is intentionally not measured here:
/// reliably reading FPS requires hooking the game's render loop, which this app does not do.
/// </summary>
public sealed class PerformanceMonitorService : IPerformanceMonitorService
{
    private readonly PerformanceCounter? _cpuCounter;
    private readonly Dictionary<string, PerformanceCounter> _gpuEngineCounters = new();
    private readonly string? _nvidiaSmiPath;

    public bool IsGpuMonitoringAvailable { get; }
    public bool IsGpuTemperatureAvailable => _nvidiaSmiPath is not null;

    public PerformanceMonitorService()
    {
        try
        {
            _cpuCounter = new PerformanceCounter("Processor", "% Processor Time", "_Total");
            _cpuCounter.NextValue();
        }
        catch
        {
            _cpuCounter = null;
        }

        IsGpuMonitoringAvailable = PerformanceCounterCategory.Exists("GPU Engine");
        _nvidiaSmiPath = LocateNvidiaSmi();
    }

    public PerformanceSample ReadSample()
    {
        var sample = new PerformanceSample();

        try { sample.CpuUsagePercent = Math.Round(_cpuCounter?.NextValue() ?? 0, 1); }
        catch { sample.CpuUsagePercent = 0; }

        try
        {
            NativeMemoryStatus.GlobalMemoryStatusEx(out var status);
            sample.RamTotalBytes = (long)status.ullTotalPhys;
            sample.RamUsedBytes = (long)(status.ullTotalPhys - status.ullAvailPhys);
            sample.RamUsedPercent = sample.RamTotalBytes == 0 ? 0 : Math.Round((double)sample.RamUsedBytes / sample.RamTotalBytes * 100, 1);
        }
        catch
        {
            // leave zeros
        }

        if (IsGpuMonitoringAvailable)
        {
            sample.GpuUsagePercent = ReadGpuUsage();
            sample.GpuMemoryUsedBytes = ReadGpuMemoryUsage();
        }

        if (_nvidiaSmiPath is not null)
        {
            sample.GpuTemperatureCelsius = ReadNvidiaTemperature();
        }

        return sample;
    }

    private double? ReadGpuUsage()
    {
        try
        {
            var currentInstances = new PerformanceCounterCategory("GPU Engine").GetInstanceNames()
                .Where(n => n.Contains("engtype_3D", StringComparison.OrdinalIgnoreCase))
                .ToHashSet();

            foreach (var stale in _gpuEngineCounters.Keys.Except(currentInstances).ToList())
            {
                _gpuEngineCounters[stale].Dispose();
                _gpuEngineCounters.Remove(stale);
            }

            foreach (var instance in currentInstances)
            {
                if (_gpuEngineCounters.ContainsKey(instance)) continue;
                try
                {
                    var counter = new PerformanceCounter("GPU Engine", "Utilization Percentage", instance, readOnly: true);
                    counter.NextValue(); // prime — first read of a new counter is always 0
                    _gpuEngineCounters[instance] = counter;
                }
                catch
                {
                    // Instance disappeared between enumeration and creation — skip it.
                }
            }

            if (_gpuEngineCounters.Count == 0) return 0;

            var total = 0.0;
            foreach (var counter in _gpuEngineCounters.Values)
            {
                try { total += counter.NextValue(); }
                catch { /* instance may have just been torn down */ }
            }

            return Math.Round(Math.Min(total, 100), 1);
        }
        catch
        {
            return null;
        }
    }

    private static long? ReadGpuMemoryUsage()
    {
        try
        {
            if (!PerformanceCounterCategory.Exists("GPU Process Memory")) return null;

            var category = new PerformanceCounterCategory("GPU Process Memory");
            var instances = category.GetInstanceNames();
            long total = 0;

            foreach (var instance in instances)
            {
                using var counter = new PerformanceCounter("GPU Process Memory", "Dedicated Usage", instance, readOnly: true);
                total += (long)counter.NextValue();
            }

            return total;
        }
        catch
        {
            return null;
        }
    }

    private static string? LocateNvidiaSmi()
    {
        var candidates = new[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "NVIDIA Corporation", "NVSMI", "nvidia-smi.exe"),
            Path.Combine(Environment.SystemDirectory, "nvidia-smi.exe")
        };

        return candidates.FirstOrDefault(File.Exists);
    }

    private double? ReadNvidiaTemperature()
    {
        if (_nvidiaSmiPath is null) return null;

        try
        {
            var psi = new ProcessStartInfo(_nvidiaSmiPath, "--query-gpu=temperature.gpu --format=csv,noheader")
            {
                CreateNoWindow = true,
                UseShellExecute = false,
                RedirectStandardOutput = true
            };

            using var process = Process.Start(psi);
            if (process is null) return null;

            var output = process.StandardOutput.ReadToEnd().Trim();
            process.WaitForExit(2000);

            return double.TryParse(output, out var temp) ? temp : null;
        }
        catch
        {
            return null;
        }
    }

    public void Dispose()
    {
        _cpuCounter?.Dispose();
        foreach (var counter in _gpuEngineCounters.Values)
            counter.Dispose();
        _gpuEngineCounters.Clear();
    }
}
