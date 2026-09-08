namespace ResetFpsBooster.Core.Models;

public sealed class PerformanceSample
{
    public double CpuUsagePercent { get; set; }
    public double RamUsedPercent { get; set; }
    public long RamUsedBytes { get; set; }
    public long RamTotalBytes { get; set; }
    public double? GpuUsagePercent { get; set; }
    public long? GpuMemoryUsedBytes { get; set; }
    public double? GpuTemperatureCelsius { get; set; }
    public DateTime Timestamp { get; set; } = DateTime.Now;
}
