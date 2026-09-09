using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class BottleneckEngineView : UserControl
{
    public BottleneckEngineView()
    {
        InitializeComponent();
        Unloaded += (_, _) => (DataContext as BottleneckEngineViewModel)?.StopMonitoringCommand.Execute(null);
    }
}
