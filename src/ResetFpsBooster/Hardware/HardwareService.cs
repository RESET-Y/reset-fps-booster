using System.IO;
using System.Management;
using System.Runtime.InteropServices;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;
using Microsoft.Win32;

namespace ResetFpsBooster.Hardware;

/// <summary>Reads real hardware and OS information via WMI and the .NET runtime — no fabricated values.</summary>
public sealed class HardwareService : IHardwareService
{
    public Task<SystemSnapshot> GetSnapshotAsync(CancellationToken ct = default)
        => Task.Run(() => BuildSnapshot(ct), ct);

    private static SystemSnapshot BuildSnapshot(CancellationToken ct)
    {
        var snapshot = new SystemSnapshot
        {
            Cpu = ReadCpu(),
            Memory = ReadMemory(),
            Motherboard = ReadMotherboard(),
            OperatingSystem = ReadOs(),
            Drives = ReadDrives(),
            IsLaptop = DetectIsLaptop()
        };

        ct.ThrowIfCancellationRequested();

        snapshot.AllGpus = ReadGpus();
        snapshot.PrimaryGpu = snapshot.AllGpus.FirstOrDefault(g => g.AdapterRamBytes > 0) ?? snapshot.AllGpus.FirstOrDefault();

        return snapshot;
    }

    private static CpuInfo ReadCpu()
    {
        var info = new CpuInfo();
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed FROM Win32_Processor");
            foreach (var obj in searcher.Get())
            {
                info.Name = (obj["Name"] as string)?.Trim() ?? info.Name;
                info.Manufacturer = (obj["Manufacturer"] as string)?.Trim() ?? info.Manufacturer;
                info.Cores += Convert.ToInt32(obj["NumberOfCores"] ?? 0);
                info.LogicalProcessors += Convert.ToInt32(obj["NumberOfLogicalProcessors"] ?? 0);
                var clock = Convert.ToInt32(obj["MaxClockSpeed"] ?? 0);
                info.MaxClockSpeedGhz = Math.Max(info.MaxClockSpeedGhz, clock / 1000.0);
            }
        }
        catch
        {
            info.Name = "Unavailable (WMI query failed)";
        }

        try
        {
            using var cpuCounter = new System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total");
            cpuCounter.NextValue();
            Thread.Sleep(200);
            info.CurrentUsagePercent = Math.Round(cpuCounter.NextValue(), 1);
        }
        catch
        {
            info.CurrentUsagePercent = -1;
        }

        return info;
    }

    private static List<GpuInfo> ReadGpus()
    {
        var gpus = new List<GpuInfo>();
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Name, DriverVersion, DriverDate, AdapterRAM, PNPDeviceID FROM Win32_VideoController");
            foreach (var obj in searcher.Get())
            {
                var name = (obj["Name"] as string)?.Trim() ?? "Unknown GPU";
                var vendor = "Unknown";
                var lowerName = name.ToLowerInvariant();
                if (lowerName.Contains("nvidia")) vendor = "NVIDIA";
                else if (lowerName.Contains("amd") || lowerName.Contains("radeon")) vendor = "AMD";
                else if (lowerName.Contains("intel")) vendor = "Intel";

                DateTime? driverDate = null;
                if (obj["DriverDate"] is string wmiDate && wmiDate.Length >= 8)
                {
                    try { driverDate = ManagementDateTimeConverter.ToDateTime(wmiDate); }
                    catch { /* ignore unparsable WMI date */ }
                }

                long adapterRam = 0;
                try { adapterRam = Convert.ToInt64(obj["AdapterRAM"] ?? 0L); }
                catch { /* some drivers report this as negative/overflowed on >4GB cards */ }

                gpus.Add(new GpuInfo
                {
                    Name = name,
                    DriverVersion = (obj["DriverVersion"] as string) ?? "Unknown",
                    DriverDate = driverDate,
                    AdapterRamBytes = adapterRam,
                    Vendor = vendor
                });
            }
        }
        catch
        {
            gpus.Add(new GpuInfo { Name = "Unavailable (WMI query failed)" });
        }

        return gpus;
    }

    private static MemoryInfo ReadMemory()
    {
        var info = new MemoryInfo();
        try
        {
            NativeMemoryStatus.GlobalMemoryStatusEx(out var status);
            info.TotalBytes = (long)status.ullTotalPhys;
            info.AvailableBytes = (long)status.ullAvailPhys;
        }
        catch
        {
            // leave zeros — UI will show "unavailable"
        }

        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Capacity, Speed FROM Win32_PhysicalMemory");
            foreach (var obj in searcher.Get())
            {
                info.ModuleCount++;
                if (obj["Speed"] is not null)
                    info.SpeedMhz = Math.Max(info.SpeedMhz, Convert.ToDouble(obj["Speed"]));
            }
        }
        catch
        {
            // module count / speed stay at defaults
        }

        return info;
    }

    private static MotherboardInfo ReadMotherboard()
    {
        var info = new MotherboardInfo();
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT Manufacturer, Product FROM Win32_BaseBoard");
            foreach (var obj in searcher.Get())
            {
                info.Manufacturer = (obj["Manufacturer"] as string)?.Trim() ?? info.Manufacturer;
                info.Product = (obj["Product"] as string)?.Trim() ?? info.Product;
            }
        }
        catch
        {
            // leave "Unknown"
        }

        return info;
    }

    private static OsInfo ReadOs()
    {
        var info = new OsInfo
        {
            Architecture = RuntimeInformation.OSArchitecture.ToString()
        };

        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            info.ProductName = key?.GetValue("ProductName") as string ?? "Windows";
            var build = key?.GetValue("CurrentBuildNumber") as string ?? "0";
            var ubr = key?.GetValue("UBR");
            info.Build = ubr is not null ? $"{build}.{ubr}" : build;
            info.Version = Environment.OSVersion.Version.ToString();

            if (int.TryParse(build, out var buildNumber))
                info.IsGamingOptimizedOsVersion = buildNumber >= 22000; // Windows 11+ ships HAGS/Auto HDR/DirectStorage improvements
        }
        catch
        {
            info.ProductName = Environment.OSVersion.VersionString;
        }

        return info;
    }

    private static List<StorageDrive> ReadDrives()
    {
        var drives = new List<StorageDrive>();
        var mediaTypeByLetter = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        try
        {
            using var searcher = new ManagementObjectSearcher(@"root\Microsoft\Windows\Storage",
                "SELECT DeviceId, MediaType FROM MSFT_PhysicalDisk");
            foreach (var obj in searcher.Get())
            {
                // MediaType: 3 = HDD, 4 = SSD, 5 = SCM, 0 = Unspecified
                var mediaType = Convert.ToInt32(obj["MediaType"] ?? 0) switch
                {
                    4 => "SSD",
                    3 => "HDD",
                    5 => "Storage Class Memory",
                    _ => "Unknown"
                };
                // Physical disks aren't directly keyed by drive letter; best-effort mapping is done below via partitions.
                mediaTypeByLetter[obj["DeviceId"]?.ToString() ?? string.Empty] = mediaType;
            }
        }
        catch
        {
            // Storage namespace can be unavailable on some systems/VMs — media type will show as Unknown.
        }

        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed || !drive.IsReady) continue;

                drives.Add(new StorageDrive
                {
                    DriveLetter = drive.Name,
                    VolumeLabel = SafeVolumeLabel(drive),
                    TotalBytes = drive.TotalSize,
                    FreeBytes = drive.AvailableFreeSpace,
                    MediaType = "Unknown"
                });
            }
        }
        catch
        {
            // leave drives empty
        }

        TryAssignMediaTypes(drives);
        return drives;
    }

    private static string SafeVolumeLabel(DriveInfo drive)
    {
        try { return string.IsNullOrWhiteSpace(drive.VolumeLabel) ? "Local Disk" : drive.VolumeLabel; }
        catch { return "Local Disk"; }
    }

    private static void TryAssignMediaTypes(List<StorageDrive> drives)
    {
        try
        {
            using var searcher = new ManagementObjectSearcher(@"root\Microsoft\Windows\Storage",
                "SELECT * FROM MSFT_Partition");
            using var diskSearcher = new ManagementObjectSearcher(@"root\Microsoft\Windows\Storage",
                "SELECT DeviceId, MediaType FROM MSFT_PhysicalDisk");

            var diskMedia = new Dictionary<string, string>();
            foreach (var disk in diskSearcher.Get())
            {
                var mediaType = Convert.ToInt32(disk["MediaType"] ?? 0) switch
                {
                    4 => "SSD",
                    3 => "HDD",
                    _ => "Unknown"
                };
                diskMedia[disk["DeviceId"]?.ToString() ?? string.Empty] = mediaType;
            }

            foreach (var partition in searcher.Get())
            {
                var driveLetter = partition["DriveLetter"]?.ToString();
                var diskNumber = partition["DiskNumber"]?.ToString();
                if (string.IsNullOrEmpty(driveLetter) || diskNumber is null) continue;

                var match = drives.FirstOrDefault(d => d.DriveLetter.StartsWith(driveLetter, StringComparison.OrdinalIgnoreCase));
                if (match is not null && diskMedia.TryGetValue(diskNumber, out var mt))
                    match.MediaType = mt;
            }
        }
        catch
        {
            // Leave "Unknown" — some systems (VMs, certain drivers) don't expose MSFT_PhysicalDisk reliably.
        }
    }

    private static bool DetectIsLaptop()
    {
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT * FROM Win32_Battery");
            return searcher.Get().Count > 0;
        }
        catch
        {
            return false;
        }
    }
}
