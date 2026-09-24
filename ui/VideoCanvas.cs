using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace VideoBlackout.Wpf;

public sealed class VideoCanvas : Grid
{
    public event Action? OpenRequested;
    public event Action<string[]>? FilesDropped;
    public event Action<Int32Rect>? RectDrawn;
    public event Action<int, int>? PointClicked;

    private readonly Image _image = new() { Stretch = Stretch.Uniform };
    private readonly Canvas _overlay = new() { Background = Brushes.Transparent };
    private readonly Border _emptyCard = new()
    {
        CornerRadius = new CornerRadius(16),
        Background = new SolidColorBrush(Color.FromArgb(8, 255, 255, 255)),
        IsHitTestVisible = false
    };
    private readonly Rectangle _emptyDash = new()
    {
        RadiusX = 12,
        RadiusY = 12,
        Stroke = new SolidColorBrush(Color.FromArgb(90, 232, 168, 56)),
        StrokeThickness = 1.5,
        StrokeDashArray = new DoubleCollection { 8, 6 },
        Fill = Brushes.Transparent,
        Margin = new Thickness(18),
        IsHitTestVisible = false
    };
    private readonly StackPanel _emptyText = new()
    {
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        IsHitTestVisible = false
    };
    private readonly Grid _emptyHost = new() { IsHitTestVisible = false };

    private WriteableBitmap? _bmp;
    private Point _origin;
    private Rectangle? _rubber;
    private bool _drawing;
    private long _frameIndex;
    private readonly List<HeldBox> _held = new();
    private bool _hoverEmpty;
    private bool _dragOver;
    private readonly TextBlock[] _emptyLines;
    public bool FollowTool { get; set; } = true;
    public bool DrawEnabled { get; set; } = true;
    public Color DrawColor { get; set; } = Color.FromRgb(0x4C, 0x8D, 0xFF);
    private bool _clickOnly;
    public int VideoWidth { get; private set; }
    public int VideoHeight { get; private set; }
    public bool HasVideo => _bmp != null;

    public void ApplyLocale()
    {
        if (_emptyLines.Length < 5)
            return;
        _emptyLines[0].Text = L.T("emptyDrop");
        _emptyLines[1].Text = L.T("emptyStep1");
        _emptyLines[2].Text = L.T("emptyStep2");
        _emptyLines[3].Text = L.T("emptyKeepLine");
        _emptyLines[4].Text = L.T("emptyBrowse");
    }

    public VideoCanvas()
    {
        Background = new SolidColorBrush(Color.FromRgb(0x0E, 0x11, 0x16));
        AllowDrop = true;
        _emptyHost.Children.Add(_emptyCard);
        _emptyHost.Children.Add(_emptyDash);
        _emptyLines =
        [
            MakeText("", 16, true, Color.FromRgb(0xF2, 0xF5, 0xF8), 0),
            MakeText("", 12, false, Color.FromRgb(0x8B, 0x95, 0xA5), 18),
            MakeText("", 12, false, Color.FromRgb(0x8B, 0x95, 0xA5), 4),
            MakeText("", 12, false, Color.FromRgb(0x8B, 0x95, 0xA5), 4),
            MakeText("", 12, false, Color.FromRgb(0x8B, 0x95, 0xA5), 16)
        ];
        foreach (var line in _emptyLines)
            _emptyText.Children.Add(line);
        ApplyLocale();
        _emptyHost.Children.Add(_emptyText);
        Children.Add(_image);
        Children.Add(_overlay);
        Children.Add(_emptyHost);
        SizeChanged += (_, _) =>
        {
            LayoutEmptyCard();
            LayoutHeld();
        };
        Drop += OnDrop;
        DragOver += (_, e) =>
        {
            e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop)
                ? DragDropEffects.Copy
                : DragDropEffects.None;
            e.Handled = true;
            if (!_dragOver)
            {
                _dragOver = true;
                UpdateEmptyChrome();
            }
        };
        DragLeave += (_, _) =>
        {
            _dragOver = false;
            UpdateEmptyChrome();
        };
        MouseEnter += (_, _) =>
        {
            _hoverEmpty = true;
            UpdateEmptyChrome();
        };
        MouseLeave += (_, _) =>
        {
            _hoverEmpty = false;
            _dragOver = false;
            UpdateEmptyChrome();
        };
        MouseLeftButtonDown += OnDown;
        MouseMove += OnMove;
        MouseLeftButtonUp += OnUp;
        MouseLeftButtonDown += (_, e) =>
        {
            if (!HasVideo)
            {
                OpenRequested?.Invoke();
                e.Handled = true;
            }
        };
        Loaded += (_, _) => LayoutEmptyCard();
    }

    private static TextBlock MakeText(string text, double size, bool bold, Color color, double top) =>
        new()
        {
            Text = text,
            FontSize = size,
            FontWeight = bold ? FontWeights.Bold : FontWeights.Normal,
            Foreground = new SolidColorBrush(color),
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, top, 0, 0)
        };

    private void LayoutEmptyCard()
    {
        if (ActualWidth < 8 || ActualHeight < 8)
            return;
        _emptyHost.Margin = new Thickness(ActualWidth / 6, ActualHeight / 4, ActualWidth / 6, ActualHeight / 4);
    }

    private void UpdateEmptyChrome()
    {
        var active = !HasVideo && (_hoverEmpty || _dragOver);
        _emptyCard.Background = new SolidColorBrush(Color.FromArgb((byte)(active ? 14 : 8), 255, 255, 255));
        _emptyDash.Stroke = new SolidColorBrush(Color.FromArgb((byte)(active ? 180 : 90), 232, 168, 56));
        _emptyDash.StrokeThickness = active ? 2 : 1.5;
    }

    public void SetVideoSize(int w, int h)
    {
        VideoWidth = w;
        VideoHeight = h;
    }

    public void UpdateFrame(byte[] rgb, int width, int height, int stride)
    {
        if (_bmp == null || _bmp.PixelWidth != width || _bmp.PixelHeight != height)
        {
            _bmp = new WriteableBitmap(width, height, 96, 96, PixelFormats.Rgb24, null);
            _image.Source = _bmp;
            _emptyHost.Visibility = Visibility.Collapsed;
        }

        _bmp.WritePixels(new Int32Rect(0, 0, width, height), rgb, stride, 0);
    }

    public void SetFrameIndex(long frame)
    {
        _frameIndex = frame;
        foreach (var mark in _held)
        {
            var show = frame >= mark.Start ? Visibility.Visible : Visibility.Collapsed;
            mark.Shape.Visibility = show;
            mark.Label.Visibility = show;
        }
    }

    public void SyncMaskBoxes(IReadOnlyList<(int Id, int Type, int X, int Y, int W, int H)> boxes)
    {
        foreach (var box in boxes)
        {
            if (box.Type is not (2 or 3) || box.W < 2 || box.H < 2)
                continue;
            var follow = box.Type == 2;
            var mark = _held.FirstOrDefault(m => m.Id == box.Id);
            if (mark == null)
            {
                mark = _held
                    .Where(m => m.Id == 0 && m.Follow == follow)
                    .OrderBy(m => CenterDistance(m, box.X, box.Y, box.W, box.H))
                    .FirstOrDefault(m => CenterDistance(m, box.X, box.Y, box.W, box.H) < 80);
            }
            if (mark == null)
                continue;
            var moved = mark.Id != 0 && (Math.Abs(mark.X - box.X) > 2 || Math.Abs(mark.Y - box.Y) > 2
                || Math.Abs(mark.W - box.W) > 2 || Math.Abs(mark.H - box.H) > 2);
            mark.Id = box.Id;
            mark.X = box.X;
            mark.Y = box.Y;
            mark.W = box.W;
            mark.H = box.H;
            if (moved)
                LayoutHeld();
        }
    }

    public void Clear()
    {
        foreach (var mark in _held)
        {
            _overlay.Children.Remove(mark.Shape);
            _overlay.Children.Remove(mark.Label);
        }
        _held.Clear();
        _bmp = null;
        _image.Source = null;
        _emptyHost.Visibility = Visibility.Visible;
        VideoWidth = 0;
        VideoHeight = 0;
        UpdateEmptyChrome();
        LayoutEmptyCard();
    }

    private void OnDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is string[] files && files.Length > 0)
            FilesDropped?.Invoke(files);
        _dragOver = false;
        UpdateEmptyChrome();
    }

    private Int32Rect MapToVideo(Rect widget)
    {
        if (VideoWidth <= 0 || VideoHeight <= 0 || ActualWidth < 1 || ActualHeight < 1)
            return default;
        var wa = ActualWidth / ActualHeight;
        var va = (double)VideoWidth / VideoHeight;
        double dx, dy, dw, dh;
        if (wa > va)
        {
            dh = ActualHeight;
            dw = dh * va;
            dx = (ActualWidth - dw) / 2;
            dy = 0;
        }
        else
        {
            dw = ActualWidth;
            dh = dw / va;
            dx = 0;
            dy = (ActualHeight - dh) / 2;
        }

        var r = Rect.Intersect(widget, new Rect(dx, dy, dw, dh));
        if (r.IsEmpty)
            return default;
        var sx = VideoWidth / dw;
        var sy = VideoHeight / dh;
        return new Int32Rect(
            (int)((r.X - dx) * sx),
            (int)((r.Y - dy) * sy),
            Math.Max(1, (int)(r.Width * sx)),
            Math.Max(1, (int)(r.Height * sy)));
    }

    private void OnDown(object sender, MouseButtonEventArgs e)
    {
        if (!HasVideo)
            return;
        _origin = e.GetPosition(this);
        CaptureMouse();
        if (!DrawEnabled)
        {
            _clickOnly = true;
            return;
        }
        _drawing = true;
        _rubber = new Rectangle
        {
            Stroke = new SolidColorBrush(DrawColor),
            StrokeThickness = 2,
            Fill = new SolidColorBrush(Color.FromArgb(40, DrawColor.R, DrawColor.G, DrawColor.B))
        };
        Canvas.SetLeft(_rubber, _origin.X);
        Canvas.SetTop(_rubber, _origin.Y);
        _overlay.Children.Add(_rubber);
    }

    private void OnMove(object sender, MouseEventArgs e)
    {
        if (!_drawing || _rubber == null)
            return;
        var p = e.GetPosition(this);
        var x = Math.Min(p.X, _origin.X);
        var y = Math.Min(p.Y, _origin.Y);
        Canvas.SetLeft(_rubber, x);
        Canvas.SetTop(_rubber, y);
        _rubber.Width = Math.Abs(p.X - _origin.X);
        _rubber.Height = Math.Abs(p.Y - _origin.Y);
    }

    private void OnUp(object sender, MouseButtonEventArgs e)
    {
        if (_clickOnly)
        {
            _clickOnly = false;
            ReleaseMouseCapture();
            var pt = MapToVideo(new Rect(_origin.X, _origin.Y, 1, 1));
            PointClicked?.Invoke(pt.X, pt.Y);
            return;
        }
        if (!_drawing)
            return;
        _drawing = false;
        ReleaseMouseCapture();
        if (_rubber == null)
            return;
        var shape = _rubber;
        var r = new Rect(Canvas.GetLeft(shape), Canvas.GetTop(shape), shape.Width, shape.Height);
        _rubber = null;
        if (double.IsNaN(r.Width) || double.IsNaN(r.Height) || r.Width <= 4 || r.Height <= 4)
        {
            _overlay.Children.Remove(shape);
            var pt = MapToVideo(new Rect(_origin.X, _origin.Y, 1, 1));
            PointClicked?.Invoke(pt.X, pt.Y);
            return;
        }
        var video = MapToVideo(r);
        if (video.Width < 2 || video.Height < 2)
        {
            _overlay.Children.Remove(shape);
            return;
        }
        AdoptRubber(shape, video);
        RectDrawn?.Invoke(video);
    }

    private void AdoptRubber(Rectangle shape, Int32Rect video)
    {
        shape.IsHitTestVisible = false;
        var follow = FollowTool;
        var label = new TextBlock
        {
            Text = follow ? "Follow" : "Pin",
            FontSize = 11,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromRgb(0x12, 0x10, 0x0C)),
            Background = new SolidColorBrush(follow
                ? Color.FromRgb(0x4C, 0x8D, 0xFF)
                : Color.FromRgb(0xC8, 0x4D, 0xFF)),
            Padding = new Thickness(4, 1, 4, 1),
            IsHitTestVisible = false
        };
        _overlay.Children.Add(label);
        _held.Add(new HeldBox
        {
            Follow = follow,
            X = video.X,
            Y = video.Y,
            W = video.Width,
            H = video.Height,
            Start = _frameIndex,
            Shape = shape,
            Label = label
        });
        Canvas.SetLeft(label, Canvas.GetLeft(shape));
        Canvas.SetTop(label, Math.Max(0, Canvas.GetTop(shape) - 18));
    }

    private void LayoutHeld()
    {
        if (!TryLetterbox(out var dx, out var dy, out var dw, out var dh))
            return;
        var sx = dw / VideoWidth;
        var sy = dh / VideoHeight;
        foreach (var mark in _held)
        {
            var x = dx + mark.X * sx;
            var y = dy + mark.Y * sy;
            Canvas.SetLeft(mark.Shape, x);
            Canvas.SetTop(mark.Shape, y);
            mark.Shape.Width = Math.Max(2, mark.W * sx);
            mark.Shape.Height = Math.Max(2, mark.H * sy);
            Canvas.SetLeft(mark.Label, x);
            Canvas.SetTop(mark.Label, Math.Max(0, y - 18));
            var show = _frameIndex >= mark.Start ? Visibility.Visible : Visibility.Collapsed;
            mark.Shape.Visibility = show;
            mark.Label.Visibility = show;
        }
    }

    private bool TryLetterbox(out double dx, out double dy, out double dw, out double dh)
    {
        dx = dy = dw = dh = 0;
        if (VideoWidth <= 0 || VideoHeight <= 0 || ActualWidth < 1 || ActualHeight < 1)
            return false;
        var wa = ActualWidth / ActualHeight;
        var va = (double)VideoWidth / VideoHeight;
        if (wa > va)
        {
            dh = ActualHeight;
            dw = dh * va;
            dx = (ActualWidth - dw) / 2;
        }
        else
        {
            dw = ActualWidth;
            dh = dw / va;
            dy = (ActualHeight - dh) / 2;
        }
        return dw > 1 && dh > 1;
    }

    private static double CenterDistance(HeldBox mark, int x, int y, int w, int h)
    {
        var dx = (mark.X + mark.W / 2.0) - (x + w / 2.0);
        var dy = (mark.Y + mark.H / 2.0) - (y + h / 2.0);
        return Math.Sqrt(dx * dx + dy * dy);
    }

    private sealed class HeldBox
    {
        public int Id;
        public bool Follow;
        public int X, Y, W, H;
        public long Start;
        public Rectangle Shape = null!;
        public TextBlock Label = null!;
    }
}
