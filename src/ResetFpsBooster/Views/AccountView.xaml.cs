using System.Windows.Controls;
using ResetFpsBooster.ViewModels;

namespace ResetFpsBooster.Views;

public partial class AccountView : UserControl
{
    public AccountView()
    {
        InitializeComponent();

        // PasswordBox.Password is deliberately not bindable in WPF, so the view
        // hands the view model a way to read it at the moment of the call - and
        // clears the box right after, so the password does not linger on screen.
        DataContextChanged += (_, _) =>
        {
            if (DataContext is AccountViewModel vm)
                vm.ReadPassword = () =>
                {
                    var pw = PasswordInput.Password;
                    PasswordInput.Clear();
                    return pw;
                };
        };
    }
}
