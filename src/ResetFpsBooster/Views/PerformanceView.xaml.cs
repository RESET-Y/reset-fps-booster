using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class PerformanceView : UserControl
{
    public PerformanceView()
    {
        InitializeComponent();
        Unloaded += (_, _) => (DataContext as PerformanceViewModel)?.StopMonitoringCommand.Execute(null);
    }
}
