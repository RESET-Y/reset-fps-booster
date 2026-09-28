#if RFB_BETA
using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class FrameBoostBetaView : UserControl
{
    // Deliberately no Unloaded handling. This view used to stop FrameBoost
    // when it was unloaded, which meant navigating to any other page switched
    // the boost off underneath the user - the opposite of what a switch is
    // for. It now stays on until it is turned off, or until the app exits
    // (App.OnExit shuts the engine down).
    //
    // Loaded only re-reads the window list, so a game started while another
    // page was open is already in it when this page comes back.
    public FrameBoostBetaView()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            if (DataContext is FrameBoostBetaViewModel { IsRunning: false } vm)
                vm.RefreshGameWindowsCommand.Execute(null);
        };
    }
}
#endif
