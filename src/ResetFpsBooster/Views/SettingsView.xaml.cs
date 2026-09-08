using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class SettingsView : UserControl
{
    public SettingsView()
    {
        InitializeComponent();
        // The settings toggles bind straight to the AppSettings model for simplicity;
        // persist whenever the page is left so nothing is lost without an explicit Save button.
        Unloaded += (_, _) => (DataContext as SettingsViewModel)?.SaveCommand.Execute(null);
    }
}
