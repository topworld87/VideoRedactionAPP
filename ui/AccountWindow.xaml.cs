using System.Windows;
using System.Windows.Input;

namespace VideoBlackout.Wpf;

public partial class AccountWindow : Window
{
    private readonly LicenseClient _license;
    private CancellationTokenSource? _signIn;

    public AccountWindow(LicenseClient license)
    {
        _license = license;
        InitializeComponent();
        HwidBox.Text = string.IsNullOrWhiteSpace(license.Hwid) ? L.T("accountUnavailable") : license.Hwid;
        Closed += (_, _) => _signIn?.Cancel();
        Refresh();
    }

    private void Refresh()
    {
        var plan = _license.Current;
        PlanText.Text = plan.LocalizedName;
        EmailText.Text = !string.IsNullOrWhiteSpace(plan.Email)
            ? plan.Email
            : plan.IsSignedIn ? L.T("accountChipUser") : L.T("accountGuest");
        SummaryText.Text = plan.Summary;
        if (plan.IsPaid && plan.ValidUntil.HasValue)
        {
            UntilText.Visibility = Visibility.Visible;
            UntilText.Text = L.F("accountUntil", plan.ValidUntil.Value.ToLocalTime().ToString("yyyy-MM-dd"));
        }
        else
            UntilText.Visibility = Visibility.Collapsed;
        SignInBtn.Content = plan.IsSignedIn ? L.T("accountSignInAgain") : L.T("accountSignIn");
        SignOutBtn.Visibility = plan.IsSignedIn ? Visibility.Visible : Visibility.Collapsed;
        SignOutBtn.IsEnabled = plan.IsSignedIn;
        SignInBtn.IsEnabled = _signIn == null;
    }

    private async void OnSignIn(object sender, RoutedEventArgs e)
    {
        _signIn?.Cancel();
        _signIn = new CancellationTokenSource();
        SignInBtn.IsEnabled = false;
        StatusText.Text = L.T("accountWaiting");
        try
        {
            var result = await _license.SignInAsync(_signIn.Token);
            StatusText.Text = result.Message;
            if (result.Activated)
            {
                try { await _license.RefreshOnlineAsync(_signIn.Token); }
                catch (OperationCanceledException) { /* window closed */ }
                if (IsLoaded)
                    Close();
                return;
            }
        }
        catch (OperationCanceledException)
        {
            StatusText.Text = L.T("accountCancelled");
        }
        finally
        {
            _signIn = null;
            if (IsLoaded)
                Refresh();
        }
    }

    private void OnSignOut(object sender, RoutedEventArgs e)
    {
        if (!AppDialog.Confirm(this, L.T("dlgSignOut"), L.T("msgSignOutBody"), ok: L.T("dlgSignOut")))
            return;
        _license.SignOut();
        Close();
    }

    private void OnUpgrade(object sender, RoutedEventArgs e) => _license.OpenPricing();

    private void OnManage(object sender, RoutedEventArgs e) => _license.OpenAccount();

    private void OnCopyHwid(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(_license.Hwid))
            return;
        Clipboard.SetText(_license.Hwid);
        StatusText.Text = L.T("accountCopied");
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }
}
