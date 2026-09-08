using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Optimization;

namespace ResetFpsBooster.ViewModels;

public sealed partial class OptimizationModuleViewModel : ObservableObject
{
    public IOptimizationModule Module { get; }

    [ObservableProperty] private bool _isSelected;
    [ObservableProperty] private bool _isAvailable = true;
    [ObservableProperty] private bool _isApplied;
    [ObservableProperty] private string _detailText = string.Empty;
    [ObservableProperty] private string? _unavailableReason;
    [ObservableProperty] private string? _lastResultMessage;
    [ObservableProperty] private bool? _lastResultSuccess;

    public string Name => Module.Name;
    public string Description => Module.Description;
    public RiskLevel Risk => Module.Risk;
    public OptimizationCategory Category => Module.Category;
    public bool RequiresAdmin => Module.RequiresAdmin;
    public bool RequiresReboot => Module.RequiresReboot;

    public OptimizationModuleViewModel(IOptimizationModule module)
    {
        Module = module;
    }

    public void ApplyStatus(OptimizationStatus status)
    {
        IsAvailable = status.IsAvailable;
        IsApplied = status.IsApplied;
        DetailText = status.DetailText;
        UnavailableReason = status.UnavailableReason;

        // Only pre-check Low-risk modules. Medium/Experimental modules (e.g. HAGS, which has
        // documented freeze/crash reports on some GPU-driver + anti-cheat combinations) require
        // the user to deliberately opt in rather than getting applied as part of a default sweep.
        if (status.IsAvailable && !status.IsApplied && Module.Risk == RiskLevel.Low)
            IsSelected = true;
    }
}
