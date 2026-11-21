using System.Diagnostics;
using System.Windows.Threading;
using ResetFpsBooster.Core.Picture;
using ResetFpsBooster.Games;

namespace ResetFpsBooster.Services;

/// <summary>
/// KEEPS THE PICTURE IN LINE WITH THE GAMES. Every few seconds it looks for a game from the library
/// that is running (apart from the programs on the ignore list). With auto-off, the picture goes to
/// Windows' own while a game runs and comes back when the game stops. Nothing is read from the game.
/// </summary>
public sealed class PictureWatcher : IDisposable
{
    private readonly IPictureService _picture;
    private readonly IGameLibraryService _library;
    private readonly ISettingsService _settings;
    private readonly DispatcherTimer _timer;

    public PictureWatcher(IPictureService picture, IGameLibraryService library, ISettingsService settings)
    {
        _picture = picture;
        _library = library;
        _settings = settings;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(3) };
        _timer.Tick += (_, _) => Tick();
    }

    /// <summary>The library game that pauses the picture right now, or null.</summary>
    public string? PausingGame { get; private set; }

    /// <summary>Raised after every check, so the page can show what is happening.</summary>
    public event EventHandler? Changed;

    public void Start()
    {
        _timer.Start();
        Tick();
    }

    public void Stop() => _timer.Stop();

    public void Dispose() => _timer.Stop();

    private void Tick()
    {
        PausingGame = FindRunningGame();

        var s = _settings.Current;
        var current = s.PictureCurrent;
        var wanted = !PictureMath.IsNeutral(current.Saturation, current.Brightness, current.Contrast);

        var action = PictureRules.Decide(s.PictureAutoOff, PausingGame is not null, wanted, _picture.IsApplied);
        if (action == PictureAction.Apply) _picture.Apply(current);
        else if (action == PictureAction.Restore) _picture.Restore();

        Changed?.Invoke(this, EventArgs.Empty);
    }

    private string? FindRunningGame()
    {
        var ignored = _settings.Current.PictureIgnoredGames;
        var games = _library.GetGames()
            .Where(g => !string.IsNullOrEmpty(g.ExecutablePath) && !ignored.Contains(g.Name, StringComparer.OrdinalIgnoreCase))
            .ToList();
        if (games.Count == 0) return null;

        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    var match = games.FirstOrDefault(g => GameExecutable.IsSameProcess(g.ExecutablePath, process.ProcessName));
                    if (match is not null) return match.Name;
                }
                catch
                {
                    // Inaccessible process: skip it.
                }
            }
        }
        return null;
    }
}
