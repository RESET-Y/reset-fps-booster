using System.Diagnostics;
using System.Linq;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

/// <summary>
/// Computes real per-process CPU% by tracking each process's cumulative CPU time between two
/// samples (the same technique Task Manager itself uses) — never a snapshot-only guess. Only
/// reports processes that are neither this app, the currently detected game, nor a core OS
/// process, since those aren't "background workload" in any meaningful sense.
/// </summary>
public sealed class ProcessTelemetryCollector
{
    private static readonly string[] ExcludedProcessNames =
    {
        "system", "idle", "registry", "smss", "csrss", "wininit", "services", "lsass",
        "winlogon", "memcompression", "dwm", "resetfpsbooster", "secure system", "svchost"
    };

    private Dictionary<int, TimeSpan> _lastCpuTimes = new();
    private DateTime _lastSampleAt = DateTime.MinValue;

    public List<BackgroundProcessLoad> Read(string? activeGameProcessName, int topN = 5)
    {
        var now = DateTime.Now;
        var elapsed = _lastSampleAt == DateTime.MinValue ? TimeSpan.Zero : now - _lastSampleAt;
        var currentTimes = new Dictionary<int, TimeSpan>();
        var loads = new List<BackgroundProcessLoad>();
        var processorCount = Math.Max(1, Environment.ProcessorCount);
        var currentProcessId = Environment.ProcessId;

        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    if (process.Id == currentProcessId) continue;
                    if (ExcludedProcessNames.Contains(process.ProcessName, StringComparer.OrdinalIgnoreCase)) continue;
                    if (activeGameProcessName is not null && string.Equals(process.ProcessName, activeGameProcessName, StringComparison.OrdinalIgnoreCase)) continue;

                    var cpuTime = process.TotalProcessorTime;
                    currentTimes[process.Id] = cpuTime;

                    if (elapsed <= TimeSpan.Zero) continue;
                    if (!_lastCpuTimes.TryGetValue(process.Id, out var previous)) continue;

                    var deltaMs = (cpuTime - previous).TotalMilliseconds;
                    if (deltaMs <= 0) continue;

                    var percent = deltaMs / elapsed.TotalMilliseconds / processorCount * 100.0;
                    if (percent >= 1.0)
                        loads.Add(new BackgroundProcessLoad { ProcessName = process.ProcessName, CpuPercent = Math.Round(percent, 1) });
                }
                catch
                {
                    // Protected/system-owned process, or it exited mid-scan — skip.
                }
            }
        }

        _lastCpuTimes = currentTimes;
        _lastSampleAt = now;

        return loads.OrderByDescending(l => l.CpuPercent).Take(topN).ToList();
    }
}
