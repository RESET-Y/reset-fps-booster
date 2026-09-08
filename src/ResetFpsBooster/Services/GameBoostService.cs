using System.Diagnostics;
using System.IO;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;

namespace ResetFpsBooster.Services;

public sealed class GameBoostService : IGameBoostService
{
    // Streaming/broadcast and voice-chat software — always left at their normal priority so a
    // stream or call never suffers, regardless of what else this feature is doing.
    private static readonly string[] NeverTouchProcessNames =
    {
        "obs64", "obs32", "obs", "obs-studio",
        "streamlabs", "streamlabsobs", "slobs client",
        "xsplit", "xsplitcore", "xsplitbroadcaster",
        "nvidiabroadcast", "nvidia broadcast",
        "twitchstudio", "vmix", "ffmpeg", "restreamer",
        "wavelink", "camerahub", "streamdeck", "streamdeckcaptureplugin",
        "discord", "discordptb", "discordcanary", "teamspeak3", "teams", "skype",

        // Core OS / shell — never worth the risk for no real benefit.
        "system", "idle", "registry", "smss", "csrss", "wininit", "services", "lsass",
        "winlogon", "memcompression", "dwm", "explorer", "resetfpsbooster", "secure system",
        "svchost", "fontdrvhost", "sihost", "taskhostw", "runtimebroker", "audiodg",
        "spoolsv", "conhost", "ctfmon", "searchindexer", "wudfhost"
    };

    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(3);

    private readonly IGameLibraryService _libraryService;
    private readonly IChangeLogService _changeLog;

    private CancellationTokenSource? _cts;
    private Task? _loopTask;

    private int _boostedGameProcessId = -1;
    private readonly List<(int Pid, ProcessPriorityClass OriginalPriority)> _deprioritized = new();

    public bool IsRunning => _cts is not null;
    public bool IsBoostActive { get; private set; }
    public string? ActiveGameName { get; private set; }

    public GameBoostService(IGameLibraryService libraryService, IChangeLogService changeLog)
    {
        _libraryService = libraryService;
        _changeLog = changeLog;
    }

    public void Start()
    {
        if (IsRunning) return;

        _cts = new CancellationTokenSource();
        _loopTask = Task.Run(() => WatchLoopAsync(_cts.Token));
    }

    public void Stop()
    {
        if (_cts is null) return;

        _cts.Cancel();
        RestoreIfActive("Game Boost Mode disabled");
        _cts = null;
        _loopTask = null;
    }

    private async Task WatchLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                Tick();
            }
            catch
            {
                // Best-effort background watcher — never let a transient failure kill the loop.
            }

            try
            {
                await Task.Delay(PollInterval, ct);
            }
            catch (TaskCanceledException)
            {
                break;
            }
        }
    }

    private void Tick()
    {
        var runningGame = FindRunningGame();

        if (IsBoostActive)
        {
            var stillRunning = runningGame is not null && runningGame.Value.Process.Id == _boostedGameProcessId;
            if (!stillRunning)
            {
                RestoreIfActive($"{ActiveGameName} exited");
            }
            runningGame?.Process.Dispose();
            return;
        }

        if (runningGame is null) return;

        var (process, name) = runningGame.Value;
        using (process)
        {
            ApplyBoost(process, name);
        }
    }

    private (Process Process, string Name)? FindRunningGame()
    {
        var games = _libraryService.GetGames()
            .Where(g => !string.IsNullOrEmpty(g.ExecutablePath))
            .ToList();

        if (games.Count == 0) return null;

        foreach (var process in Process.GetProcesses())
        {
            try
            {
                var match = games.FirstOrDefault(g =>
                    string.Equals(Path.GetFileNameWithoutExtension(g.ExecutablePath), process.ProcessName, StringComparison.OrdinalIgnoreCase));

                if (match is not null)
                    return (process, match.Name);
            }
            catch
            {
                // Inaccessible process — skip.
            }

            process.Dispose();
        }

        return null;
    }

    private void ApplyBoost(Process gameProcess, string gameName)
    {
        var foregroundProcessId = GetForegroundProcessId();
        _deprioritized.Clear();

        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    if (process.Id == gameProcess.Id || process.Id == foregroundProcessId) continue;
                    if (NeverTouchProcessNames.Contains(process.ProcessName, StringComparer.OrdinalIgnoreCase)) continue;

                    var current = process.PriorityClass;
                    if (current is ProcessPriorityClass.Idle or ProcessPriorityClass.BelowNormal) continue;

                    _deprioritized.Add((process.Id, current));
                    process.PriorityClass = ProcessPriorityClass.BelowNormal;
                }
                catch
                {
                    // Protected/system-owned process, or it exited mid-scan — skip.
                }
            }
        }

        try
        {
            gameProcess.PriorityClass = ProcessPriorityClass.AboveNormal;
        }
        catch
        {
            // Not critical — the deprioritized background processes already give it the room it needs.
        }

        _boostedGameProcessId = gameProcess.Id;
        IsBoostActive = true;
        ActiveGameName = gameName;

        _changeLog.Append(new List<ChangeLogEntry>
        {
            new()
            {
                ModuleName = "Game Boost Mode",
                SettingName = $"Detected {gameName}",
                OldValue = "Normal priority",
                NewValue = $"{_deprioritized.Count} background process(es) deprioritized"
            }
        });
    }

    private void RestoreIfActive(string reason)
    {
        if (!IsBoostActive) return;

        var restored = 0;
        foreach (var (pid, originalPriority) in _deprioritized)
        {
            try
            {
                using var process = Process.GetProcessById(pid);
                process.PriorityClass = originalPriority;
                restored++;
            }
            catch
            {
                // Process already exited — nothing to restore.
            }
        }

        _changeLog.Append(new List<ChangeLogEntry>
        {
            new()
            {
                ModuleName = "Game Boost Mode",
                SettingName = reason,
                OldValue = $"{_deprioritized.Count} process(es) deprioritized",
                NewValue = $"{restored} restored to normal priority"
            }
        });

        _deprioritized.Clear();
        _boostedGameProcessId = -1;
        IsBoostActive = false;
        ActiveGameName = null;
    }

    private static int GetForegroundProcessId()
    {
        try
        {
            var hwnd = NativeMethods.GetForegroundWindow();
            if (hwnd == IntPtr.Zero) return -1;
            NativeMethods.GetWindowThreadProcessId(hwnd, out var pid);
            return (int)pid;
        }
        catch
        {
            return -1;
        }
    }
}
