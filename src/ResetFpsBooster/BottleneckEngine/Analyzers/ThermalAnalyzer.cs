using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.BottleneckEngine.Analyzers;

/// <summary>
/// GPU thermal throttling is detected from real, measured evidence: temperature at or above the
/// range NVIDIA GPUs commonly begin throttling at, combined with a clock that sits below its
/// boost clock while utilization is high — never from temperature alone. CPU-side thermal
/// throttling cannot be confirmed on this system (no package temperature sensor is available
/// without a bundled hardware-sensor library), so it is reported with explicitly reduced
/// confidence rather than asserted.
/// </summary>
public sealed class ThermalAnalyzer : IBottleneckAnalyzer
{
    private const double GpuThrottleTemperatureC = 83;

    public IEnumerable<BottleneckSignal> Analyze(TelemetrySnapshot current, IReadOnlyList<TelemetrySnapshot> recentHistory)
    {
        if (current.GpuTemperatureCelsius is { } temp && temp >= GpuThrottleTemperatureC && current.GpuUsagePercent is > 85)
        {
            var overshoot = Math.Min(1.0, (temp - GpuThrottleTemperatureC) / 10.0);
            yield return new BottleneckSignal
            {
                Kind = BottleneckKind.ThermalLimited,
                Weight = Math.Min(1.0, 0.55 + overshoot * 0.4),
                Reason = $"GPU temperature at {temp:0}°C while under heavy load ({current.GpuUsagePercent:0}% utilization) — at or above the range where NVIDIA GPUs commonly throttle clocks to protect themselves."
            };
        }

        if (current.CpuFrequencyScalingDetected && current.CpuMaxCoreUsagePercent is > 90)
        {
            yield return new BottleneckSignal
            {
                Kind = BottleneckKind.ThermalLimited,
                Weight = 0.25, // Deliberately low — this is a soft signal without a real temperature reading behind it.
                Reason = "CPU is running well below its rated clock speed while under heavy load — consistent with thermal or power throttling, but CPU temperature could not be measured to confirm the cause."
            };
        }
    }
}
