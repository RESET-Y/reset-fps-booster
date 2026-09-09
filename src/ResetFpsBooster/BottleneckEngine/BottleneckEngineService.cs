using System.Diagnostics;
using System.IO;
using System.Linq;
using ResetFpsBooster.BottleneckEngine.Analyzers;
using ResetFpsBooster.BottleneckEngine.Collectors;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.BottleneckEngine;

/// <summary>
/// Owns every telemetry collector, builds one <see cref="TelemetrySnapshot"/> per sample, runs it
/// through every analyzer and the classifier, and keeps a short rolling history both for trend-
/// aware analysis and for the dynamic "what changed over time" view. Instantiated only while its
/// page is open (same lifecycle as <c>PerformanceMonitorService</c>) so it costs nothing when not
/// in use — no background polling loop.
/// </summary>
public sealed class BottleneckEngineService : IBottleneckEngineService
{
    private const int RecentHistoryWindowSize = 6;
    private const int MaxTrendHistoryEntries = 40;

    private readonly IGameLibraryService _gameLibraryService;
    private readonly CpuTelemetryCollector _cpu = new();
    private readonly GpuTelemetryCollector _gpu = new();
    private readonly VramTelemetryCollector _vram;
    private readonly RamTelemetryCollector _ram = new();
    private readonly DiskTelemetryCollector _disk = new();
    private readonly ProcessTelemetryCollector _process = new();
    private readonly BottleneckClassifier _classifier;
    private readonly List<TelemetrySnapshot> _recentSnapshots = new();
    private readonly List<BottleneckHistoryEntry> _trendHistory = new();

    public IReadOnlyList<BottleneckHistoryEntry> History => _trendHistory;

    public BottleneckEngineService(IGameLibraryService gameLibraryService)
    {
        _gameLibraryService = gameLibraryService;

        // GpuTelemetryCollector's constructor already called NVIDIA.Initialize() above if an
        // NVIDIA GPU is present, so this call is cheap (NVAPI ref-counts init calls).
        _vram = new VramTelemetryCollector(TryGetNvidiaGpuSafely());

        _classifier = new BottleneckClassifier(new IBottleneckAnalyzer[]
        {
            new CpuAnalyzer(),
            new GpuAnalyzer(),
            new ThermalAnalyzer(),
            new PowerAnalyzer(),
            new MemoryAnalyzer(),
            new VramAnalyzer(),
            new ProcessAnalyzer()
        });
    }

    private static NvAPIWrapper.GPU.PhysicalGPU? TryGetNvidiaGpuSafely()
    {
        try { return NvAPIWrapper.GPU.PhysicalGPU.GetPhysicalGPUs().FirstOrDefault(); }
        catch { return null; }
    }

    public BottleneckReport Sample()
    {
        var gameName = DetectRunningGame(out var gameExecutableName);

        var cpuUsage = _cpu.ReadUsage();
        var gpuSample = _gpu.Read();
        var vramSample = _vram.Read();
        var ramSample = _ram.Read();
        var diskSample = _disk.Read();
        var backgroundProcesses = _process.Read(gameExecutableName);

        var snapshot = new TelemetrySnapshot
        {
            GameName = gameName,
            CpuTotalUsagePercent = cpuUsage.Total,
            CpuPerCoreUsagePercent = cpuUsage.PerCore,
            CpuAverageClockMhz = _cpu.ReadAverageClockMhz(),
            CpuMaxClockMhz = _cpu.MaxClockMhz,
            CpuFrequencyScalingDetected = IsFrequencyScalingDetected(cpuUsage.Total, _cpu.ReadAverageClockMhz(), _cpu.MaxClockMhz),

            GpuUsagePercent = gpuSample.UsagePercent,
            GpuClockMhz = gpuSample.GraphicsClockMhz,
            GpuMemoryClockMhz = gpuSample.MemoryClockMhz,
            GpuTemperatureCelsius = gpuSample.TemperatureCelsius,
            GpuPowerPercentOfLimit = gpuSample.PowerPercentOfLimit,
            GpuVendorIsNvidia = gpuSample.IsNvidia,

            VramUsedBytes = vramSample.UsedBytes,
            VramTotalBytes = vramSample.TotalBytes,

            RamUsedBytes = ramSample.UsedBytes,
            RamTotalBytes = ramSample.TotalBytes,
            RamHardFaultsPerSec = ramSample.HardFaultsPerSec,

            DiskActivePercent = diskSample.ActivePercent,
            DiskResponseTimeMs = diskSample.ResponseTimeMs,

            TopBackgroundProcesses = backgroundProcesses

            // Frametime / 1% low / 0.1% low intentionally left null — see TelemetrySnapshot's own
            // remarks. Real values require frame-capture instrumentation (PresentMon/ETW) this
            // engine does not yet implement.
        };

        _recentSnapshots.Add(snapshot);
        if (_recentSnapshots.Count > RecentHistoryWindowSize)
            _recentSnapshots.RemoveAt(0);

        var report = _classifier.Classify(snapshot, _recentSnapshots.Take(_recentSnapshots.Count - 1).ToList());

        RecordTrend(report);

        return report;
    }

    private void RecordTrend(BottleneckReport report)
    {
        var primaryKind = report.Diagnoses.Count > 0 ? report.Diagnoses[0].Kind : (BottleneckKind?)null;
        var last = _trendHistory.LastOrDefault();

        if (last is null || last.Kind != primaryKind)
        {
            _trendHistory.Add(new BottleneckHistoryEntry { Timestamp = report.Timestamp, Kind = primaryKind });
            if (_trendHistory.Count > MaxTrendHistoryEntries)
                _trendHistory.RemoveAt(0);
        }
    }

    private static bool IsFrequencyScalingDetected(double? usage, double? currentClock, double? maxClock)
    {
        if (usage is not > 85 || currentClock is null || maxClock is not > 0) return false;
        return currentClock.Value / maxClock.Value < 0.75;
    }

    private string? DetectRunningGame(out string? executableName)
    {
        executableName = null;

        var games = _gameLibraryService.GetGames()
            .Where(g => !string.IsNullOrEmpty(g.ExecutablePath))
            .ToList();

        if (games.Count == 0) return null;

        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    var match = games.FirstOrDefault(g =>
                        string.Equals(Path.GetFileNameWithoutExtension(g.ExecutablePath), process.ProcessName, StringComparison.OrdinalIgnoreCase));

                    if (match is not null)
                    {
                        executableName = process.ProcessName;
                        return match.Name;
                    }
                }
                catch { /* inaccessible process — skip */ }
            }
        }

        return null;
    }

    public void Dispose()
    {
        _cpu.Dispose();
        _gpu.Dispose();
        _vram.Dispose();
        _ram.Dispose();
        _disk.Dispose();
    }
}
