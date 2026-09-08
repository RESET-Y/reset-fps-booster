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
    public string ExecutableLabel => string.IsNullOrEmpty(Profile.ExecutablePath) ? "Not found — browse manually" : Profile.ExecutablePath;
    public bool IsOptimized => Profile.IsOptimized;
    public string StatusLabel => Profile.IsOptimized
        ? $"Optimized {Profile.LastOptimizedAt:g}"
        : "Not optimized";

    public bool SupportsAutoexec => _gameAutoexecService.SupportsAutoexec(Profile);
    public bool IsAutoexecApplied => Profile.IsAutoexecApplied;
    public string? SteamLaunchOption => _gameAutoexecService.GetSteamLaunchOption(Profile);
    public string AutoexecHeaderLabel => $"{_gameAutoexecService.GetSupportedGameName(Profile)} FPS Autoexec";
    public string AutoexecStatusLabel => Profile.IsAutoexecApplied
        ? $"Autoexec installed {Profile.AutoexecAppliedAt:g}"
        : "Autoexec not installed";

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
