using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.ViewModels;

public sealed partial class GameProfileItemViewModel : ObservableObject
{
    public GameProfile Profile { get; }

    [ObservableProperty] private string? _lastActionMessage;

    public string Name => Profile.Name;
    public string SourceLabel => Profile.Source.ToString();
    public string InstallPath => Profile.InstallPath;
    public string ExecutableLabel => string.IsNullOrEmpty(Profile.ExecutablePath) ? "Not found — browse manually" : Profile.ExecutablePath;
    public bool IsOptimized => Profile.IsOptimized;
    public string StatusLabel => Profile.IsOptimized
        ? $"Optimized {Profile.LastOptimizedAt:g}"
        : "Not optimized";

    public GameProfileItemViewModel(GameProfile profile)
    {
        Profile = profile;
    }

    public void Refresh()
    {
        OnPropertyChanged(nameof(IsOptimized));
        OnPropertyChanged(nameof(StatusLabel));
        OnPropertyChanged(nameof(ExecutableLabel));
    }
}
