using ResetFpsBooster.Core.Localization;
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
        BottleneckKind.CpuLimited => (Loc.T("Bn.H.Cpu"), Loc.T("Bn.I.Cpu")),
        BottleneckKind.GpuLimited => (Loc.T("Bn.H.Gpu"), Loc.T("Bn.I.Gpu")),
        BottleneckKind.ThermalLimited => (Loc.T("Bn.H.Thermal"), Loc.T("Bn.I.Thermal")),
        BottleneckKind.PowerLimited => (Loc.T("Bn.H.Power"), Loc.T("Bn.I.Power")),
        BottleneckKind.MemoryLimited => (Loc.T("Bn.H.Memory"), Loc.T("Bn.I.Memory")),
        BottleneckKind.VramLimited => (Loc.T("Bn.H.Vram"), Loc.T("Bn.I.Vram")),
        BottleneckKind.BackgroundWorkload => (Loc.T("Bn.H.Background"), Loc.T("Bn.I.Background")),
        _ => (Loc.T("Bn.H.Unknown"), string.Empty)
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
                Add(Loc.T("Bn.E.CpuThreads"), Format(s.CpuMaxCoreUsagePercent, "0"));
                Add(Loc.T("Bn.E.GpuUtil"), Format(s.GpuUsagePercent, "0"));
                Add(Loc.T("Bn.E.CpuClock"), FormatMhz(s.CpuAverageClockMhz));
                break;
            case BottleneckKind.GpuLimited:
                Add(Loc.T("Bn.E.GpuUtil"), Format(s.GpuUsagePercent, "0"));
                Add(Loc.T("Bn.E.GpuClock"), FormatMhz(s.GpuClockMhz));
                Add(Loc.T("Bn.E.GpuPower"), Format(s.GpuPowerPercentOfLimit, "0", Loc.T("Bn.E.OfLimit")));
                break;
            case BottleneckKind.ThermalLimited:
                Add(Loc.T("Bn.E.GpuTemp"), Format(s.GpuTemperatureCelsius, "0", "°C"));
                Add(Loc.T("Bn.E.GpuUtil"), Format(s.GpuUsagePercent, "0"));
                Add(Loc.T("Bn.E.GpuClock"), FormatMhz(s.GpuClockMhz));
                break;
            case BottleneckKind.PowerLimited:
                Add(Loc.T("Bn.E.GpuPower"), Format(s.GpuPowerPercentOfLimit, "0", Loc.T("Bn.E.OfLimit")));
                Add(Loc.T("Bn.E.GpuClock"), FormatMhz(s.GpuClockMhz));
                Add(Loc.T("Bn.E.GpuTemp"), Format(s.GpuTemperatureCelsius, "0", "°C"));
                break;
            case BottleneckKind.MemoryLimited:
                Add(Loc.T("Bn.E.Ram"), Format(s.RamUsedPercent, "0"));
                Add(Loc.T("Bn.E.HardFaults"), s.RamHardFaultsPerSec.HasValue ? $"{s.RamHardFaultsPerSec:0}/sec" : Loc.T("Bn.E.SensorNA"));
                break;
            case BottleneckKind.VramLimited:
                Add(Loc.T("Bn.E.Vram"), Format(s.VramUsedPercent, "0"));
                Add(Loc.T("Bn.E.Frametime"), s.IsFrametimeAvailable ? $"{s.FrametimeMs:0.00} ms" : Loc.T("Bn.E.SensorNA"));
                break;
            case BottleneckKind.BackgroundWorkload:
                Add(Loc.T("Bn.E.BackgroundCpu"), $"{s.BackgroundCpuPercent:0}%");
                if (s.TopBackgroundProcesses.Count > 0)
                    Add(Loc.T("Bn.E.TopProcess"), $"{s.TopBackgroundProcesses[0].ProcessName} ({s.TopBackgroundProcesses[0].CpuPercent:0}%)");
                break;
        }

        Add(Loc.T("Bn.E.Frametime"), kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.IsFrametimeAvailable ? $"{s.FrametimeMs:0.00} ms" : Loc.T("Bn.E.SensorNA"))
            : null);
        Add("1% Low", kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.Fps1PercentLow.HasValue ? $"{s.Fps1PercentLow:0} FPS" : Loc.T("Bn.E.SensorNA"))
            : null);
        Add("0.1% Low", kind is BottleneckKind.CpuLimited or BottleneckKind.GpuLimited
            ? (s.Fps01PercentLow.HasValue ? $"{s.Fps01PercentLow:0} FPS" : Loc.T("Bn.E.SensorNA"))
            : null);

        return evidence;
    }

    private static string? Format(double? value, string numberFormat, string suffix = "%") =>
        value.HasValue ? $"{value.Value.ToString("F" + (numberFormat == "0" ? 0 : 1))}{suffix}" : Loc.T("Bn.E.SensorNA");

    private static string? FormatMhz(double? mhz) => mhz.HasValue ? $"{mhz.Value:0} MHz" : Loc.T("Bn.E.SensorNA");
}
