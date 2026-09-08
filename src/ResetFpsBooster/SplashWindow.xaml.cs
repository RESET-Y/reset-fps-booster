using System.IO;
using System.Windows;

namespace ResetFpsBooster;

/// <summary>
/// Plays the app's boot intro video, then hands control back to App.xaml.cs to show the real
/// dashboard. Falls back to a short delay instead of hanging forever if the video file is
/// missing or fails to decode — a broken video must never be able to block the app from starting.
/// </summary>
public partial class SplashWindow : Window
{
    private static readonly TimeSpan FallbackDuration = TimeSpan.FromSeconds(3);

    private readonly TaskCompletionSource _introFinished = new();

    public SplashWindow()
    {
        InitializeComponent();
    }

    public async Task RunIntroAsync()
    {
        var videoPath = Path.Combine(AppContext.BaseDirectory, "Assets", "intro.mp4");

        if (!File.Exists(videoPath))
        {
            await Task.Delay(FallbackDuration);
            return;
        }

        IntroPlayer.Source = new Uri(videoPath, UriKind.Absolute);
        IntroPlayer.Play();

        var completed = await Task.WhenAny(_introFinished.Task, Task.Delay(TimeSpan.FromSeconds(30)));
        _ = completed;
    }

    private void IntroPlayer_MediaEnded(object sender, RoutedEventArgs e) => _introFinished.TrySetResult();

    private void IntroPlayer_MediaFailed(object sender, System.Windows.ExceptionRoutedEventArgs e) => _introFinished.TrySetResult();
}
