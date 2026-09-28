using ResetFpsBooster.Core.Localization;
using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class GameProfileItemViewModel : ObservableObject
{
    private readonly IGameAutoexecService _gameAutoexecService;

    public GameProfile Profile { get; }

    [ObservableProperty] private string? _lastActionMessage;
    [ObservableProperty] private string? _autoexecActionMessage;

    public string Name => Profile.Name;
    public string SourceLabel => Profile.Source.ToString();
    public string InstallPath => Profile.InstallPath;
    public string ExecutableLabel => string.IsNullOrEmpty(Profile.ExecutablePath) ? Loc.T("Gp.ExeNotFound") : Profile.ExecutablePath;
    public bool IsOptimized => Profile.IsOptimized;
    public string StatusLabel => Profile.IsOptimized
        ? Loc.F("Gp.OptimizedAt", Profile.LastOptimizedAt!)
        : Loc.T("Gp.NotOptimized");

    public bool SupportsAutoexec => _gameAutoexecService.SupportsAutoexec(Profile);
    public bool IsAutoexecApplied => Profile.IsAutoexecApplied;
    public string? SteamLaunchOption => _gameAutoexecService.GetSteamLaunchOption(Profile);
    public string AutoexecHeaderLabel => $"{_gameAutoexecService.GetSupportedGameName(Profile)} FPS Autoexec";
    public string AutoexecStatusLabel => Profile.IsAutoexecApplied
        ? Loc.F("Gp.AutoexecAt", Profile.AutoexecAppliedAt!)
        : Loc.T("Gp.AutoexecNone");

    public GameProfileItemViewModel(GameProfile profile, IGameAutoexecService gameAutoexecService)
    {
        Profile = profile;
        _gameAutoexecService = gameAutoexecService;
    }

    public void Refresh()
    {
        OnPropertyChanged(nameof(IsOptimized));
        OnPropertyChanged(nameof(StatusLabel));
        OnPropertyChanged(nameof(ExecutableLabel));
        OnPropertyChanged(nameof(SupportsAutoexec));
        OnPropertyChanged(nameof(IsAutoexecApplied));
        OnPropertyChanged(nameof(AutoexecHeaderLabel));
        OnPropertyChanged(nameof(AutoexecStatusLabel));
    }
}
