using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace VideoBlackout.Wpf;

/// <summary>
/// Keeps a borderless window inside the monitor work area so the taskbar
/// does not cover the bottom of the window when it is opened or maximized.
/// </summary>
internal static class MonitorWorkArea
{
    private const int WmGetMinMaxInfo = 0x0024;
    private const uint MonitorDefaultToNearest = 2;

    public static void Attach(Window window)
    {
        window.SourceInitialized += (_, _) =>
        {
            if (PresentationSource.FromVisual(window) is HwndSource source)
                source.AddHook(Hook);
        };
    }

    public static void PlaceInside(Window window)
    {
        var area = WorkAreaDips(window);
        const double margin = 8;
        if (window.Width > area.Width - margin)
            window.Width = Math.Max(640, area.Width - margin);
        if (window.Height > area.Height - margin)
            window.Height = Math.Max(480, area.Height - margin);
        window.Left = area.Left + Math.Max(0, (area.Width - window.Width) / 2);
        window.Top = area.Top + Math.Max(0, (area.Height - window.Height) / 2);
    }

    public static void Clamp(Window window)
    {
        if (window.WindowState != WindowState.Normal)
            return;
        var area = WorkAreaDips(window);
        if (window.Width > area.Width)
            window.Width = area.Width;
        if (window.Height > area.Height)
            window.Height = area.Height;
        if (window.Left < area.Left)
            window.Left = area.Left;
        if (window.Top < area.Top)
            window.Top = area.Top;
        if (window.Left + window.Width > area.Right)
            window.Left = Math.Max(area.Left, area.Right - window.Width);
        if (window.Top + window.Height > area.Bottom)
            window.Top = Math.Max(area.Top, area.Bottom - window.Height);
    }

    private static Rect WorkAreaDips(Window window)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero || !TryWorkPixels(hwnd, out var work))
            return SystemParameters.WorkArea;

        var source = PresentationSource.FromVisual(window);
        var m = source?.CompositionTarget?.TransformFromDevice;
        if (m == null)
            return SystemParameters.WorkArea;
        var topLeft = m.Value.Transform(new Point(work.Left, work.Top));
        var bottomRight = m.Value.Transform(new Point(work.Right, work.Bottom));
        return new Rect(topLeft, bottomRight);
    }

    private static IntPtr Hook(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmGetMinMaxInfo && TryWorkPixels(hwnd, out var work, out var monitor))
        {
            var info = Marshal.PtrToStructure<MinMaxInfo>(lParam);
            info.MaxPosition.X = work.Left - monitor.Left;
            info.MaxPosition.Y = work.Top - monitor.Top;
            info.MaxSize.X = work.Right - work.Left;
            info.MaxSize.Y = work.Bottom - work.Top;
            info.MaxTrack.X = info.MaxSize.X;
            info.MaxTrack.Y = info.MaxSize.Y;
            Marshal.StructureToPtr(info, lParam, true);
            handled = true;
        }
        return IntPtr.Zero;
    }

    private static bool TryWorkPixels(IntPtr hwnd, out RectPixels work) =>
        TryWorkPixels(hwnd, out work, out _);

    private static bool TryWorkPixels(IntPtr hwnd, out RectPixels work, out RectPixels monitor)
    {
        work = default;
        monitor = default;
        var hmon = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        if (hmon == IntPtr.Zero)
            return false;
        var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
        if (!GetMonitorInfo(hmon, ref info))
            return false;
        work = info.Work;
        monitor = info.Monitor;
        return work.Right > work.Left && work.Bottom > work.Top;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    [StructLayout(LayoutKind.Sequential)]
    private struct RectPixels
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo
    {
        public int Size;
        public RectPixels Monitor;
        public RectPixels Work;
        public int Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PointPixels
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MinMaxInfo
    {
        public PointPixels Reserved;
        public PointPixels MaxSize;
        public PointPixels MaxPosition;
        public PointPixels MinTrack;
        public PointPixels MaxTrack;
    }
}
