namespace ResetFpsBooster.Core.Models;

public sealed class CpuInfo
{
    public string Name { get; set; } = "Unknown";
    public string Manufacturer { get; set; } = "Unknown";
    public int Cores { get; set; }
    public int LogicalProcessors { get; set; }
    public double MaxClockSpeedGhz { get; set; }
    public double CurrentUsagePercent { get; set; }
}

public sealed class GpuInfo
{
    public string Name { get; set; } = "Unknown";
    public string DriverVersion { get; set; } = "Unknown";
    public DateTime? DriverDate { get; set; }
    public long AdapterRamBytes { get; set; }
    public string Vendor { get; set; } = "Unknown"; // NVIDIA / AMD / Intel / Unknown
}

public sealed class MemoryInfo
{
    public long TotalBytes { get; set; }
    public long AvailableBytes { get; set; }
    public int ModuleCount { get; set; }
    public double SpeedMhz { get; set; }

    public long UsedBytes => Math.Max(0, TotalBytes - AvailableBytes);
    public double UsedPercent => TotalBytes == 0 ? 0 : (double)UsedBytes / TotalBytes * 100.0;
}

public sealed class StorageDrive
{
    public string DriveLetter { get; set; } = string.Empty;
    public string VolumeLabel { get; set; } = string.Empty;
    public long TotalBytes { get; set; }
    public long FreeBytes { get; set; }
    public string MediaType { get; set; } = "Unknown"; // SSD / HDD / Unknown
    public long UsedBytes => Math.Max(0, TotalBytes - FreeBytes);
    public double UsedPercent => TotalBytes == 0 ? 0 : (double)UsedBytes / TotalBytes * 100.0;
}

public sealed class MotherboardInfo
{
    public string Manufacturer { get; set; } = "Unknown";
    public string Product { get; set; } = "Unknown";
}

public sealed class OsInfo
{
    public string ProductName { get; set; } = "Unknown";
    public string Version { get; set; } = "Unknown";
    public string Build { get; set; } = "Unknown";
    public string Architecture { get; set; } = "Unknown";
    public bool IsGamingOptimizedOsVersion { get; set; }
}

public sealed class SystemSnapshot
{
    public CpuInfo Cpu { get; set; } = new();
    public GpuInfo? PrimaryGpu { get; set; }
    public List<GpuInfo> AllGpus { get; set; } = new();
    public MemoryInfo Memory { get; set; } = new();
    public List<StorageDrive> Drives { get; set; } = new();
    public MotherboardInfo Motherboard { get; set; } = new();
    public OsInfo OperatingSystem { get; set; } = new();
    public bool IsLaptop { get; set; }
    public DateTime CapturedAt { get; set; } = DateTime.Now;
}
