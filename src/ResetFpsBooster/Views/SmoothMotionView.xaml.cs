#if RFB_BETA
using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class SmoothMotionView : UserControl
{
    // Monitors are read again whenever the page is shown - one may have been
    // plugged in or switched off meanwhile.
    public SmoothMotionView()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            if (DataContext is SmoothMotionViewModel { IsOn: false } vm)
                vm.ReadScreens();
        };
    }
}
#endif
