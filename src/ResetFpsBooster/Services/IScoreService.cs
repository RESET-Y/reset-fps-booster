using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public sealed class ScoreBreakdownItem
{
    public string Label { get; set; } = string.Empty;
    public int PointsEarned { get; set; }
    public int PointsPossible { get; set; }
}

public sealed class ScoreResult
{
    public int Score { get; set; }
    public string Verdict { get; set; } = string.Empty;
    public List<ScoreBreakdownItem> Breakdown { get; set; } = new();
}

public interface IScoreService
{
    Task<ScoreResult> CalculateAsync(SystemSnapshot snapshot, List<ModuleState> moduleStates, CancellationToken ct = default);
}
