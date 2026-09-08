namespace ResetFpsBooster.Services;

/// <summary>
/// Watches for any detected game launching and, for as long as it runs, lowers the CPU-scheduling
/// priority of other background processes so the game gets preferential treatment — nothing is
/// closed, paused, or loses data, only deprioritized, and every process is restored to its
/// original priority the moment the game exits (or the app shuts down). Streaming/broadcast and
/// voice-chat software is explicitly never touched, so a stream or call stays smooth throughout.
/// </summary>
public interface IGameBoostService
{
    bool IsRunning { get; }
    bool IsBoostActive { get; }
    string? ActiveGameName { get; }

    void Start();
    void Stop();
}
