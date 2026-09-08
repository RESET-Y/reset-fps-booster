using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class OptimizerViewModel : ViewModelBase
{
    private readonly IOptimizationService _optimizationService;

    [ObservableProperty] private ObservableCollection<OptimizationModuleViewModel> _modules = new();
    [ObservableProperty] private bool _isApplying;
    [ObservableProperty] private string? _summaryMessage;
    [ObservableProperty] private bool _anyResultRequiresReboot;
    [ObservableProperty] private bool _needsAdminPrompt;
    [ObservableProperty] private int _progressCurrent;
    [ObservableProperty] private int _progressTotal;

    public bool IsAdministrator => AdminHelper.IsRunningAsAdministrator();

    public int SelectedCount => Modules.Count(m => m.IsSelected && m.IsAvailable);
    public bool HasIrreversibleSelection => Modules.Any(m => m.IsSelected && m.Module.Id == "storage.temp-cleanup");

    public OptimizerViewModel(IOptimizationService optimizationService)
    {
        _optimizationService = optimizationService;
    }

    [RelayCommand]
    public async Task LoadAsync()
    {
        IsBusy = true;
        try
        {
            var states = await _optimizationService.RefreshStatusesAsync();
            var vms = new ObservableCollection<OptimizationModuleViewModel>();

            foreach (var state in states)
            {
                var vm = new OptimizationModuleViewModel(state.Module);
                vm.ApplyStatus(state.Status);
                vm.PropertyChanged += (_, e) =>
                {
                    if (e.PropertyName == nameof(OptimizationModuleViewModel.IsSelected))
                    {
                        OnPropertyChanged(nameof(SelectedCount));
                        OnPropertyChanged(nameof(HasIrreversibleSelection));
                    }
                };
                vms.Add(vm);
            }

            Modules = vms;
            OnPropertyChanged(nameof(SelectedCount));
        }
        finally
        {
            IsBusy = false;
        }
    }

    [RelayCommand]
    public void SelectRecommended()
    {
        // "Recommended" means low-risk and reversible without a reboot. Medium/Experimental
        // modules (e.g. Hardware-Accelerated GPU Scheduling) have documented compatibility
        // issues on some GPU-driver/anti-cheat combinations and must be opted into explicitly.
        foreach (var module in Modules)
            module.IsSelected = module.IsAvailable && !module.IsApplied
                && module.Risk == RiskLevel.Low
                && module.Module.Id != "storage.temp-cleanup";
    }

    [RelayCommand]
    public void SelectNone()
    {
        foreach (var module in Modules)
            module.IsSelected = false;
    }

    [RelayCommand]
    public async Task ApplySelectedAsync()
    {
        var selected = Modules.Where(m => m.IsSelected && m.IsAvailable).ToList();
        if (selected.Count == 0)
        {
            SummaryMessage = "No optimizations selected.";
            return;
        }

        if (selected.Any(m => m.RequiresAdmin) && !IsAdministrator)
        {
            NeedsAdminPrompt = true;
            return;
        }

        NeedsAdminPrompt = false;
        IsApplying = true;
        AnyResultRequiresReboot = false;
        ProgressCurrent = 0;
        ProgressTotal = selected.Count;

        var succeeded = 0;
        var failed = 0;

        foreach (var moduleVm in selected)
        {
            var result = await _optimizationService.ApplyModuleAsync(moduleVm.Module);
            moduleVm.LastResultMessage = result.Message;
            moduleVm.LastResultSuccess = result.Success;

            if (result.Success) succeeded++; else failed++;
            if (result.RequiresReboot) AnyResultRequiresReboot = true;

            ProgressCurrent++;
        }

        await LoadAsync();

        SummaryMessage = failed == 0
            ? $"Applied {succeeded} optimization(s) successfully."
            : $"Applied {succeeded} optimization(s), {failed} could not be applied (see details below).";

        IsApplying = false;
    }

    [RelayCommand]
    public void RelaunchElevated()
    {
        AdminHelper.RelaunchElevatedAndExit();
    }

    [RelayCommand]
    public async Task ApplySingleAsync(OptimizationModuleViewModel moduleVm)
    {
        if (moduleVm.RequiresAdmin && !IsAdministrator)
        {
            NeedsAdminPrompt = true;
            return;
        }

        IsApplying = true;
        var result = await _optimizationService.ApplyModuleAsync(moduleVm.Module);
        moduleVm.LastResultMessage = result.Message;
        moduleVm.LastResultSuccess = result.Success;
        if (result.RequiresReboot) AnyResultRequiresReboot = true;

        await LoadAsync();
        IsApplying = false;
    }
}
