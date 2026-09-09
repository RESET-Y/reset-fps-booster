using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public interface IBottleneckEngineService : IDisposable
{
    IReadOnlyList<BottleneckHistoryEntry> History { get; }

    /// <summary>Samples every telemetry source once and returns a fresh diagnosis. Cheap enough to
    /// call on a multi-second interval; never spawns a tight polling loop internally.</summary>
    BottleneckReport Sample();
}
