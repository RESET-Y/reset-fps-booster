using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Localization;

namespace ResetFpsBooster.ViewModels;

public sealed partial class NavigationItem : ObservableObject
{
    public NavigationSection Section { get; }
    /// Looked up on every read, and re-announced when the language changes,
    /// so the sidebar relabels itself the instant a new language is picked.
    private readonly string _labelKey;
    public string Label => Loc.T(_labelKey);
    public IconKind Icon { get; }

    [ObservableProperty] private bool _isSelected;

    public NavigationItem(NavigationSection section, string labelKey, IconKind icon)
    {
        Section = section;
        _labelKey = labelKey;
        Icon = icon;
        Loc.LanguageChanged += (_, _) => OnPropertyChanged(nameof(Label));
    }
}
