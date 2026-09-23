using System.Collections.ObjectModel;
using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

/// THE MANAGER PAGE: create codes that give premium for free, and see them.
///
/// This page only appears in the sidebar for a manager, but hiding it is not
/// the protection. Every action here is a database function that checks
/// is_manager() for the caller before doing anything; a non-manager who got
/// here anyway would get refusals and an empty list.
public sealed partial class ManagerViewModel : ViewModelBase
{
    private readonly IAuthService _auth;

    public sealed record DurationChoice(string LabelKey, int? Days)
    {
        public string Label => Loc.T(LabelKey);
    }

    public IReadOnlyList<DurationChoice> Durations { get; } = new[]
    {
        new DurationChoice("Manager.Days7", 7),
        new DurationChoice("Manager.Days30", 30),
        new DurationChoice("Manager.Days90", 90),
        new DurationChoice("Manager.Forever", null),
    };

    [ObservableProperty] private DurationChoice _selectedDuration;
    [ObservableProperty] private int _maxUses = 1;
    [ObservableProperty] private string _note = "";
    [ObservableProperty] private string? _lastCreatedCode;
    [ObservableProperty] private string? _message;
    [ObservableProperty] private bool _isBusy;

    public ObservableCollection<CodeRow> Codes { get; } = new();
    public bool HasNoCodes => Codes.Count == 0;

    public ManagerViewModel(IAuthService auth)
    {
        _auth = auth;
        _selectedDuration = Durations[1];   // 30 days - the usual gift
        Loc.LanguageChanged += (_, _) =>
        {
            OnPropertyChanged(nameof(Durations));
            foreach (var row in Codes) row.Relabel();
        };
        _ = RefreshAsync();
    }

    [RelayCommand]
    private async Task CreateAsync()
    {
        if (MaxUses < 1) MaxUses = 1;
        IsBusy = true;
        Message = null;
        try
        {
            var code = await _auth.CreateCodeAsync(SelectedDuration.Days, MaxUses, Note);
            if (code is null)
            {
                Message = Loc.T("Manager.CreateFailed");
                return;
            }
            LastCreatedCode = code;
            Note = "";
            TryCopy(code);
            await RefreshAsync();
        }
        finally { IsBusy = false; }
    }

    [RelayCommand]
    private async Task RefreshAsync()
    {
        var list = await _auth.ListCodesAsync();
        Codes.Clear();
        foreach (var c in list) Codes.Add(new CodeRow(c, this));
        OnPropertyChanged(nameof(HasNoCodes));
    }

    internal async Task ToggleAsync(CodeRow row)
    {
        if (await _auth.SetCodeActiveAsync(row.Code, !row.Active)) await RefreshAsync();
    }

    internal static void TryCopy(string text)
    {
        try { Clipboard.SetText(text); } catch { /* clipboard busy - the code is still on screen */ }
    }

    /// One line in the list, with its own copy and on/off actions.
    public sealed partial class CodeRow : ObservableObject
    {
        private readonly PremiumCode _c;
        private readonly ManagerViewModel _owner;

        public CodeRow(PremiumCode c, ManagerViewModel owner) { _c = c; _owner = owner; }

        public string Code => _c.Code;
        public bool Active => _c.Active;
        public string? Note => _c.Note;
        public string Duration => _c.DurationDays is { } d ? Loc.F("Manager.DaysFmt", d) : Loc.T("Manager.Forever");
        public string Usage => Loc.F("Manager.UsesFmt", _c.Uses, _c.MaxUses)
                               + (_c.Active ? "" : " · " + Loc.T("Manager.Inactive"));
        public string ToggleLabel => Loc.T(_c.Active ? "Manager.Deactivate" : "Manager.Activate");
        public string Created => _c.CreatedAt.LocalDateTime.ToString("g");

        public void Relabel()
        {
            OnPropertyChanged(nameof(Duration));
            OnPropertyChanged(nameof(Usage));
            OnPropertyChanged(nameof(ToggleLabel));
        }

        [RelayCommand] private void Copy() => TryCopy(Code);
        [RelayCommand] private Task Toggle() => _owner.ToggleAsync(this);
    }
}
