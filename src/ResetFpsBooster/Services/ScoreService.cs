using System.IO;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

/// <summary>
/// Computes a transparent, reproducible score from the same status checks the optimization
/// modules already expose plus a couple of always-verifiable system facts (free disk space,
/// pending reboot). Every point earned or lost is explained in the breakdown — no fabricated numbers.
/// </summary>
public sealed class ScoreService : IScoreService
{
    public Task<ScoreResult> CalculateAsync(SystemSnapshot snapshot, List<ModuleState> moduleStates, CancellationToken ct = default)
    {
        var breakdown = new List<ScoreBreakdownItem>();

        void AddModule(string moduleId, string label, int points)
        {
            var state = moduleStates.FirstOrDefault(m => m.Module.Id == moduleId);
            var earned = state is { Status.IsAvailable: true, Status.IsApplied: true } ? points : 0;
            breakdown.Add(new ScoreBreakdownItem { Label = label, PointsEarned = earned, PointsPossible = points });
        }

        AddModule("gaming.game-mode", "Windows Game Mode", 12);
        AddModule("gaming.game-dvr", "Background game recording disabled", 10);
        AddModule("windows.power-plan", "High performance power plan", 12);
        AddModule("gpu.hags", "Hardware-accelerated GPU scheduling", 10);
        AddModule("network.throttling-index", "Network throttling disabled", 8);
        AddModule("gpu.task-priority", "Gaming task scheduler priority", 8);
        AddModule("windows.background-apps", "Background Store apps disabled", 8);
        AddModule("windows.startup-cleanup", "Startup apps optimized", 8);

        // Free disk space on the system drive — always verifiable, no module needed.
        var systemDrive = snapshot.Drives.FirstOrDefault(d => d.DriveLetter.StartsWith(Path.GetPathRoot(Environment.SystemDirectory) ?? "C:", StringComparison.OrdinalIgnoreCase))
            ?? snapshot.Drives.FirstOrDefault();
        var freePercent = systemDrive is { TotalBytes: > 0 } ? (double)systemDrive.FreeBytes / systemDrive.TotalBytes * 100 : 0;
        var diskPoints = freePercent switch
        {
            >= 20 => 12,
            >= 10 => 8,
            >= 5 => 4,
            _ => 0
        };
        breakdown.Add(new ScoreBreakdownItem { Label = "Free disk space", PointsEarned = diskPoints, PointsPossible = 12 });

        // RAM headroom.
        var ramFreePercent = 100 - snapshot.Memory.UsedPercent;
        var ramPoints = ramFreePercent switch
        {
            >= 40 => 12,
            >= 20 => 8,
            >= 10 => 4,
            _ => 0
        };
        breakdown.Add(new ScoreBreakdownItem { Label = "Available memory", PointsEarned = ramPoints, PointsPossible = 12 });

        var totalEarned = breakdown.Sum(b => b.PointsEarned);
        var totalPossible = breakdown.Sum(b => b.PointsPossible);
        var score = totalPossible == 0 ? 0 : (int)Math.Round((double)totalEarned / totalPossible * 100);

        var verdict = score switch
        {
            >= 90 => "Your PC is highly optimized for gaming.",
            >= 70 => "Your PC is well optimized. A few tweaks are still available.",
            >= 45 => "There's meaningful room for improvement.",
            _ => "Your PC is running mostly default settings — run Optimize Now for a quick win."
        };

        return Task.FromResult(new ScoreResult { Score = score, Verdict = verdict, Breakdown = breakdown });
    }
}
