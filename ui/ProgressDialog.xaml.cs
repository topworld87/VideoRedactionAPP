using System.ComponentModel;
using System.Windows;
using System.Windows.Input;

namespace VideoBlackout.Wpf;

public partial class ProgressDialog : Window
{
    private bool _allowClose;

    public ProgressDialog()
    {
        InitializeComponent();
        TitleText.Text = L.T("progressTitle");
        MessageText.Text = L.T("progressBody");
        CancelBtn.Content = L.T("dlgCancel");
        Closing += OnClosing;
    }

    public event Action? CancelRequested;

    public void SetTitle(string title)
    {
        Title = title;
        TitleText.Text = title;
    }

    public void SetProgress(int percent, string? status)
    {
        Bar.Value = Math.Clamp(percent, 0, 100);
        if (!string.IsNullOrWhiteSpace(status))
            MessageText.Text = status;
    }

    public void SetCancelling()
    {
        MessageText.Text = L.T("progressCancelling");
        CancelBtn.Visibility = Visibility.Collapsed;
    }

    public void AllowClose() => _allowClose = true;

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        SetCancelling();
        CancelRequested?.Invoke();
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
            DragMove();
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_allowClose)
            return;
        e.Cancel = true;
        OnCancelClick(this, new RoutedEventArgs());
    }
}
