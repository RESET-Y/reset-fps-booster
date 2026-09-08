using System.Collections.ObjectModel;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;

namespace ResetFpsBooster.ViewModels;

public sealed partial class SettingsViewModel : ViewModelBase
{
    private readonly ISettingsService _settingsService;
    private readonly IUpdateService _updateService;

    public AppSettings Settings => _settingsService.Current;

    public bool IsAdministrator => AdminHelper.IsRunningAsAdministrator();

    public const string AppVersion = Core.Utilities.AppVersionInfo.Current;
    public const string AuthorName = "Lukas Reschke";

    [ObservableProperty] private bool _startWithWindows;
    [ObservableProperty] private bool _startWithWindowsAsAdmin;
    [ObservableProperty] private string? _startupModeMessage;
    [ObservableProperty] private string _updateRepositoryInput;

    [ObservableProperty] private string _selectedAccentColorHex;
    [ObservableProperty] private bool _accentColorChanged;

    public ObservableCollection<AccentColorOption> AccentPresets { get; } =
        new(ThemeColorHelper.Presets.Select(p => new AccentColorOption(p.Name, p.Hex)));

    [ObservableProperty] private bool _isCheckingForUpdate;
    [ObservableProperty] private bool _isDownloadingUpdate;
    [ObservableProperty] private double _downloadProgressPercent;
    [ObservableProperty] private string? _updateStatusMessage;
    [ObservableProperty] private bool _isUpdateAvailable;
    [ObservableProperty] private string? _latestVersionText;
    [ObservableProperty] private string? _releaseNotes;
    [ObservableProperty] private string? _pendingInstallerPath;
    [ObservableProperty] private string? _pendingDownloadUrl;

    public SettingsViewModel(ISettingsService settingsService, IUpdateService updateService)
    {
        _settingsService = settingsService;
        _updateService = updateService;
        _startWithWindows = Settings.StartWithWindows;
        _startWithWindowsAsAdmin = Settings.StartWithWindowsAsAdmin;
        _updateRepositoryInput = Settings.UpdateRepository;
        _selectedAccentColorHex = Settings.AccentColorHex;

        // A silent check already ran at app startup (MainViewModel) — reuse that result instead of
        // forcing the user to click "Check for Updates" again just to see what it already found.
        if (_updateService.LastResult is { } cached)
            ApplyResult(cached);
    }

    partial void OnStartWithWindowsChanged(bool value)
    {
        _settingsService.ApplyStartWithWindows(value);
        if (value) StartWithWindowsAsAdmin = false;
    }

    partial void OnStartWithWindowsAsAdminChanged(bool value)
    {
        var (success, message) = _settingsService.ApplyStartWithWindowsAsAdmin(value);
        StartupModeMessage = message;

        if (!success)
        {
            // Revert the toggle without re-triggering this handler.
            _startWithWindowsAsAdmin = !value;
            OnPropertyChanged(nameof(StartWithWindowsAsAdmin));
            return;
        }

        if (value) StartWithWindows = false;
    }

    [RelayCommand]
    public void SelectAccentColor(string hex)
    {
        SelectedAccentColorHex = hex;
        Settings.AccentColorHex = hex;
        _settingsService.Save();
        AccentColorChanged = true;
    }

    [RelayCommand]
    public void RestartApp()
    {
        var exePath = System.Diagnostics.Process.GetCurrentProcess().MainModule?.FileName;
        if (string.IsNullOrEmpty(exePath)) return;

        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(exePath) { UseShellExecute = true });
        Environment.Exit(0);
    }

    public bool HasDownloadUrl => !string.IsNullOrEmpty(PendingDownloadUrl);

    partial void OnPendingDownloadUrlChanged(string? value) => OnPropertyChanged(nameof(HasDownloadUrl));

    partial void OnUpdateRepositoryInputChanged(string value)
    {
        Settings.UpdateRepository = value.Trim();
        _settingsService.Save();
    }

    [RelayCommand]
    public void SaveToggle(string propertyName)
    {
        _settingsService.Save();
        _ = propertyName;
    }

    [RelayCommand]
    public void Save()
    {
        _settingsService.Save();
    }

    [RelayCommand]
    public void RelaunchElevated()
    {
        AdminHelper.RelaunchElevatedAndExit();
    }

    [RelayCommand]
    public async Task CheckForUpdatesAsync()
    {
        IsCheckingForUpdate = true;
        IsUpdateAvailable = false;
        UpdateStatusMessage = null;

        try
        {
            var result = await _updateService.CheckForUpdateAsync();
            ApplyResult(result);
        }
        finally
        {
            IsCheckingForUpdate = false;
        }
    }

    private void ApplyResult(UpdateCheckResult result)
    {
        if (!result.Success)
        {
            UpdateStatusMessage = result.ErrorMessage;
            return;
        }

        if (result.IsUpdateAvailable)
        {
            IsUpdateAvailable = true;
            LatestVersionText = result.LatestVersion;
            ReleaseNotes = string.IsNullOrWhiteSpace(result.ReleaseNotes) ? null : result.ReleaseNotes;
            PendingDownloadUrl = result.DownloadUrl;
            UpdateStatusMessage = string.IsNullOrEmpty(result.DownloadUrl)
                ? $"Version {result.LatestVersion} is available (you have {result.CurrentVersion}), but this release has no installer (.exe) attached."
                : $"Version {result.LatestVersion} is available (you have {result.CurrentVersion}).";
        }
        else
        {
            UpdateStatusMessage = $"You're up to date (version {result.CurrentVersion}).";
        }
    }

    [RelayCommand]
    public async Task DownloadAndInstallUpdateAsync()
    {
        if (string.IsNullOrEmpty(PendingDownloadUrl))
        {
            UpdateStatusMessage = "This release has no downloadable installer attached.";
            return;
        }

        IsDownloadingUpdate = true;
        DownloadProgressPercent = 0;

        try
        {
            var progress = new Progress<UpdateDownloadProgress>(p =>
            {
                DownloadProgressPercent = p.PercentComplete ?? 0;
            });

            var installerPath = await _updateService.DownloadUpdateAsync(PendingDownloadUrl, progress);
            PendingInstallerPath = installerPath;
            UpdateStatusMessage = "Download complete. Click \"Install & Restart\" to finish.";
        }
        catch (Exception ex)
        {
            UpdateStatusMessage = $"Download failed: {ex.Message}";
        }
        finally
        {
            IsDownloadingUpdate = false;
        }
    }

    [RelayCommand]
    public void InstallDownloadedUpdate()
    {
        if (string.IsNullOrEmpty(PendingInstallerPath) || !File.Exists(PendingInstallerPath))
        {
            UpdateStatusMessage = "The downloaded installer could not be found. Try downloading again.";
            return;
        }

        _updateService.LaunchInstallerAndExit(PendingInstallerPath);
    }
}
