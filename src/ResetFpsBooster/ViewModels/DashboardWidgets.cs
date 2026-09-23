using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ResetFpsBooster.Core.Localization;

namespace ResetFpsBooster.ViewModels;

/// One block on the dashboard that the user can hide and bring back.
public sealed partial class DashboardWidget : ObservableObject
{
    public DashboardWidget(string id, string labelKey) { Id = id; LabelKey = labelKey; }

    public string Id { get; }
    public string LabelKey { get; }
    public string Label => Loc.T(LabelKey);

    [ObservableProperty] private bool _isVisible = true;

    public void Relabel() => OnPropertyChanged(nameof(Label));
}

/// Every dashboard widget, looked up by id so the view can bind
/// Widgets[score].IsVisible. The order here is the order in "Add widgets".
public sealed class DashboardWidgetSet
{
    private readonly Dictionary<string, DashboardWidget> _byId;

    public DashboardWidgetSet()
    {
        All = new[]
        {
            new DashboardWidget("hardware", "Widget.Hardware"),
            new DashboardWidget("score", "Widget.Score"),
            new DashboardWidget("boost", "Widget.Boost"),
            new DashboardWidget("scan", "Widget.Scan"),
            new DashboardWidget("optimizations", "Widget.Optimizations"),
            new DashboardWidget("changes", "Widget.Changes"),
            new DashboardWidget("sysinfo", "Widget.SystemInfo"),
            new DashboardWidget("driver", "Widget.Driver"),
            new DashboardWidget("powerplan", "Widget.PowerPlan"),
            new DashboardWidget("gamemode", "Widget.GameMode"),
            new DashboardWidget("games", "Widget.Games"),
        };
        _byId = All.ToDictionary(w => w.Id);
    }

    public IReadOnlyList<DashboardWidget> All { get; }

    public DashboardWidget this[string id] => _byId[id];

    public bool TryGet(string id, out DashboardWidget widget) => _byId.TryGetValue(id, out widget!);
}
