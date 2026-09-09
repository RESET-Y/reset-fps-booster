using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine;

/// <summary>Builds the human-readable side of a diagnosis strictly from what was actually measured
/// this tick — the headline names the kind, but every sentence and every evidence row is either an
/// analyzer's own reason string (already built from real numbers) or a value read straight from
/// the snapshot. Nothing here is templated filler unconnected to the data.</summary>
public sealed class BottleneckExplanationGenerator
{
    public BottleneckDiagnosis Build(BottleneckKind kind, double confidence, TelemetrySnapshot snapshot, List<string> reasons)
    {
        var (headline, intro) = HeadlineAndIntro(kind);
        var explanation = reasons.Count > 0 ? $"{intro} {reasons[0]}" : intro;

        return new BottleneckDiagnosis
        {
            Kind = kind,
            ConfidencePercent = confidence,
            Headline = headline,
            Explanation = explanation,
            Evidence = BuildEvidence(kind, snapshot)
        };
    }

    private static (string Headline, string Intro) HeadlineAndIntro(BottleneckKind kind) => kind switch
    {
        BottleneckKind.CpuLimited => ("CPU LIMITED", "Your CPU is currently limiting GPU utilization in this workload."),
        BottleneckKind.GpuLimited => ("GPU LIMITED", "Your GPU is the dominant factor right now — it is working close to its ceiling."),
        BottleneckKind.ThermalLimited => ("THERMAL LIMITED", "Heat is currently capping performance before power or workload demand does."),
        BottleneckKind.PowerLimited => ("POWER LIMITED", "Your GPU is hitting its configured power limit, which is capping its clock speed."),
        BottleneckKind.MemoryLimited => ("MEMORY LIMITED", "Available RAM is under real pressure, not just heavily used."),
        BottleneckKind.VramLimited => ("VRAM LIMITED", "Video memory is nearly exhausted for the current workload."),
        BottleneckKind.BackgroundWorkload => ("BACKGROUND WORKLOAD", "Other running processes are consuming resources your game could otherwise use."),
        _ => ("UNKNOWN", string.Empty)
    };

    private static List<(string Label, string Value)> BuildEvidence(BottleneckKind kind, TelemetrySnapshot s)
    {
        var evidence = new List<(string, string)>();

        void Add(string label, string? value)
        {
            if (value is not null) evidence.Add((label, value));
        }

        switch (kind)
        {
            case BottleneckKind.CpuLimited:
                Add("CPU Thread Saturation", Format(s.CpuMaxCoreUsagePercent, "0"));
                Add("GPU Utilization", Format(s.GpuUsagePercent, "0"));
                Add("CPU Clock", FormatMhz(s.CpuAverageClockMhz));
                break;
            case BottleneckKind.GpuLimited:
                Add("GPU Utilization", Format(s.GpuUsagePercent, "0"));
                Add("GPU Clock", FormatMhz(s.GpuClockMhz));
                Add("GPU Power", Format(s.GpuPowerPercentOfLimit, "0", "% of limit"));
                break;
            case BottleneckKind.ThermalLimited:
                Add("GPU Temperature", Format(s.GpuTemperatureCelsius, "0", "°C"));
                Add("GPU Utilization", Format(s.GpuUsagePercent, "0"));
                Add("GPU Clock", FormatMhz(s.GpuClockMhz));
                break;
            case BottleneckKind.PowerLimited:
                Add("GPU Power", Format(s.GpuPowerPercentOfLimit, "0", "% of limit"));
                Add("GPU Clock", FormatMhz(s.GpuClockMhz));
                Add("GPU Temperature", Format(s.GpuTemperatureCelsius, "0", "°C"));
                break;
            case BottleneckKind.MemoryLimited:
                Add("RAM Usage", Format(s.RamUsedPercent, "0"));
                Add("Hard Page Faults", s.RamHardFaultsPerSec.HasValue ? $"{s.RamHardFaultsPerSec:0}/sec" : "Sensor unavailable");
                break;
            case BottleneckKind.VramLimited:
                Add("VRAM Usage", Format(s.VramUsedPercent, "0"));
                Add("Frametime", s.IsFrametimeAvailable ? $"{s.FrametimeMs:0.00} ms" : "Sensor unavailable");
                break;
            case BottleneckKind.BackgroundWorkload:
                Add("Background CPU Usage", $"{s.BackgroundCpuPercent:0}%");
                if (s.TopBackgroundProcesses.Count > 0)
                    Add("Top Process", $"{s.TopBackgroundProcesses[0].ProcessName} ({s.TopBackgroundProcesses[0].CpuPercent:0}%)");
                break;
        }

        Add("Frametime", kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.IsFrametimeAvailable ? $"{s.FrametimeMs:0.00} ms" : "Sensor unavailable")
            : null);
        Add("1% Low", kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.Fps1PercentLow.HasValue ? $"{s.Fps1PercentLow:0} FPS" : "Sensor unavailable")
            : null);
        Add("0.1% Low", kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.Fps01PercentLow.HasValue ? $"{s.Fps01PercentLow:0} FPS" : "Sensor unavailable")
            : null);

        return evidence;
    }

    private static string? Format(double? value, string numberFormat, string suffix = "%") =>
        value.HasValue ? $"{value.Value.ToString("F" + (numberFormat == "0" ? 0 : 1))}{suffix}" : "Sensor unavailable";

    private static string? FormatMhz(double? mhz) => mhz.HasValue ? $"{mhz.Value:0} MHz" : "Sensor unavailable";
}
