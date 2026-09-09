using System.Diagnostics;
using System.Linq;
using System.Management;
using NvAPIWrapper.GPU;

namespace ResetFpsBooster.BottleneckEngine.Collectors;

public sealed class VramTelemetrySample
{
    public long? UsedBytes { get; set; }
    public long? TotalBytes { get; set; }
}

/// <summary>
/// Prefers NVIDIA's own NVAPI memory info (accurate to the byte, no size cap) when an NVIDIA GPU
/// is present. Falls back to the vendor-agnostic "GPU Process Memory" counter for used VRAM and
/// WMI's Win32_VideoController.AdapterRAM for total — the latter is a known 32-bit field that
/// wraps around above ~4 GB on some driver/OS combinations, so it's a best-effort fallback only,
/// never used when the accurate NVAPI figure is available.
/// </summary>
public sealed class VramTelemetryCollector : IDisposable
{
    private readonly PhysicalGPU? _nvidiaGpu;
    private readonly long? _wmiTotalBytes;

    public VramTelemetryCollector(PhysicalGPU? nvidiaGpu)
    {
        _nvidiaGpu = nvidiaGpu;
        _wmiTotalBytes = _nvidiaGpu is null ? ReadAdapterRamFromWmi() : null;
    }

    public VramTelemetrySample Read()
    {
        if (_nvidiaGpu is not null)
        {
            try
            {
                var info = _nvidiaGpu.MemoryInformation;
                var totalKb = info.DedicatedVideoMemoryInkB;
                var availableKb = info.CurrentAvailableDedicatedVideoMemoryInkB;
                if (totalKb > 0)
                {
                    return new VramTelemetrySample
                    {
                        TotalBytes = (long)totalKb * 1024,
                        UsedBytes = (long)Math.Max(0, totalKb - availableKb) * 1024
                    };
                }
            }
            catch { /* fall through to the generic path below */ }
        }

        return new VramTelemetrySample
        {
            TotalBytes = _wmiTotalBytes,
            UsedBytes = ReadUsedFromProcessMemoryCounter()
        };
    }

    private static long? ReadUsedFromProcessMemoryCounter()
    {
        try
        {
            if (!PerformanceCounterCategory.Exists("GPU Process Memory")) return null;

            var instances = new PerformanceCounterCategory("GPU Process Memory").GetInstanceNames();
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

    private static long? ReadAdapterRamFromWmi()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT AdapterRAM FROM Win32_VideoController");
            var rams = searcher.Get().Cast<ManagementObject>()
                .Select(o => Convert.ToInt64(o["AdapterRAM"] ?? 0L))
                .Where(r => r > 0)
                .ToList();

            return rams.Count == 0 ? null : rams.Max();
        }
        catch
        {
            return null;
        }
    }

    public void Dispose() { }
}
