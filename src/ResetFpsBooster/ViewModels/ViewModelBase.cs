using CommunityToolkit.Mvvm.ComponentModel;

namespace ResetFpsBooster.ViewModels;

public abstract partial class ViewModelBase : ObservableObject
{
    [ObservableProperty]
    private bool _isBusy;

    [ObservableProperty]
    private string? _statusMessage;
}
