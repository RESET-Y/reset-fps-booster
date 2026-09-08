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
    [ObservableProperty] private string _updateRepositoryInput;

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
        _updateRepositoryInput = Settings.UpdateRepository;
    }

    partial void OnStartWithWindowsChanged(bool value)
    {
        _settingsService.ApplyStartWithWindows(value);
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
        finally
        {
            IsCheckingForUpdate = false;
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
