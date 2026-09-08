using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface ISystemScanService
{
    Task<List<ScanStep>> RunScanAsync(IProgress<ScanStep>? progress, CancellationToken ct = default);
}
