using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;

namespace VideoBlackout.Wpf;

public sealed class FilmstripTimeline : FrameworkElement
{
    private const double MaxFrameWidth = 100;
    private const double AudioLane = 52;
    private const double HandlePx = 6;
    private const double MinRangeSec = 0.08;

    private readonly FilmstripLoader _loader;
    private readonly WaveformLoader _waveform;
    private readonly DispatcherTimer _seekTimer;
    private readonly DispatcherTimer _detailTimer;
    private double _fps = 25;
    private long _frameCount;
    private long _position;
    private double _zoom;
    private double _scroll;
    private long _hover = -1;
    private long _seekWant = -1;
    private long _seekSent = -2;
    private bool _scrubbing;
    private const double BarLane = 22;
    private DragMode _drag;
    private double _grabX;
    private bool _barHot;
    private bool _zoomHint;
    private readonly DispatcherTimer _hintTimer;

    private ObservableCollection<AudioRangeMark>? _audioRanges;
    private int _selectedAudioId = -1;
    private int _audioHitId = -1;
    private double _audioAnchorSec;
    private double _audioOrigStart;
    private double _audioOrigEnd;
    private double _draftStart;
    private double _draftEnd;
    private bool _hasDraft;

    public FilmstripTimeline()
    {
        SnapsToDevicePixels = true;
        UseLayoutRounding = true;
        ClipToBounds = true;
        Focusable = true;
        TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.Linear);
        _hintTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(2200) };
        _hintTimer.Tick += (_, _) =>
        {
            _hintTimer.Stop();
            _zoomHint = false;
            InvalidateVisual();
        };

        _loader = new FilmstripLoader(Dispatcher);
        _loader.Updated += () =>
        {
            InvalidateVisual();
            ScheduleDetail();
        };
        _waveform = new WaveformLoader(Dispatcher);
        _waveform.Updated += () => InvalidateVisual();

        _seekTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(70) };
        _seekTimer.Tick += (_, _) => FlushSeek();
        _detailTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(180) };
        _detailTimer.Tick += (_, _) =>
        {
            _detailTimer.Stop();
            RequestDetail();
        };
    }

    public event Action<long>? SeekRequested;
    public event Action<long>? PositionChanged;
    public event Action? ScrubStarted;
    public event Action? ZoomChanged;
    public event Action<double, double, int>? AudioRangeCreated;
    public event Action<int, double, double>? AudioRangeChanged;
    public event Action<int>? AudioRangeSelected;
    public event Action<int>? AudioRangeDeleteRequested;

    /// <summary>0 = mute, 1 = beep for newly drawn ranges.</summary>
    public int NewRangeEffect { get; set; }

    public int SelectedAudioId => _selectedAudioId;

    protected override void OnPropertyChanged(DependencyPropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.Property == IsEnabledProperty)
            InvalidateVisual();
    }

    public bool IsScrubbing => _scrubbing;
    public double Zoom => _zoom;

    public bool FrameLevel => _frameCount > 1 && ActualWidth > 1 && PixelsPerFrame() >= 14;

    public string ZoomLabel
    {
        get
        {
            if (_frameCount <= 0 || ActualWidth < 1)
                return "Scroll to zoom";
            if (_zoom < 0.001)
                return "Whole video";
            if (PixelsPerFrame() >= 14)
                return "1 frame";
            var visible = VisibleFrames;
            if (visible <= 90)
                return $"{Math.Max(1, (int)Math.Round(visible))} frames";
            var sec = visible / Math.Max(0.1, _fps);
            return sec < 90 ? $"{sec:0.#} s" : $"{sec / 60:0.#} min";
        }
    }

    public void BindAudioRanges(ObservableCollection<AudioRangeMark> ranges)
    {
        if (_audioRanges != null)
            _audioRanges.CollectionChanged -= OnAudioCollectionChanged;
        _audioRanges = ranges;
        _audioRanges.CollectionChanged += OnAudioCollectionChanged;
        _selectedAudioId = -1;
        InvalidateVisual();
    }

    public void SetSelectedAudio(int id)
    {
        if (_selectedAudioId == id)
            return;
        _selectedAudioId = id;
        InvalidateVisual();
    }

    public void ClearAudioSelection()
    {
        _selectedAudioId = -1;
        InvalidateVisual();
    }

    private void OnAudioCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e) =>
        InvalidateVisual();

    public void SetVideo(string? path, double fps, long frameCount)
    {
        _detailTimer.Stop();
        _seekTimer.Stop();
        _fps = fps > 0.1 ? fps : 25;
        _frameCount = Math.Max(0, frameCount);
        _position = 0;
        _scroll = 0;
        _zoom = 0;
        _hover = -1;
        _seekWant = -1;
        _seekSent = -2;
        _scrubbing = false;
        _drag = DragMode.None;
        _hasDraft = false;
        _selectedAudioId = -1;
        if (string.IsNullOrEmpty(path) || _frameCount <= 0)
        {
            _loader.Stop();
            _waveform.Stop();
        }
        else
        {
            _loader.Start(path, _fps, _frameCount);
            var dur = Math.Max(0.05, (_frameCount - 1) / Math.Max(0.1, _fps));
            _waveform.Start(path, dur);
        }
        ClampScroll();
        InvalidateVisual();
        ZoomChanged?.Invoke();
    }

    public void SetPosition(long frame, bool follow, bool loadDetail = false)
    {
        if (_scrubbing || _frameCount <= 0)
            return;
        _position = Math.Clamp(frame, 0, _frameCount - 1);
        if (follow)
            Reveal(_position);
        InvalidateVisual();
        if (loadDetail)
            ScheduleDetail();
    }

    public void ApplyZoom(double zoom) => ApplyZoom(zoom, AnchorX());

    public void NudgeZoom(double delta) => ApplyZoom(_zoom + delta);

    public void Shutdown()
    {
        _seekTimer.Stop();
        _detailTimer.Stop();
        if (_audioRanges != null)
            _audioRanges.CollectionChanged -= OnAudioCollectionChanged;
        _loader.Dispose();
        _waveform.Dispose();
    }

    public static string FormatTimecode(long frame, double fps)
    {
        if (frame < 0)
            frame = 0;
        var rate = (int)Math.Round(fps);
        if (rate < 1)
            rate = 25;
        var whole = frame / rate;
        var sub = frame % rate;
        var ff = rate >= 100 ? sub.ToString("000") : sub.ToString("00");
        return $"{whole / 3600:00}:{whole / 60 % 60:00}:{whole % 60:00}:{ff}";
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        var w = double.IsInfinity(availableSize.Width) ? 240 : Math.Max(0, availableSize.Width);
        var h = double.IsInfinity(availableSize.Height) ? 96 : Math.Max(0, availableSize.Height);
        return new Size(w, h);
    }

    protected override HitTestResult HitTestCore(PointHitTestParameters hitTestParameters) =>
        new PointHitTestResult(this, hitTestParameters.HitPoint);

    protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
    {
        base.OnRenderSizeChanged(sizeInfo);
        ClampScroll();
        InvalidateVisual();
        ZoomChanged?.Invoke();
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        if (_frameCount <= 0)
            return;
        Focus();
        var p = e.GetPosition(this);
        CaptureMouse();
        if (InAudioZone(p))
        {
            BeginAudioDrag(p);
            e.Handled = true;
            return;
        }
        _scrubbing = true;
        ScrubStarted?.Invoke();
        if (InBarZone(p))
        {
            _drag = DragMode.Pan;
            TryBar(out var barX, out var barW);
            _grabX = p.X < barX || p.X > barX + barW ? barW / 2 : p.X - barX;
            PanFromBar(p.X);
        }
        else
        {
            _drag = DragMode.Seek;
            if (e.ClickCount == 2)
                ToggleZoom(p.X);
            SeekAt(p.X, immediate: true);
        }
        UpdateCursor(p);
        e.Handled = true;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (_frameCount <= 0)
            return;
        var p = e.GetPosition(this);
        UpdateCursor(p);
        var hot = _drag != DragMode.Seek && InBarZone(p);
        if (hot != _barHot)
        {
            _barHot = hot;
            InvalidateVisual();
        }

        if (_drag is DragMode.AudioCreate or DragMode.AudioMove or DragMode.AudioResizeStart
            or DragMode.AudioResizeEnd)
        {
            UpdateAudioDrag(p.X);
            return;
        }
        if (_drag == DragMode.Seek)
        {
            SeekAt(p.X, immediate: false);
            return;
        }
        if (_drag == DragMode.Pan)
        {
            PanFromBar(p.X);
            return;
        }
        var hover = FrameAt(p.X);
        if (hover == _hover)
            return;
        _hover = hover;
        InvalidateVisual();
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        EndDrag();
        if (IsMouseCaptured)
            ReleaseMouseCapture();
        e.Handled = true;
    }

    protected override void OnLostMouseCapture(MouseEventArgs e) => EndDrag();

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.Delete && _selectedAudioId > 0)
        {
            AudioRangeDeleteRequested?.Invoke(_selectedAudioId);
            e.Handled = true;
            return;
        }
        base.OnKeyDown(e);
    }

    protected override void OnMouseEnter(MouseEventArgs e)
    {
        _zoomHint = true;
        _hintTimer.Stop();
        _hintTimer.Start();
        UpdateCursor(e.GetPosition(this));
        InvalidateVisual();
    }

    protected override void OnMouseLeave(MouseEventArgs e)
    {
        _zoomHint = false;
        _hintTimer.Stop();
        if (_barHot)
        {
            _barHot = false;
            InvalidateVisual();
        }
        if (IsMouseCaptured)
            return;
        Cursor = Cursors.Arrow;
        ForceCursor = false;
        if (_hover < 0)
        {
            InvalidateVisual();
            return;
        }
        _hover = -1;
        InvalidateVisual();
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        if (_frameCount <= 1)
            return;
        var steps = e.Delta / 120.0;
        if (steps == 0)
            return;
        if ((Keyboard.Modifiers & ModifierKeys.Control) != 0)
            ApplyZoom(_zoom + steps * 0.08, e.GetPosition(this).X);
        else
            PanPixels(-steps * Math.Max(48, ActualWidth * 0.18));
        e.Handled = true;
    }

    protected override void OnRender(DrawingContext dc)
    {
        var width = ActualWidth;
        var height = ActualHeight;
        if (width < 2 || height < 2)
            return;
        if (!IsEnabled)
            dc.PushOpacity(0.45);

        const double rulerH = 18;
        var showBar = TryBar(out var barX, out var barW);
        var barSpace = showBar ? BarLane : 0;
        var audioSpace = _frameCount > 0 ? AudioLane : 0;
        var film = new Rect(0, rulerH, width, Math.Max(0, height - rulerH - audioSpace - barSpace));
        var audioRect = new Rect(0, film.Bottom, width, audioSpace);
        dc.DrawRectangle(RulerBg, null, new Rect(0, 0, width, rulerH));
        dc.DrawRectangle(FilmBg, null, film);

        var ppf = PixelsPerFrame();
        dc.PushClip(new RectangleGeometry(film, 5, 5));
        DrawThumbs(dc, _loader.Coarse, film, ppf);
        DrawThumbs(dc, _loader.Detail, film, ppf);
        if (ppf >= 8 && _frameCount > 1)
            DrawFrameLines(dc, film, ppf);
        if (ppf >= 14 && _frameCount > 1)
        {
            var hx = (_position - _scroll) * ppf;
            dc.DrawRectangle(FrameHi, null, new Rect(hx, film.Y, ppf, film.Height));
        }
        if (_loader.Coarse.Count == 0 && !string.IsNullOrEmpty(_loader.Status))
            DrawCentered(dc, _loader.Status, film, MutedBrush);
        dc.Pop();
        dc.DrawRoundedRectangle(null, FilmPen, film, 5, 5);
        DrawRuler(dc, width, rulerH, ppf);

        if (audioSpace > 0)
            DrawAudioLane(dc, audioRect, ppf);

        if (_hover >= 0 && _hover != _position && _drag != DragMode.Seek)
        {
            var hx = (_hover - _scroll) * ppf;
            dc.DrawLine(HoverPen, new Point(Snap(hx), film.Y), new Point(Snap(hx), film.Bottom));
            DrawBadge(dc, hx, film.Y + 4, FormatTimecode(_hover, _fps));
        }

        var px = (_position - _scroll) * ppf;
        if (px >= -1 && px <= width + 1)
        {
            var x = Snap(px);
            dc.DrawLine(PlayPen, new Point(x, 0), new Point(x, film.Bottom + audioSpace));
            var head = new StreamGeometry();
            using (var g = head.Open())
            {
                g.BeginFigure(new Point(px - 5.5, 0), true, true);
                g.LineTo(new Point(px + 5.5, 0), true, false);
                g.LineTo(new Point(px, 8), true, false);
            }
            dc.DrawGeometry(AccentBrush, null, head);
            if (_drag == DragMode.Seek || _hover == _position)
                DrawBadge(dc, px, film.Y + 4, FormatTimecode(_position, _fps));
        }
        else
        {
            DrawEdgeMark(dc, film, px < 0);
        }

        if (showBar)
            DrawBar(dc, width, height, barX, barW);

        if (_zoomHint && IsEnabled)
            DrawZoomHint(dc, width, film.Y + 8);
        if (!IsEnabled)
            dc.Pop();
    }

    private void DrawAudioLane(DrawingContext dc, Rect lane, double ppf)
    {
        dc.DrawRoundedRectangle(AudioLaneBg, AudioLaneEdge, lane, 4, 4);
        dc.PushClip(new RectangleGeometry(lane, 4, 4));
        DrawWaveform(dc, lane, ppf);

        if (_audioRanges != null)
        {
            foreach (var r in _audioRanges)
                DrawAudioRange(dc, lane, ppf, r.StartSec, r.EndSec, r.Effect, r.Id == _selectedAudioId);
        }
        if (_hasDraft)
            DrawAudioRange(dc, lane, ppf, _draftStart, _draftEnd, NewRangeEffect, true);
        dc.Pop();

        var label = Text(L.T("audioLane"), 11, AudioLabelBrush);
        var lw = label.Width + 10;
        var lh = label.Height + 4;
        dc.DrawRoundedRectangle(AudioLabelBg, null, new Rect(6, lane.Y + 3, lw, lh), 3, 3);
        dc.DrawText(label, new Point(11, lane.Y + 4));

        var peaks = _waveform.Peaks;
        if (peaks.Count == 0 && !_waveform.Ready && _audioRanges is { Count: 0 } && !_hasDraft)
        {
            var hint = Text(L.T("audioWaveLoading"), 11, AudioHintBrush);
            dc.DrawText(hint, new Point(Math.Max(8, (lane.Width - hint.Width) / 2),
                lane.Y + (lane.Height - hint.Height) / 2));
        }
        else if (_waveform.Ready && !(_waveform.HasAudio) && _audioRanges is { Count: 0 } && !_hasDraft)
        {
            var hint = Text(L.T("audioWaveEmpty"), 11, AudioHintBrush);
            dc.DrawText(hint, new Point(Math.Max(8, (lane.Width - hint.Width) / 2),
                lane.Y + (lane.Height - hint.Height) / 2));
        }
        else if (peaks.Count == 0 && _audioRanges is { Count: 0 } && !_hasDraft)
        {
            var hint = Text(L.T("audioLaneHint"), 11, AudioHintBrush);
            dc.DrawText(hint, new Point(Math.Max(8, (lane.Width - hint.Width) / 2),
                lane.Y + (lane.Height - hint.Height) / 2));
        }
    }

    private void DrawWaveform(DrawingContext dc, Rect lane, double ppf)
    {
        var peaks = _waveform.Peaks;
        if (peaks.Count == 0 || _frameCount <= 0 || ppf <= 0 || lane.Width < 2)
            return;

        var mid = lane.Y + lane.Height * 0.5;
        var amp = Math.Max(4, lane.Height * 0.42);
        var durationFrames = Math.Max(1.0, _frameCount - 1);
        var geo = new StreamGeometry { FillRule = FillRule.Nonzero };
        using (var g = geo.Open())
        {
            var started = false;
            // Top edge left → right
            for (var x = 0.0; x <= lane.Width + 0.5; x += 1.0)
            {
                var frame = _scroll + x / ppf;
                var t = Math.Clamp(frame / durationFrames, 0, 1);
                var peak = SamplePeak(peaks, t);
                var y = mid - peak * amp;
                if (!started)
                {
                    g.BeginFigure(new Point(x, y), true, true);
                    started = true;
                }
                else
                    g.LineTo(new Point(x, y), true, false);
            }
            // Bottom edge right → left (mirror)
            for (var x = lane.Width; x >= -0.5; x -= 1.0)
            {
                var frame = _scroll + x / ppf;
                var t = Math.Clamp(frame / durationFrames, 0, 1);
                var peak = SamplePeak(peaks, t);
                g.LineTo(new Point(x, mid + peak * amp), true, false);
            }
        }
        geo.Freeze();
        dc.DrawGeometry(WaveFill, null, geo);

        // Center line
        dc.DrawLine(WaveCenterPen, new Point(0, mid), new Point(lane.Width, mid));
    }

    private static float SamplePeak(IReadOnlyList<float> peaks, double t)
    {
        if (peaks.Count == 0)
            return 0;
        if (peaks.Count == 1)
            return peaks[0];
        var pos = t * (peaks.Count - 1);
        var i = (int)Math.Floor(pos);
        if (i < 0)
            return peaks[0];
        if (i >= peaks.Count - 1)
            return peaks[^1];
        var f = (float)(pos - i);
        return peaks[i] * (1 - f) + peaks[i + 1] * f;
    }

    private void DrawAudioRange(DrawingContext dc, Rect lane, double ppf, double startSec, double endSec,
        int effect, bool selected)
    {
        var x0 = (startSec * _fps - _scroll) * ppf;
        var x1 = (endSec * _fps - _scroll) * ppf;
        if (x1 < x0)
            (x0, x1) = (x1, x0);
        if (x1 < 0 || x0 > lane.Width)
            return;
        var fill = effect == 1 ? BeepFill : MuteFill;
        var border = effect == 1 ? BeepPen : MutePen;
        var y = lane.Y + 4;
        var h = lane.Height - 8;
        var rect = new Rect(x0, y, Math.Max(2, x1 - x0), h);
        dc.DrawRoundedRectangle(fill, selected ? SelectedAudioPen : border, rect, 3, 3);
        if (rect.Width >= HandlePx * 2)
        {
            dc.DrawRectangle(HandleFill, null, new Rect(rect.X, rect.Y, 3, rect.Height));
            dc.DrawRectangle(HandleFill, null, new Rect(rect.Right - 3, rect.Y, 3, rect.Height));
        }
    }

    private void BeginAudioDrag(Point p)
    {
        ScrubStarted?.Invoke();
        var sec = SecAt(p.X);
        var hit = HitAudio(p.X, out var edge);
        if (hit != null)
        {
            _audioHitId = hit.Id;
            _selectedAudioId = hit.Id;
            AudioRangeSelected?.Invoke(hit.Id);
            _audioOrigStart = hit.StartSec;
            _audioOrigEnd = hit.EndSec;
            _audioAnchorSec = sec;
            _draftStart = hit.StartSec;
            _draftEnd = hit.EndSec;
            _hasDraft = true;
            _drag = edge switch
            {
                -1 => DragMode.AudioResizeStart,
                1 => DragMode.AudioResizeEnd,
                _ => DragMode.AudioMove
            };
            InvalidateVisual();
            return;
        }

        _audioHitId = -1;
        _selectedAudioId = -1;
        AudioRangeSelected?.Invoke(-1);
        _audioAnchorSec = sec;
        _draftStart = sec;
        _draftEnd = sec;
        _hasDraft = true;
        _drag = DragMode.AudioCreate;
        InvalidateVisual();
    }

    private void UpdateAudioDrag(double x)
    {
        var sec = SecAt(x);
        var maxSec = DurationSec();
        switch (_drag)
        {
            case DragMode.AudioCreate:
                _draftStart = Math.Min(_audioAnchorSec, sec);
                _draftEnd = Math.Max(_audioAnchorSec, sec);
                break;
            case DragMode.AudioMove:
            {
                var dur = _audioOrigEnd - _audioOrigStart;
                var delta = sec - _audioAnchorSec;
                var start = Math.Clamp(_audioOrigStart + delta, 0, Math.Max(0, maxSec - dur));
                _draftStart = start;
                _draftEnd = start + dur;
                break;
            }
            case DragMode.AudioResizeStart:
                _draftStart = Math.Clamp(sec, 0, _audioOrigEnd - MinRangeSec);
                _draftEnd = _audioOrigEnd;
                break;
            case DragMode.AudioResizeEnd:
                _draftStart = _audioOrigStart;
                _draftEnd = Math.Clamp(sec, _audioOrigStart + MinRangeSec, maxSec);
                break;
        }
        InvalidateVisual();
    }

    private void CommitAudioDrag()
    {
        if (!_hasDraft)
            return;
        var start = Math.Min(_draftStart, _draftEnd);
        var end = Math.Max(_draftStart, _draftEnd);
        _hasDraft = false;
        if (end - start < MinRangeSec)
        {
            InvalidateVisual();
            return;
        }
        start = Math.Max(0, start);
        end = Math.Min(DurationSec(), end);
        if (end - start < MinRangeSec)
        {
            InvalidateVisual();
            return;
        }

        if (_drag == DragMode.AudioCreate)
            AudioRangeCreated?.Invoke(start, end, NewRangeEffect);
        else if (_audioHitId > 0)
            AudioRangeChanged?.Invoke(_audioHitId, start, end);
        InvalidateVisual();
    }

    private AudioRangeMark? HitAudio(double x, out int edge)
    {
        edge = 0;
        if (_audioRanges == null || _audioRanges.Count == 0)
            return null;
        var ppf = PixelsPerFrame();
        AudioRangeMark? body = null;
        foreach (var r in _audioRanges)
        {
            var x0 = (r.StartSec * _fps - _scroll) * ppf;
            var x1 = (r.EndSec * _fps - _scroll) * ppf;
            if (x1 < x0)
                (x0, x1) = (x1, x0);
            if (x < x0 - 2 || x > x1 + 2)
                continue;
            if (Math.Abs(x - x0) <= HandlePx)
            {
                edge = -1;
                return r;
            }
            if (Math.Abs(x - x1) <= HandlePx)
            {
                edge = 1;
                return r;
            }
            body = r;
        }
        return body;
    }

    private double SecAt(double x)
    {
        var frame = FrameAt(x);
        return frame / Math.Max(0.1, _fps);
    }

    private double DurationSec() =>
        _frameCount <= 0 ? 0 : Math.Max(0, (_frameCount - 1) / Math.Max(0.1, _fps));

    private void UpdateCursor(Point p)
    {
        if (!IsEnabled || _frameCount <= 0)
        {
            Cursor = Cursors.Arrow;
            ForceCursor = false;
            return;
        }
        if (_drag is DragMode.AudioResizeStart or DragMode.AudioResizeEnd
            || (InAudioZone(p) && HitAudio(p.X, out var edge) != null && edge != 0))
        {
            Cursor = Cursors.SizeWE;
            ForceCursor = true;
            return;
        }
        if (InAudioZone(p) || _drag is DragMode.AudioCreate or DragMode.AudioMove)
        {
            Cursor = Cursors.IBeam;
            ForceCursor = true;
            return;
        }
        Cursor = InBarZone(p) || _drag == DragMode.Pan ? Cursors.SizeAll : Cursors.Hand;
        ForceCursor = true;
    }

    private bool InBarZone(Point p) =>
        TryBar(out _, out _) && p.Y >= ActualHeight - BarLane - 6;

    private bool InAudioZone(Point p)
    {
        if (_frameCount <= 0)
            return false;
        var showBar = TryBar(out _, out _);
        var barSpace = showBar ? BarLane : 0;
        var top = ActualHeight - barSpace - AudioLane;
        var bottom = ActualHeight - barSpace;
        return p.Y >= top && p.Y <= bottom;
    }

    private void DrawBar(DrawingContext dc, double width, double height, double barX, double barW)
    {
        var lane = new Rect(0, height - BarLane, width, BarLane);
        dc.DrawRectangle(RulerBg, null, lane);
        var track = new Rect(0, height - 15, width, 8);
        dc.DrawRoundedRectangle(BarTrack, null, track, 4, 4);
        var hot = _barHot || _drag == DragMode.Pan;
        var thumbH = hot ? 16.0 : 12.0;
        var thumb = new Rect(barX, height - BarLane + (BarLane - thumbH) / 2, barW, thumbH);
        dc.DrawRoundedRectangle(hot ? BarThumbHot : BarThumb, null, thumb, 6, 6);
        var mid = thumb.X + thumb.Width / 2;
        for (var i = -1; i <= 1; i++)
        {
            var x = Snap(mid + i * 4);
            dc.DrawLine(GripPen, new Point(x, thumb.Y + 4), new Point(x, thumb.Bottom - 4));
        }
    }

    private void DrawZoomHint(DrawingContext dc, double width, double y)
    {
        var ft = Text(L.T("timelineZoomHint"), 12, Brushes.White);
        var w = ft.Width + 18;
        var h = ft.Height + 8;
        var x = Math.Max(4, (width - w) / 2);
        dc.DrawRoundedRectangle(BadgeBg, null, new Rect(x, y, w, h), 4, 4);
        dc.DrawText(ft, new Point(x + 9, y + 3));
    }

    private void DrawThumbs(DrawingContext dc, IReadOnlyList<FilmThumb> thumbs, Rect film, double ppf)
    {
        foreach (var thumb in thumbs)
        {
            var x0 = (thumb.Frame - _scroll) * ppf;
            var x1 = (thumb.Frame + thumb.Span - _scroll) * ppf;
            if (x1 < 0 || x0 > film.Width)
                continue;
            DrawSpan(dc, thumb.Image, x0, x1, film.Y, film.Height);
        }
    }

    private static void DrawSpan(DrawingContext dc, ImageSource image, double x0, double x1, double y, double h)
    {
        var spanW = x1 - x0;
        if (spanW < 1 || h < 1 || image.Width < 1 || image.Height < 1)
            return;
        var dest = new Rect(x0, y, spanW, h);
        dc.PushClip(new RectangleGeometry(dest));
        var tile = Math.Min(spanW, Math.Clamp(h * 16.0 / 9.0, 64, 140));
        if (spanW <= tile * 1.25)
            DrawCover(dc, image, dest);
        else
        {
            for (var x = x0; x < x1 - 0.5; x += tile)
                DrawCover(dc, image, new Rect(x, y, Math.Min(tile, x1 - x), h));
        }
        dc.Pop();
    }

    private static void DrawCover(DrawingContext dc, ImageSource image, Rect dest)
    {
        var scale = Math.Max(dest.Width / image.Width, dest.Height / image.Height);
        var w = image.Width * scale;
        var h = image.Height * scale;
        var x = dest.X + (dest.Width - w) / 2;
        var y = dest.Y + (dest.Height - h) / 2;
        dc.DrawImage(image, new Rect(x, y, w, h));
    }

    private void DrawFrameLines(DrawingContext dc, Rect film, double ppf)
    {
        var first = Math.Max(0, (long)Math.Floor(_scroll));
        var last = Math.Min(_frameCount, first + (long)Math.Ceiling(film.Width / ppf) + 2);
        for (var frame = first; frame <= last; frame++)
        {
            var x = Snap((frame - _scroll) * ppf);
            if (x < -1 || x > film.Right + 1)
                continue;
            dc.DrawLine(FramePen, new Point(x, film.Y), new Point(x, film.Bottom));
        }
    }

    private void DrawRuler(DrawingContext dc, double width, double rulerH, double ppf)
    {
        if (_frameCount <= 0 || ppf <= 0)
            return;
        var step = ChooseStep(ppf);
        var minor = step >= 10 ? step / 5 : 0;
        var start = Math.Max(0, (long)(_scroll - step));
        if (minor >= 1)
        {
            var aligned = (long)(Math.Floor(start / minor) * minor);
            for (var frame = aligned; ; frame += (long)minor)
            {
                var x = (frame - _scroll) * ppf;
                if (x > width)
                    break;
                if (x >= 0 && frame % (long)step != 0)
                    dc.DrawLine(TickPen, new Point(Snap(x), rulerH - 4), new Point(Snap(x), rulerH));
            }
        }
        var labelStep = (long)Math.Max(1, Math.Round(step));
        var firstLabel = (long)(Math.Floor(start / (double)labelStep) * labelStep);
        for (var frame = firstLabel; frame < _frameCount + labelStep; frame += labelStep)
        {
            var x = (frame - _scroll) * ppf;
            if (x > width + 4)
                break;
            if (x < -40)
                continue;
            dc.DrawLine(TickPen, new Point(Snap(x), rulerH - 8), new Point(Snap(x), rulerH));
            var text = step < _fps * 0.95 && step <= 30
                ? frame.ToString(CultureInfo.CurrentCulture)
                : FormatTimecode(frame, _fps);
            var ft = Text(text, 10, MutedBrush);
            var tx = x + 3;
            if (tx + ft.Width > width)
                tx = x - ft.Width - 3;
            if (tx >= -2)
                dc.DrawText(ft, new Point(tx, 1));
        }
    }

    private double ChooseStep(double ppf)
    {
        var minFrames = 88 / Math.Max(0.01, ppf);
        if (minFrames <= 1)
            return 1;
        foreach (var step in new double[] { 2, 5, 10, 15, 30 })
        {
            if (step >= minFrames)
                return step;
        }
        foreach (var sec in new double[] { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 })
        {
            var frames = sec * _fps;
            if (frames >= minFrames)
                return frames;
        }
        return minFrames;
    }

    private void DrawBadge(DrawingContext dc, double x, double y, string text)
    {
        var ft = Text(text, 11, Brushes.White);
        var w = ft.Width + 10;
        var h = ft.Height + 4;
        var bx = x + 8;
        if (bx + w > ActualWidth - 2)
            bx = x - w - 8;
        bx = Math.Max(2, bx);
        dc.DrawRoundedRectangle(BadgeBg, null, new Rect(bx, y, w, h), 3, 3);
        dc.DrawText(ft, new Point(bx + 5, y + 1));
    }

    private void DrawEdgeMark(DrawingContext dc, Rect film, bool atStart)
    {
        var x = atStart ? 8 : film.Right - 8;
        var geo = new StreamGeometry();
        using (var g = geo.Open())
        {
            if (atStart)
            {
                g.BeginFigure(new Point(x + 5, film.Y + 6), true, true);
                g.LineTo(new Point(x - 2, film.Y + 11), true, false);
                g.LineTo(new Point(x + 5, film.Y + 16), true, false);
            }
            else
            {
                g.BeginFigure(new Point(x - 5, film.Y + 6), true, true);
                g.LineTo(new Point(x + 2, film.Y + 11), true, false);
                g.LineTo(new Point(x - 5, film.Y + 16), true, false);
            }
        }
        dc.DrawGeometry(AccentBrush, null, geo);
    }

    private void DrawCentered(DrawingContext dc, string text, Rect area, Brush brush)
    {
        var ft = Text(text, 12, brush);
        dc.DrawText(ft, new Point(area.X + (area.Width - ft.Width) / 2, area.Y + (area.Height - ft.Height) / 2));
    }

    private FormattedText Text(string value, double size, Brush brush) =>
        new(value, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            Consolas, size, brush, VisualTreeHelper.GetDpi(this).PixelsPerDip);

    private void SeekAt(double x, bool immediate)
    {
        var frame = FrameAt(x);
        _position = frame;
        _hover = frame;
        PositionChanged?.Invoke(frame);
        InvalidateVisual();
        _seekWant = frame;
        if (immediate)
            FlushSeek();
        else if (!_seekTimer.IsEnabled)
            _seekTimer.Start();
    }

    private void FlushSeek()
    {
        if (_seekWant < 0 || _seekWant == _seekSent)
            return;
        _seekSent = _seekWant;
        SeekRequested?.Invoke(_seekWant);
    }

    private void EndDrag()
    {
        if (_drag == DragMode.None && !_scrubbing)
            return;
        var mode = _drag;
        if (mode is DragMode.AudioCreate or DragMode.AudioMove or DragMode.AudioResizeStart
            or DragMode.AudioResizeEnd)
            CommitAudioDrag();
        _drag = DragMode.None;
        _scrubbing = false;
        _seekTimer.Stop();
        if (mode == DragMode.Seek)
            FlushSeek();
        ScheduleDetail();
        InvalidateVisual();
    }

    private void PanFromBar(double mouseX)
    {
        if (!TryBar(out _, out var barW))
            return;
        var track = Math.Max(1, ActualWidth - barW);
        var t = Math.Clamp((mouseX - _grabX) / track, 0, 1);
        var visible = ActualWidth / PixelsPerFrame();
        _scroll = t * Math.Max(0, _frameCount - visible);
        ClampScroll();
        InvalidateVisual();
        ScheduleDetail();
    }

    private void PanPixels(double dx)
    {
        var ppf = PixelsPerFrame();
        if (ppf <= 0)
            return;
        _scroll += dx / ppf;
        ClampScroll();
        InvalidateVisual();
        ScheduleDetail();
    }

    private void ToggleZoom(double anchorX)
    {
        if (_zoom > 0.02)
        {
            ApplyZoom(0, anchorX);
            return;
        }
        var target = ActualWidth / Math.Min(_frameCount, 12);
        ApplyZoom(ZoomFromPpf(target), anchorX);
    }

    private void ApplyZoom(double zoom, double anchorX)
    {
        if (_frameCount <= 1 || ActualWidth < 2)
            return;
        zoom = Math.Clamp(zoom, 0, 1);
        if (Math.Abs(zoom - _zoom) < 0.0008)
            return;
        var old = PixelsPerFrame();
        var anchorFrame = _scroll + anchorX / old;
        _zoom = zoom;
        var next = PixelsPerFrame();
        _scroll = anchorFrame - anchorX / next;
        ClampScroll();
        InvalidateVisual();
        ZoomChanged?.Invoke();
        ScheduleDetail();
    }

    private double AnchorX()
    {
        var x = (_position - _scroll) * PixelsPerFrame();
        return x < 0 || x > ActualWidth ? ActualWidth / 2 : x;
    }

    private void Reveal(long frame)
    {
        var ppf = PixelsPerFrame();
        if (ppf <= 0 || ActualWidth < 2)
            return;
        var visible = ActualWidth / ppf;
        if (visible >= _frameCount)
        {
            _scroll = 0;
            return;
        }
        var x = (frame - _scroll) * ppf;
        if (x < ActualWidth * 0.12 || x > ActualWidth * 0.82)
            _scroll = frame - visible * 0.33;
        ClampScroll();
    }

    private long FrameAt(double x)
    {
        var ppf = PixelsPerFrame();
        if (ppf <= 0 || _frameCount <= 0)
            return 0;
        return Math.Clamp((long)Math.Floor(_scroll + x / ppf), 0, _frameCount - 1);
    }

    private double PixelsPerFrame()
    {
        var width = Math.Max(1, ActualWidth);
        var fit = width / Math.Max(1, _frameCount);
        var max = Math.Max(fit, MaxFrameWidth);
        if (_frameCount <= 1 || max <= fit * 1.001)
            return fit;
        return fit * Math.Pow(max / fit, Math.Clamp(_zoom, 0, 1));
    }

    private double ZoomFromPpf(double ppf)
    {
        var width = Math.Max(1, ActualWidth);
        var fit = width / Math.Max(1, _frameCount);
        var max = Math.Max(fit, MaxFrameWidth);
        if (max <= fit * 1.001)
            return 0;
        ppf = Math.Clamp(ppf, fit, max);
        return Math.Clamp(Math.Log(ppf / fit) / Math.Log(max / fit), 0, 1);
    }

    private double VisibleFrames => ActualWidth / Math.Max(0.01, PixelsPerFrame());

    private void ClampScroll()
    {
        var ppf = PixelsPerFrame();
        var visible = ActualWidth / Math.Max(0.01, ppf);
        var max = Math.Max(0, _frameCount - visible);
        _scroll = Math.Clamp(_scroll, 0, max);
    }

    private bool TryBar(out double x, out double w)
    {
        x = 0;
        w = 0;
        if (_frameCount <= 1 || ActualWidth <= 1)
            return false;
        var ppf = PixelsPerFrame();
        var visible = ActualWidth / ppf;
        if (visible >= _frameCount - 0.5)
            return false;
        w = Math.Min(ActualWidth, Math.Max(48, ActualWidth * visible / _frameCount));
        var span = Math.Max(1, ActualWidth - w);
        var maxScroll = Math.Max(0.001, _frameCount - visible);
        x = (_scroll / maxScroll) * span;
        return true;
    }

    private void ScheduleDetail()
    {
        if (_frameCount <= 1)
            return;
        _detailTimer.Stop();
        _detailTimer.Start();
    }

    private void RequestDetail()
    {
        if (_frameCount <= 1 || ActualWidth < 2)
            return;
        var ppf = PixelsPerFrame();
        var coarse = _loader.CoarseSpan;
        if (coarse <= 0 || coarse * ppf < 140)
            return;
        var visible = Math.Max(1, ActualWidth / ppf);
        var pad = Math.Max(1, visible * 0.35);
        var start = (long)Math.Max(0, Math.Floor(_scroll - pad));
        var end = (long)Math.Min(_frameCount, Math.Ceiling(_scroll + visible + pad));
        if (end <= start)
            return;
        var window = end - start;
        const int maxThumbs = 56;
        var span = Math.Max(1, (window + maxThumbs - 1) / maxThumbs);
        if (span >= coarse)
            return;
        _loader.RequestDetail(start, end, span);
    }

    private static double Snap(double v) => Math.Round(v) + 0.5;

    private enum DragMode
    {
        None, Seek, Pan,
        AudioCreate, AudioMove, AudioResizeStart, AudioResizeEnd
    }

    private static readonly Typeface Consolas = new("Consolas");
    private static readonly Brush RulerBg = Freeze(new SolidColorBrush(Color.FromRgb(0x10, 0x15, 0x1C)));
    private static readonly Brush FilmBg = Freeze(new SolidColorBrush(Color.FromRgb(0x0B, 0x0F, 0x14)));
    private static readonly Brush AccentBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xE8, 0xA8, 0x38)));
    private static readonly Brush MutedBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x8B, 0x95, 0xA5)));
    private static readonly Brush FrameHi = Freeze(new SolidColorBrush(Color.FromArgb(56, 0xE8, 0xA8, 0x38)));
    private static readonly Brush BadgeBg = Freeze(new SolidColorBrush(Color.FromArgb(230, 0x15, 0x1A, 0x22)));
    private static readonly Brush BarTrack = Freeze(new SolidColorBrush(Color.FromRgb(0x1C, 0x24, 0x30)));
    private static readonly Brush BarThumb = Freeze(new SolidColorBrush(Color.FromRgb(0x8B, 0x9B, 0xB0)));
    private static readonly Brush BarThumbHot = Freeze(new SolidColorBrush(Color.FromRgb(0xE8, 0xEC, 0xF1)));
    private static readonly Brush GripBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x1A, 0x20, 0x28)));
    private static readonly Pen GripPen = FreezePen(new Pen(GripBrush, 1.5));
    private static readonly Brush FilmEdge = Freeze(new SolidColorBrush(Color.FromRgb(0x2C, 0x36, 0x45)));
    private static readonly Brush HoverBrush = Freeze(new SolidColorBrush(Color.FromArgb(120, 255, 255, 255)));
    private static readonly Brush FrameLine = Freeze(new SolidColorBrush(Color.FromArgb(70, 255, 255, 255)));
    private static readonly Brush TickBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x5C, 0x6B, 0x80)));
    private static readonly Pen FilmPen = FreezePen(new Pen(FilmEdge, 1));
    private static readonly Pen PlayPen = FreezePen(new Pen(Brushes.White, 1.5));
    private static readonly Pen HoverPen = FreezePen(new Pen(HoverBrush, 1));
    private static readonly Pen FramePen = FreezePen(new Pen(FrameLine, 1));
    private static readonly Pen TickPen = FreezePen(new Pen(TickBrush, 1));
    private static readonly Brush AudioLaneBg = Freeze(new SolidColorBrush(Color.FromRgb(0x14, 0x2E, 0x2C)));
    private static readonly Brush AudioLabelBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x6E, 0xE0, 0xC8)));
    private static readonly Brush AudioLabelBg = Freeze(new SolidColorBrush(Color.FromArgb(180, 0x10, 0x20, 0x1E)));
    private static readonly Brush AudioHintBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xA8, 0xE0, 0xD4)));
    private static readonly Pen AudioLaneEdge = FreezePen(new Pen(
        Freeze(new SolidColorBrush(Color.FromRgb(0x3D, 0xB8, 0xA0))), 1.5));
    private static readonly Brush WaveFill = Freeze(new SolidColorBrush(Color.FromArgb(200, 0x3D, 0xC4, 0xA8)));
    private static readonly Pen WaveCenterPen = FreezePen(new Pen(
        Freeze(new SolidColorBrush(Color.FromArgb(70, 0x6E, 0xE0, 0xC8))), 1));
    private static readonly Brush MuteFill = Freeze(new SolidColorBrush(Color.FromArgb(110, 0x2E, 0xC4, 0xA8)));
    private static readonly Brush BeepFill = Freeze(new SolidColorBrush(Color.FromArgb(130, 0xE8, 0xA8, 0x38)));
    private static readonly Brush HandleFill = Freeze(new SolidColorBrush(Color.FromArgb(220, 255, 255, 255)));
    private static readonly Pen MutePen = FreezePen(new Pen(
        Freeze(new SolidColorBrush(Color.FromRgb(0x6E, 0xE0, 0xC8))), 1));
    private static readonly Pen BeepPen = FreezePen(new Pen(
        Freeze(new SolidColorBrush(Color.FromRgb(0xE8, 0xA8, 0x38))), 1));
    private static readonly Pen SelectedAudioPen = FreezePen(new Pen(Brushes.White, 1.5));

    private static SolidColorBrush Freeze(SolidColorBrush brush)
    {
        brush.Freeze();
        return brush;
    }

    private static Pen FreezePen(Pen pen)
    {
        pen.Freeze();
        return pen;
    }
}
