using System.Windows;
using System.Windows.Input;

namespace VideoBlackout.Wpf;

public partial class AppDialog : Window
{
    private AppDialog()
    {
        InitializeComponent();
        PreviewKeyDown += (_, e) =>
        {
            if (e.Key != Key.Escape)
                return;
            DialogResult = CancelBtn.Visibility == Visibility.Visible ? false : true;
            e.Handled = true;
        };
    }

    public static void Show(Window? owner, string title, string message,
        string? detail = null, string? detailLabel = null)
    {
        var dlg = Create(owner, title, message, detail, detailLabel);
        dlg.CancelBtn.Visibility = Visibility.Collapsed;
        dlg.OkBtn.Content = L.T("dlgOk");
        dlg.ShowDialog();
    }

    public static bool Confirm(Window? owner, string title, string message,
        string? ok = null, string? cancel = null)
    {
        var dlg = Create(owner, title, message);
        dlg.OkBtn.Content = ok ?? L.T("dlgOk");
        dlg.CancelBtn.Content = cancel ?? L.T("dlgCancel");
        dlg.CancelBtn.Visibility = Visibility.Visible;
        return dlg.ShowDialog() == true;
    }

    public static bool Ask(Window? owner, string title, string message,
        string? detail = null, string? yes = null, string? no = null)
    {
        var dlg = Create(owner, title, message, detail);
        dlg.OkBtn.Content = yes ?? L.T("dlgOk");
        dlg.CancelBtn.Content = no ?? L.T("dlgCancel");
        dlg.CancelBtn.Visibility = Visibility.Visible;
        return dlg.ShowDialog() == true;
    }

    private static AppDialog Create(Window? owner, string title, string message,
        string? detail = null, string? detailLabel = null)
    {
        var dlg = new AppDialog();
        if (owner is { IsLoaded: true })
            dlg.Owner = owner;
        else
            dlg.WindowStartupLocation = WindowStartupLocation.CenterScreen;
        dlg.Title = title;
        dlg.TitleText.Text = title;
        dlg.MessageText.Text = message;
        if (!string.IsNullOrWhiteSpace(detail))
        {
            dlg.DetailBox.Text = detail;
            dlg.DetailBox.Visibility = Visibility.Visible;
            if (!string.IsNullOrWhiteSpace(detailLabel))
            {
                dlg.DetailLabel.Text = detailLabel;
                dlg.DetailLabel.Visibility = Visibility.Visible;
            }
            else
                dlg.DetailBox.Margin = new Thickness(0, 12, 0, 2);
        }
        return dlg;
    }

    private void OnOkClick(object sender, RoutedEventArgs e) => DialogResult = true;

    private void OnCancelClick(object sender, RoutedEventArgs e) => DialogResult = false;

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        DialogResult = CancelBtn.Visibility == Visibility.Visible ? false : true;
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }
}
