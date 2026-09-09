namespace ResetFpsBooster.Core.Models;

/// <summary>
/// One tick's worth of raw system telemetry. Every field is nullable — null means the sensor
/// genuinely isn't available on this machine, never a fabricated placeholder. Collectors populate
/// this; analyzers only ever read from it, never invent values that aren't here.
/// </summary>
public sealed class TelemetrySnapshot
{
    public DateTime Timestamp { get; set; } = DateTime.Now;
    public string? GameName { get; set; }

    // CPU
    public double? CpuTotalUsagePercent { get; set; }
    public double[]? CpuPerCoreUsagePercent { get; set; }
    public double? CpuAverageClockMhz { get; set; }
    public double? CpuMaxClockMhz { get; set; }
    public double? CpuTemperatureCelsius { get; set; }
    public bool CpuFrequencyScalingDetected { get; set; }

    // GPU
    public double? GpuUsagePercent { get; set; }
    public double? GpuClockMhz { get; set; }
    public double? GpuMemoryClockMhz { get; set; }
    public double? GpuTemperatureCelsius { get; set; }
    public double? GpuPowerPercentOfLimit { get; set; }
    public bool GpuVendorIsNvidia { get; set; }

    // VRAM
    public long? VramUsedBytes { get; set; }
    public long? VramTotalBytes { get; set; }

    // RAM
    public long? RamUsedBytes { get; set; }
    public long? RamTotalBytes { get; set; }
    public double? RamHardFaultsPerSec { get; set; }

    // Storage
    public double? DiskActivePercent { get; set; }
    public double? DiskResponseTimeMs { get; set; }

    // Background workload
    public List<BackgroundProcessLoad> TopBackgroundProcesses { get; set; } = new();
    public double BackgroundCpuPercent => TopBackgroundProcesses.Sum(p => p.CpuPercent);

    // Frame delivery — genuinely unavailable until real frame-capture (PresentMon/ETW) exists.
    // Never estimated from CPU/GPU usage; that would be exactly the fabricated number this engine
    // exists to avoid.
    public double? FrametimeMs { get; set; }
    public double? Fps1PercentLow { get; set; }
    public double? Fps01PercentLow { get; set; }
    public bool IsFrametimeAvailable => FrametimeMs.HasValue;

    public double? GpuUsageHeadroomPercent => GpuUsagePercent.HasValue ? Math.Max(0, 100 - GpuUsagePercent.Value) : null;
    public double? RamUsedPercent => RamTotalBytes is > 0 ? Math.Round((double)RamUsedBytes! / RamTotalBytes.Value * 100, 1) : null;
    public double? VramUsedPercent => VramTotalBytes is > 0 ? Math.Round((double)VramUsedBytes! / VramTotalBytes.Value * 100, 1) : null;
    public double? CpuMaxCoreUsagePercent => CpuPerCoreUsagePercent is { Length: > 0 } ? CpuPerCoreUsagePercent.Max() : null;
}

public sealed class BackgroundProcessLoad
{
    public string ProcessName { get; set; } = string.Empty;
    public double CpuPercent { get; set; }
}

public enum BottleneckKind
{
    CpuLimited,
    GpuLimited,
    ThermalLimited,
    PowerLimited,
    MemoryLimited,
    VramLimited,
    BackgroundWorkload
}

/// <summary>One analyzer's vote toward a possible diagnosis. Multiple signals for the same
/// <see cref="Kind"/> are combined by the classifier into a single confidence score — no single
/// analyzer decides anything by itself.</summary>
public sealed class BottleneckSignal
{
    public BottleneckKind Kind { get; set; }
    public double Weight { get; set; }
    public string Reason { get; set; } = string.Empty;
}

public sealed class BottleneckDiagnosis
{
    public BottleneckKind Kind { get; set; }
    public double ConfidencePercent { get; set; }
    public string Headline { get; set; } = string.Empty;
    public string Explanation { get; set; } = string.Empty;
    public List<(string Label, string Value)> Evidence { get; set; } = new();
}

public sealed class BottleneckHistoryEntry
{
    public DateTime Timestamp { get; set; }
    public BottleneckKind? Kind { get; set; }
}

public sealed class BottleneckReport
{
    public DateTime Timestamp { get; set; } = DateTime.Now;
    public string? GameName { get; set; }
    public List<BottleneckDiagnosis> Diagnoses { get; set; } = new();
    public bool IsBalanced => Diagnoses.Count == 0;
    public TelemetrySnapshot? Snapshot { get; set; }
}
