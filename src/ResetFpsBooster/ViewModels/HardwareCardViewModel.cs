using CommunityToolkit.Mvvm.ComponentModel;

namespace ResetFpsBooster.ViewModels;

public sealed partial class HardwareCardViewModel : ObservableObject
{
    [ObservableProperty] private string _title = string.Empty;
    [ObservableProperty] private string _subtitle = string.Empty;
    [ObservableProperty] private double _usagePercent;
    [ObservableProperty] private string _statLineOne = string.Empty;
    [ObservableProperty] private string _statLineTwo = string.Empty;
    [ObservableProperty] private string? _temperatureLabel;
}
