#if RFB_BETA
using System.Windows.Controls;

namespace ResetFpsBooster.Views;

public partial class FrameBoostBetaView : UserControl
{
    // Deliberately no Loaded/Unloaded handling. This view used to stop
    // FrameBoost when it was unloaded, which meant navigating to any other
    // page switched the boost off underneath the user - the opposite of what
    // a switch is for. It now stays on until it is turned off, or until the
    // app exits (App.OnExit shuts the engine down).
    public FrameBoostBetaView()
    {
        InitializeComponent();
    }
}
#endif
