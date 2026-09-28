using System.Windows;
using ResetFpsBooster.Core.Localization;
using ResetFpsBooster.Core.Models;
using ResetFpsBooster.Services;

namespace ResetFpsBooster;

/// THE "NEW VERSION" POP-UP, shown once per start when the silent update check
/// finds a newer release with an installer attached. "Later" just closes it;
/// Settings still has the update for anyone who changes their mind.
public partial class UpdateWindow : Window
{
    private readonly IUpdateService _update;
    private readonly UpdateCheckResult _result;

    public UpdateWindow(IUpdateService update, UpdateCheckResult result)
    {
        InitializeComponent();
        _update = update;
        _result = result;

        NewVersion.Text = "v" + result.LatestVersion;
        CurrentVersion.Text = Loc.F("Update.YouHave", result.CurrentVersion);
        if (string.IsNullOrWhiteSpace(result.ReleaseNotes)) NotesPanel.Visibility = Visibility.Collapsed;
        else Notes.Text = result.ReleaseNotes.Trim();
    }

    private async void OnInstall(object sender, RoutedEventArgs e)
    {
        InstallButton.IsEnabled = false;
        LaterButton.IsEnabled = false;
        ErrorText.Visibility = Visibility.Collapsed;
        ProgressPanel.Visibility = Visibility.Visible;
        ProgressText.Text = Loc.F("Update.Downloading", 0);

        try
        {
            var progress = new Progress<UpdateDownloadProgress>(p =>
            {
                var percent = p.PercentComplete ?? 0;
                Progress.Value = percent;
                ProgressText.Text = Loc.F("Update.Downloading", (int)percent);
            });
            var installer = await _update.DownloadUpdateAsync(_result.DownloadUrl!, progress);
            ProgressText.Text = Loc.T("Update.Starting");
            _update.LaunchInstallerAndExit(installer);
        }
        catch (Exception ex)
        {
            ProgressPanel.Visibility = Visibility.Collapsed;
            ErrorText.Text = Loc.F("Update.Failed", ex.Message);
            ErrorText.Visibility = Visibility.Visible;
            InstallButton.IsEnabled = true;
            LaterButton.IsEnabled = true;
        }
    }

    private void OnLater(object sender, RoutedEventArgs e) => Close();
}
