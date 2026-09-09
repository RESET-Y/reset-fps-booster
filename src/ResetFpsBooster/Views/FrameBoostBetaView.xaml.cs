#if RFB_BETA
using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class FrameBoostBetaView : UserControl
{
    public FrameBoostBetaView()
    {
        InitializeComponent();
        Loaded += (_, _) => (DataContext as FrameBoostBetaViewModel)?.RefreshWindowsCommand.Execute(null);
        Unloaded += (_, _) => (DataContext as FrameBoostBetaViewModel)?.StopCaptureCommand.Execute(null);
    }
}
#endif
