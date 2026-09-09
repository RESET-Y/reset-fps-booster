using System.Windows;
using System.Windows.Media.Animation;
using ResetFpsBooster.Core;
using ResetFpsBooster.Core.Utilities;
using ResetFpsBooster.Services;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster;

public partial class App : Application
{
    private AppServices? _services;
    private bool _isShowingCrashDialog;

    protected override async void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        AppPaths.EnsureFoldersExist();
        ThemeColorHelper.ApplyAccentColor(new SettingsService().Current.AccentColorHex);

#if RFB_BETA
        Resources.MergedDictionaries.Add(new ResourceDictionary
        {
            Source = new Uri("Themes/FrameBoostBetaTemplate.xaml", UriKind.Relative)
        });
#endif

        DispatcherUnhandledException += (_, args) =>
        {
            args.Handled = true;

            // Never show a modal dialog from inside a layout/render callback: if the same error
            // keeps re-triggering on every layout pass, a modal MessageBox would re-enter the
            // dispatcher and retrigger it again from inside itself, growing the stack until it
            // overflows. One dialog per crash burst is enough to inform the user.
            if (_isShowingCrashDialog) return;
            _isShowingCrashDialog = true;
            try
            {
                MessageBox.Show(
                    $"An unexpected error occurred and was ignored:\n\n{args.Exception.Message}",
                    "RESET FPS BOOSTER",
                    MessageBoxButton.OK,
                    MessageBoxImage.Warning);
            }
            finally
            {
                _isShowingCrashDialog = false;
            }
        };

        var splash = new SplashWindow();
        splash.Show();

        await splash.RunIntroAsync();

        ShowMainWindow();
        splash.Close();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        // Restores any deprioritized background processes if the app is closed mid-boost —
        // otherwise they'd stay at BelowNormal priority until their own next restart.
        _services?.GameBoost.Stop();
        base.OnExit(e);
    }

    private void ShowMainWindow()
    {
        _services = new AppServices();
        var mainViewModel = new MainViewModel(_services);

        var window = new MainWindow { DataContext = mainViewModel, Opacity = 0 };
        WindowBackdrop.ApplyDarkModeAndBackdrop(window);
        MainWindow = window;
        window.Show();

        var fadeIn = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(250));
        window.BeginAnimation(UIElement.OpacityProperty, fadeIn);
    }
}
