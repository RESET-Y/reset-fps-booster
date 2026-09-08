using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IPerformanceMonitorService : IDisposable
{
    /// <summary>True if the GPU usage/memory counters could be initialized on this machine.</summary>
    bool IsGpuMonitoringAvailable { get; }
    /// <summary>True if a vendor tool (currently nvidia-smi for NVIDIA) exposed GPU temperature.</summary>
    bool IsGpuTemperatureAvailable { get; }

    PerformanceSample ReadSample();
}
