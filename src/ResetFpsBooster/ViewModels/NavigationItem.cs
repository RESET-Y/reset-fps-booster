using CommunityToolkit.Mvvm.ComponentModel;

namespace ResetFpsBooster.ViewModels;

public sealed partial class NavigationItem : ObservableObject
{
    public NavigationSection Section { get; }
    public string Label { get; }
    public IconKind Icon { get; }

    [ObservableProperty] private bool _isSelected;

    public NavigationItem(NavigationSection section, string label, IconKind icon)
    {
        Section = section;
        Label = label;
        Icon = icon;
    }
}
