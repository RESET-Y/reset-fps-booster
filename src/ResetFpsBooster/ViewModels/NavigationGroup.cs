using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Localization;

namespace ResetFpsBooster.ViewModels;

/// <summary>
/// A heading in the sidebar with the pages under it, which can be folded away.
/// The first group has no heading and is always open (the Dashboard stands
/// alone at the top). The id is what is remembered of a folded group.
/// </summary>
public sealed partial class NavigationGroup : ObservableObject
{
    private readonly string? _labelKey;

    public string Id { get; }
    public string Label => _labelKey is null ? "" : Loc.T(_labelKey);
    public bool HasHeader => _labelKey is not null;
    public ObservableCollection<NavigationItem> Items { get; } = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(ShowItems))]
    [NotifyPropertyChangedFor(nameof(IsActiveHeader))]
    private bool _isExpanded = true;

    /// <summary>The pages are shown: always for a group without a heading, otherwise while it is open.</summary>
    public bool ShowItems => !HasHeader || IsExpanded;

    /// <summary>The page being looked at sits in this group.</summary>
    public bool HasSelection => Items.Any(i => i.IsSelected);

    /// <summary>Folded, but the current page is inside: the heading lights up so the page is not lost.</summary>
    public bool IsActiveHeader => HasSelection && !IsExpanded;

    public NavigationGroup(string id, string? labelKey, IEnumerable<NavigationItem> items)
    {
        Id = id;
        _labelKey = labelKey;
        foreach (var item in items) Items.Add(item);
        Loc.LanguageChanged += (_, _) => OnPropertyChanged(nameof(Label));
    }

    public void RefreshSelection()
    {
        OnPropertyChanged(nameof(HasSelection));
        OnPropertyChanged(nameof(IsActiveHeader));
    }
}
