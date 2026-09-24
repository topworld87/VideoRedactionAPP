using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Input;

namespace VideoBlackout.Wpf;

public partial class LicensesWindow : Window
{
    public LicensesWindow()
    {
        InitializeComponent();
        Title = L.T("msgLicenses");
        TitleText.Text = L.T("msgLicenses");
        NoticeBox.Text = OpenSourceNotice.Load();
    }

    private void OnViewLicense(object sender, RoutedEventArgs e)
    {
        var path = OpenSourceNotice.LicensePath("AGPL-3.0.txt");
        if (!File.Exists(path))
        {
            AppDialog.Show(this, L.T("msgLicenses"), L.T("msgLicenseMissing"));
            return;
        }
        Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
    }

    private void OnCloseClick(object sender, RoutedEventArgs e) => Close();

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }
}
