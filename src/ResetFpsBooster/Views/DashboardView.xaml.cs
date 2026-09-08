using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class DashboardView : UserControl
{
    public DashboardView()
    {
        InitializeComponent();
        Unloaded += (_, _) => (DataContext as DashboardViewModel)?.StopLiveUpdates();
    }
}
