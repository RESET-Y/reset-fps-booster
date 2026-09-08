using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IHardwareService
{
    Task<SystemSnapshot> GetSnapshotAsync(CancellationToken ct = default);
}
