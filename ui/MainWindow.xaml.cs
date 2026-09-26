using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace VideoBlackout.Wpf;

public partial class MainWindow : Window
{
    private readonly NativeEngine _engine = new();
    private readonly PreviewAudio _previewAudio = new();
    private readonly ObservableCollection<TrackRow> _tracks = new();
    private readonly ObservableCollection<FileRow> _files = new();
    private readonly ObservableCollection<AudioRangeMark> _audioRanges = new();
    private int _localAudioId = 1;
    private readonly ICollectionView _trackView;
    private readonly CollectionViewSource _maskSource = new();
    private bool _playing;
    private bool _previewExpanded;
    private long _frameCount = 1;
    private double _fps = 25;
    private long _currentFrame;
    private bool _follow = true;
    private bool _reverse;
    private string? _openedPath;
    private string? _pendingOpenPath;
    private ProgressDialog? _exportDlg;
    private List<FileRow>? _batch;
    private int _batchIndex;
    private bool _batchRunning;
    private readonly LicenseClient _license = new(NativeEngine.HardwareId());
    private CancellationTokenSource? _signIn;
    private bool _signingIn;
    private static readonly string[] VideoExt =
        [".mp4", ".avi", ".mov", ".mkv", ".wmv", ".m4v", ".webm", ".mpg", ".mpeg"];

    public MainWindow()
    {
        AppLocale.Load();
        MonitorWorkArea.Attach(this);
        InitializeComponent();
        L.Changed += OnLocaleChanged;
        _license.Changed += OnLicenseChanged;
        _license.Load();
        _trackView = CollectionViewSource.GetDefaultView(_tracks);
        _trackView.Filter = TrackFilter;
        TrackList.ItemsSource = _trackView;
        _maskSource.Source = _tracks;
        _maskSource.Filter += (_, e) => e.Accepted = e.Item is TrackRow row && row.IsMaskEntry;
        MaskList.ItemsSource = _maskSource.View;
        FileList.ItemsSource = _files;
        AllowDrop = true;
        Drop += (_, e) => HandleDroppedPaths(e);
        Canvas.OpenRequested += () => OnOpen(this, new RoutedEventArgs());
        Canvas.FilesDropped += files => HandleDroppedPaths(files);
        Canvas.RectDrawn += OnRectDrawn;
        Canvas.PointClicked += OnCanvasClick;

        _engine.FrameReady += OnFrame;
        _engine.TracksUpdated += tracks => Dispatcher.BeginInvoke(() => ApplyTracks(tracks));
        _engine.Opened += (fps, frames, w, h) =>
        {
            var path = _pendingOpenPath;
            var fps1 = fps > 0.1 ? fps : 25;
            var frames1 = Math.Max(1, frames);
            Dispatcher.BeginInvoke(() =>
            {
                _fps = fps1;
                _frameCount = frames1;
                Timeline.SetVideo(path, _fps, _frameCount);
                Canvas.SetVideoSize(w, h);
                UpdateTime(0);
                RuntimeStatus.Text = _engine.RuntimeStatus();
                if (!string.IsNullOrEmpty(path))
                    _previewAudio.Open(path, _fps);
            });
        };
        _engine.ErrorOccurred += msg => Dispatcher.BeginInvoke(() =>
            AppDialog.Show(this, L.T("msgApp"), msg));
        _engine.PlayFinished += () => Dispatcher.BeginInvoke(() =>
        {
            _playing = false;
            PlayBtn.Content = "▶";
            _previewAudio.Pause();
        });
        _engine.ExportProgress += (pct, status) => Dispatcher.BeginInvoke(() =>
            _exportDlg?.SetProgress(pct, status));
        _engine.ExportDone += (ok, path) => Dispatcher.BeginInvoke(() => OnExportFinished(ok, path));

        Timeline.SeekRequested += OnTimelineSeek;
        Timeline.PositionChanged += OnTimelinePosition;
        Timeline.ScrubStarted += OnTimelineScrubStarted;
        Timeline.BindAudioRanges(_audioRanges);
        Timeline.AudioRangeCreated += OnAudioRangeCreated;
        Timeline.AudioRangeChanged += OnAudioRangeChanged;
        Timeline.AudioRangeSelected += OnAudioRangeSelected;
        Timeline.AudioRangeDeleteRequested += id => RemoveAudioRange(id);

        Closed += (_, _) =>
        {
            _signIn?.Cancel();
            CloseExportProgressDialog();
            Timeline.Shutdown();
            _previewAudio.Dispose();
            _engine.Dispose();
        };
        ApplyLocaleChrome();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        ApplyLocaleChrome();
        CenterOnWorkArea();
        RefreshPreviewTransport();
        RefreshPlanUi();
        RefreshProcessUi();
        RefreshTargetEmpty();
        _ = RefreshLicenseAsync();
    }

    private async Task RefreshLicenseAsync()
    {
        try
        {
            await _license.RefreshOnlineAsync(CancellationToken.None);
        }
        catch
        {
            // Keep the cached plan when the check cannot finish.
        }
        await Dispatcher.InvokeAsync(() =>
        {
            RefreshPlanUi();
            RefreshProcessUi();
        });
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && _previewExpanded)
        {
            SetPreviewExpanded(false);
            e.Handled = true;
            return;
        }
        if (e.Key == Key.F8)
        {
            OnDumpDetectDebug(sender, e);
            e.Handled = true;
            return;
        }
        if (Keyboard.FocusedElement is System.Windows.Controls.Primitives.TextBoxBase
            or ListBox or ListBoxItem or Slider or ComboBox or ComboBoxItem or MenuItem)
            return;
        if ((Keyboard.Modifiers & (ModifierKeys.Control | ModifierKeys.Alt)) != 0)
            return;
        var bySecond = (Keyboard.Modifiers & ModifierKeys.Shift) != 0;
        if (e.Key == Key.Left)
        {
            StepFrame(-1, bySecond);
            e.Handled = true;
        }
        else if (e.Key == Key.Right)
        {
            StepFrame(1, bySecond);
            e.Handled = true;
        }
        else if (e.Key == Key.Home)
        {
            StepTo(0);
            e.Handled = true;
        }
        else if (e.Key == Key.End)
        {
            StepTo(Math.Max(0, _frameCount - 1));
            e.Handled = true;
        }
        else if (e.Key == Key.Delete)
        {
            var id = Timeline.SelectedAudioId;
            if (id > 0)
            {
                RemoveAudioRange(id);
                e.Handled = true;
            }
        }
    }

    private void StepFrame(int direction, bool bySecond)
    {
        if (string.IsNullOrEmpty(_openedPath) || _frameCount <= 1)
            return;
        var step = bySecond ? Math.Max(1, (long)Math.Round(_fps)) : 1L;
        StepTo(_currentFrame + direction * step);
    }

    private void StepTo(long frame)
    {
        if (string.IsNullOrEmpty(_openedPath) || _frameCount <= 0)
            return;
        OnTimelineScrubStarted();
        var next = Math.Clamp(frame, 0, _frameCount - 1);
        _currentFrame = next;
        Timeline.SetPosition(next, follow: true, loadDetail: true);
        UpdateTime(next);
        _engine.Seek(next);
        _previewAudio.SeekFrame(next);
        SyncPreviewRedaction(next);
    }

    private void OnDumpDetectDebug(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_openedPath))
        {
            AppDialog.Show(this, L.T("msgDetectDebug"), L.T("msgOpenPause"));
            return;
        }
        _engine.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
        var dir = _engine.DumpDetectDebug();
        if (string.IsNullOrEmpty(dir))
        {
            AppDialog.Show(this, L.T("msgDetectDebug"), L.T("msgDumpFailed"));
            return;
        }
        AppDialog.Show(this, L.T("msgDetectDebug"), L.T("msgDumpSaved"), dir);
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = dir,
                UseShellExecute = true
            });
        }
        catch
        {
            // Folder path is already shown in the dialog.
        }
    }

    private void CenterOnWorkArea() => MonitorWorkArea.PlaceInside(this);

    protected override void OnStateChanged(EventArgs e)
    {
        base.OnStateChanged(e);
        if (WindowState == WindowState.Normal)
            MonitorWorkArea.Clamp(this);
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
            return;
        if (e.ClickCount == 2)
        {
            OnMax(sender, e);
            return;
        }
        if (WindowState == WindowState.Maximized)
            return;
        DragMove();
    }

    private void OnFrame(IntPtr rgb, int w, int h, int stride, long index, double pts)
    {
        var buf = new byte[h * stride];
        Marshal.Copy(rgb, buf, 0, buf.Length);
        Dispatcher.BeginInvoke(() =>
        {
            Canvas.UpdateFrame(buf, w, h, stride);
            Canvas.SetFrameIndex(index);
            if (Timeline.IsScrubbing)
                return;
            _currentFrame = index;
            Timeline.SetPosition(index, _playing);
            UpdateTime(index);
            SyncPreviewRedaction(index);
        });
    }

    private void UpdateTime(long frame)
    {
        var cur = FilmstripTimeline.FormatTimecode(frame, _fps);
        var total = FilmstripTimeline.FormatTimecode(Math.Max(0, _frameCount), _fps);
        TimeCurrent.Text = cur;
        TimeTotal.Text = total;
    }

    private void SyncPreviewRedaction(long frame)
    {
        var sec = frame / Math.Max(0.1, _fps);
        _previewAudio.ApplyRedactionAt(sec, _audioRanges);
    }

    private void ApplyTracks(NativeEngine.TrackC[] tracks)
    {
        var seen = new HashSet<int>();
        foreach (var t in tracks)
        {
            seen.Add(t.Id);
            var row = _tracks.FirstOrDefault(r => r.Id == t.Id);
            if (row == null)
            {
                row = new TrackRow(t.Id, t.Label, t.Enabled != 0, t.MatchTier, t.Type);
                row.SetJobPresentation(_reverse);
                _tracks.Add(row);
                continue;
            }
            row.Label = t.Label;
            row.MatchTier = t.MatchTier;
            row.SetJobPresentation(_reverse);
            if (row.Enabled != (t.Enabled != 0))
                row.Enabled = t.Enabled != 0;
        }
        for (var i = _tracks.Count - 1; i >= 0; i--)
        {
            if (!seen.Contains(_tracks[i].Id))
                _tracks.RemoveAt(i);
        }
        _trackView.Refresh();
        _maskSource.View.Refresh();
        Canvas.SyncMaskBoxes(tracks
            .Where(t => t.Type is 2 or 3 && t.Lost == 0)
            .Select(t => (t.Id, t.Type, t.X, t.Y, t.W, t.H))
            .ToList());
        RefreshTargetEmpty();
        RefreshProcessUi();
    }

    private bool TrackFilter(object obj) =>
        obj is TrackRow row && (!_reverse || row.IsKeepEntry);

    private bool OpenVideo(string path, bool quiet = false)
    {
        if (!File.Exists(path))
            return false;
        if (string.Equals(_openedPath, path, StringComparison.OrdinalIgnoreCase))
        {
            ApplyDetect();
            _engine.SetFacePolicy(_reverse ? 1 : 0);
            return true;
        }
        EnqueuePaths([path]);
        SelectPath(path);
        _engine.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
        _pendingOpenPath = path;
        _previewAudio.Stop();
        if (!_engine.Open(path))
        {
            if (!quiet)
                AppDialog.Show(this, L.T("msgApp"), L.T("msgOpenFail"));
            return false;
        }
        _openedPath = path;
        _tracks.Clear();
        ClearAudioRangesUi();
        _engine.ClearGallery();
        RuntimeStatus.Text = _engine.RuntimeStatus();
        _engine.SetFacePolicy(_reverse ? 1 : 0);
        ApplyDetect();
        RefreshTargetEmpty();
        RefreshProcessUi();
        RefreshPreviewTransport();
        return true;
    }

    private void ClosePreview()
    {
        _openedPath = null;
        _engine.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
        _previewAudio.Stop();
        _tracks.Clear();
        ClearAudioRangesUi();
        Canvas.Clear();
        Timeline.SetVideo(null, _fps, 0);
        UpdateTime(0);
        RefreshPreviewTransport();
        RefreshTargetEmpty();
        RefreshProcessUi();
    }

    private void RefreshPreviewTransport()
    {
        var ready = !string.IsNullOrEmpty(_openedPath);
        PlayBtn.IsEnabled = ready;
        PreviewMuteBtn.IsEnabled = ready;
        Timeline.IsEnabled = ready;
        AudioMuteBtn.IsEnabled = ready;
        AudioBeepBtn.IsEnabled = ready;
        AudioDeleteBtn.IsEnabled = ready && Timeline.SelectedAudioId > 0;
        TimeCurrent.Opacity = ready ? 1 : 0.35;
        TimeTotal.Opacity = ready ? 1 : 0.35;
    }

    private void ClearAudioRangesUi()
    {
        _audioRanges.Clear();
        _localAudioId = 1;
        _engine.ClearAudioRanges();
        Timeline.ClearAudioSelection();
        AudioDeleteBtn.IsEnabled = false;
    }

    private void OnAudioMute(object sender, RoutedEventArgs e)
    {
        AudioMuteBtn.IsChecked = true;
        AudioBeepBtn.IsChecked = false;
        Timeline.NewRangeEffect = 0;
        ApplyEffectToSelected(0);
    }

    private void OnAudioBeep(object sender, RoutedEventArgs e)
    {
        AudioBeepBtn.IsChecked = true;
        AudioMuteBtn.IsChecked = false;
        Timeline.NewRangeEffect = 1;
        ApplyEffectToSelected(1);
    }

    private void ApplyEffectToSelected(int effect)
    {
        var id = Timeline.SelectedAudioId;
        var mark = _audioRanges.FirstOrDefault(r => r.Id == id);
        if (mark == null)
            return;
        mark.Effect = effect;
        _engine.UpdateAudioRange(mark.Id, mark.StartSec, mark.EndSec, effect);
        SyncPreviewRedaction(_currentFrame);
    }

    private void OnAudioDelete(object sender, RoutedEventArgs e)
    {
        var id = Timeline.SelectedAudioId;
        if (id > 0)
            RemoveAudioRange(id);
    }

    private void OnAudioRangeCreated(double startSec, double endSec, int effect)
    {
        var id = _engine.AddAudioRange(startSec, endSec, effect);
        if (id <= 0)
            id = _localAudioId++;
        var mark = new AudioRangeMark
        {
            Id = id,
            StartSec = startSec,
            EndSec = endSec,
            Effect = effect,
            Selected = true
        };
        foreach (var r in _audioRanges)
            r.Selected = false;
        _audioRanges.Add(mark);
        Timeline.SetSelectedAudio(id);
        AudioDeleteBtn.IsEnabled = true;
        SyncPreviewRedaction(_currentFrame);
    }

    private void OnAudioRangeChanged(int id, double startSec, double endSec)
    {
        var mark = _audioRanges.FirstOrDefault(r => r.Id == id);
        if (mark == null)
            return;
        mark.StartSec = startSec;
        mark.EndSec = endSec;
        _engine.UpdateAudioRange(id, startSec, endSec, mark.Effect);
        SyncPreviewRedaction(_currentFrame);
    }

    private void OnAudioRangeSelected(int id)
    {
        foreach (var r in _audioRanges)
            r.Selected = r.Id == id;
        Timeline.SetSelectedAudio(id);
        AudioDeleteBtn.IsEnabled = !string.IsNullOrEmpty(_openedPath) && id > 0;
        if (id <= 0)
            return;
        var mark = _audioRanges.FirstOrDefault(r => r.Id == id);
        if (mark == null)
            return;
        if (mark.Effect == 1)
        {
            AudioBeepBtn.IsChecked = true;
            AudioMuteBtn.IsChecked = false;
            Timeline.NewRangeEffect = 1;
        }
        else
        {
            AudioMuteBtn.IsChecked = true;
            AudioBeepBtn.IsChecked = false;
            Timeline.NewRangeEffect = 0;
        }
    }

    private void RemoveAudioRange(int id)
    {
        if (id <= 0)
            return;
        _engine.RemoveAudioRange(id);
        var mark = _audioRanges.FirstOrDefault(r => r.Id == id);
        if (mark != null)
            _audioRanges.Remove(mark);
        Timeline.ClearAudioSelection();
        AudioDeleteBtn.IsEnabled = false;
        SyncPreviewRedaction(_currentFrame);
    }

    private void ScheduleOpen(string path)
    {
        SelectPath(path);
        Dispatcher.BeginInvoke(() =>
        {
            if (FileList.SelectedItem is FileRow row &&
                string.Equals(row.Path, path, StringComparison.OrdinalIgnoreCase))
                OpenVideo(path);
        }, DispatcherPriority.Background);
    }

    private List<string> EnqueuePaths(IEnumerable<string> paths)
    {
        var added = new List<string>();
        foreach (var raw in paths)
        {
            foreach (var path in ExpandVideos(raw))
            {
                if (_files.Any(f => string.Equals(f.Path, path, StringComparison.OrdinalIgnoreCase)))
                    continue;
                _files.Add(new FileRow(path));
                added.Add(path);
            }
        }
        RefreshFileSummary();
        RefreshProcessUi();
        return added;
    }

    private void HandleDroppedPaths(DragEventArgs e)
    {
        if (e.Data.GetData(DataFormats.FileDrop) is string[] files)
            HandleDroppedPaths(files);
    }

    private void HandleDroppedPaths(IEnumerable<string> raw)
    {
        var inputs = raw.ToList();
        if (inputs.Count == 0)
            return;
        var hasDir = inputs.Any(Directory.Exists);
        var added = EnqueuePaths(inputs);
        if (added.Count == 1 && !hasDir && inputs.Count == 1)
            ScheduleOpen(added[0]);
    }

    private static IEnumerable<string> ExpandVideos(string path)
    {
        if (Directory.Exists(path))
        {
            foreach (var file in Directory.EnumerateFiles(path, "*.*", SearchOption.TopDirectoryOnly))
            {
                if (IsVideo(file))
                    yield return Path.GetFullPath(file);
            }
            yield break;
        }
        if (File.Exists(path) && IsVideo(path))
            yield return Path.GetFullPath(path);
    }

    private static bool IsVideo(string path) =>
        VideoExt.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);

    private void SelectPath(string path)
    {
        FileList.SelectedItem = _files.FirstOrDefault(f =>
            string.Equals(f.Path, path, StringComparison.OrdinalIgnoreCase));
    }

    private void RefreshFileSummary()
    {
        if (_files.Count == 0)
        {
            FileSummary.Text = L.T("dropHere");
            return;
        }
        var waiting = _files.Count(f => f.Status == "Waiting");
        var done = _files.Count(f => f.Status == "Done");
        var failed = _files.Count(f => f.Status == "Failed");
        FileSummary.Text = L.F("fileSummary", _files.Count, waiting, done, failed);
    }

    private void ApplyDetect()
    {
        if (DetectCombo == null || KeepPlatesCheck == null)
            return;
        if (_reverse)
        {
            _engine.SetFaces(true);
            _engine.SetYolo(KeepPlatesCheck.IsChecked == true);
            return;
        }
        var index = DetectCombo.SelectedIndex;
        _engine.SetFaces(index != 2);
        _engine.SetYolo(index != 1);
    }

    private void OnDetectChanged(object sender, SelectionChangedEventArgs e) => ApplyDetect();

    private void OnKeepPlatesChanged(object sender, RoutedEventArgs e)
    {
        if (_reverse)
            ApplyDetect();
    }

    private void OnFileListDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop)
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void OnFileListDrop(object sender, DragEventArgs e)
    {
        e.Handled = true;
        if (e.Data.GetData(DataFormats.FileDrop) is string[] files)
            EnqueuePaths(files);
    }

    private void OnFileDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is not DependencyObject source)
            return;
        var item = ItemsControl.ContainerFromElement(FileList, source) as ListBoxItem;
        if (item?.Content is not FileRow row)
            return;
        OpenVideo(row.Path);
    }

    private void OnAddFiles(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = VideoFilter(),
            Multiselect = true
        };
        if (dlg.ShowDialog(this) == true)
            EnqueuePaths(dlg.FileNames);
    }

    private void OnRemoveFile(object sender, RoutedEventArgs e)
    {
        if (FileList.SelectedItem is not FileRow row)
            return;
        var wasOpen = string.Equals(_openedPath, row.Path, StringComparison.OrdinalIgnoreCase);
        _files.Remove(row);
        RefreshFileSummary();
        RefreshProcessUi();
        if (wasOpen)
            ClosePreview();
    }

    private void OnClearDone(object sender, RoutedEventArgs e)
    {
        var opened = _openedPath;
        foreach (var row in _files.Where(f => f.Status is "Done" or "Cancelled").ToList())
            _files.Remove(row);
        RefreshFileSummary();
        RefreshProcessUi();
        if (!string.IsNullOrEmpty(opened) &&
            !_files.Any(f => string.Equals(f.Path, opened, StringComparison.OrdinalIgnoreCase)))
            ClosePreview();
    }

    private void OnClearAll(object sender, RoutedEventArgs e)
    {
        _files.Clear();
        RefreshFileSummary();
        ClosePreview();
    }

    private void OnOpen(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Filter = VideoFilter()
        };
        if (dlg.ShowDialog(this) == true)
        {
            EnqueuePaths([dlg.FileName]);
            ScheduleOpen(dlg.FileName);
        }
    }

    private void OnProcess(object sender, RoutedEventArgs e)
    {
        if (_batchRunning)
            return;
        if (_reverse)
            ProcessKeepVideo();
        else
            ProcessRedactQueue();
    }

    private void ProcessKeepVideo()
    {
        if (!HasKeptPeople())
        {
            AppDialog.Show(this, L.T("msgKeepPeople"), L.T("msgClickFace"));
            return;
        }
        var sourcePath = _openedPath ?? (FileList.SelectedItem as FileRow)?.Path;
        if (string.IsNullOrEmpty(sourcePath))
        {
            AppDialog.Show(this, L.T("msgKeepPeople"), L.T("msgOpenFirst"));
            return;
        }
        if (!OpenVideo(sourcePath))
            return;
        var dest = PickSavePath(sourcePath);
        if (dest == null)
            return;
        StartSingleExport(sourcePath, dest, L.T("processThis"));
    }

    private void ProcessRedactQueue()
    {
        var queue = RunnableFiles();
        if (queue.Count == 0)
        {
            AppDialog.Show(this, L.T("msgRedactAll"), L.T("msgAddFirst"));
            return;
        }
        if (queue.Count > 1 && !_license.Current.AllowBatch)
        {
            if (AppDialog.Confirm(this, L.T("msgBatch"),
                    L.F("msgBatchLimit", _license.Current.LocalizedName),
                    ok: L.T("dlgUpgrade"), cancel: L.T("dlgClose")))
                _license.OpenPricing();
            return;
        }
        if (queue.Count == 1)
        {
            var dest = PickSavePath(queue[0].Path);
            if (dest == null)
                return;
            StartSingleExport(queue[0].Path, dest, L.T("msgProcessAll"));
            return;
        }
        if (!AppDialog.Confirm(this, L.T("msgProcessAll"),
                L.F("msgProcessConfirm", queue.Count)))
            return;
        _batch = queue;
        _batchIndex = 0;
        _batchRunning = true;
        RefreshProcessUi();
        _engine.Pause();
        _previewAudio.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
        CloseExportProgressDialog();
        _exportDlg = new ProgressDialog { Owner = this };
        _exportDlg.SetTitle(L.T("msgProcessAll"));
        _exportDlg.CancelRequested += OnBatchCancel;
        if (!StartCurrentBatchExport())
        {
            _batchRunning = false;
            _batch = null;
            _exportDlg.AllowClose();
            _exportDlg.Close();
            _exportDlg = null;
            RefreshProcessUi();
            AppDialog.Show(this, L.T("msgProcessAll"), L.T("msgStartFail"));
            return;
        }
        _exportDlg.ShowDialog();
    }

    private void OnBatchCancel()
    {
        _batchRunning = false;
        _engine.CancelExport();
        _exportDlg?.SetCancelling();
    }

    private bool StartCurrentBatchExport()
    {
        if (_batch == null)
            return false;
        while (_batchIndex < _batch.Count)
        {
            if (!_batchRunning)
                return false;
            var row = _batch[_batchIndex];
            _exportDlg?.SetProgress(
                (int)Math.Round(_batchIndex * 100.0 / Math.Max(1, _batch.Count)),
                $"{_batchIndex + 1} / {_batch.Count}  {row.FileName}");
            if (!OpenVideo(row.Path, quiet: true))
            {
                row.Status = "Failed";
                _batchIndex++;
                continue;
            }
            if (LengthBlockMessage() != null)
            {
                row.Status = "Too long";
                _batchIndex++;
                continue;
            }
            var dest = UniqueRedactedPath(row.Path);
            row.Status = "Processing";
            RefreshFileSummary();
            if (_engine.Export(dest, _license.Current.Watermark))
                return true;
            row.Status = "Failed";
            _batchIndex++;
        }
        return false;
    }

    private void StartSingleExport(string sourcePath, string dest, string title)
    {
        if (!OpenVideo(sourcePath))
            return;
        var tooLong = LengthBlockMessage();
        if (tooLong != null)
        {
            AppDialog.Show(this, title, tooLong);
            return;
        }
        _engine.Pause();
        _previewAudio.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
        if (!_engine.Export(dest, _license.Current.Watermark))
        {
            AppDialog.Show(this, title, L.T("msgExportStartFail"));
            return;
        }
        var row = _files.FirstOrDefault(f =>
            string.Equals(f.Path, sourcePath, StringComparison.OrdinalIgnoreCase));
        if (row != null)
            row.Status = "Processing";
        RefreshFileSummary();
        ShowExportProgressDialog(title);
    }

    private string? PickSavePath(string sourcePath)
    {
        var dlg = new SaveFileDialog
        {
            Filter = L.T("filterMp4") + "|*.mp4",
            FileName = SuggestedRedactedName(sourcePath)
        };
        var dir = Path.GetDirectoryName(sourcePath);
        if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
            dlg.InitialDirectory = dir;
        return dlg.ShowDialog(this) == true ? dlg.FileName : null;
    }

    private static string SuggestedRedactedName(string? sourcePath)
    {
        if (string.IsNullOrEmpty(sourcePath))
            return "redacted.mp4";
        var baseName = Path.GetFileNameWithoutExtension(sourcePath);
        return string.IsNullOrEmpty(baseName) ? "redacted.mp4" : $"{baseName}_redacted.mp4";
    }

    private static string UniqueRedactedPath(string sourcePath)
    {
        var dir = Path.GetDirectoryName(sourcePath) ?? "";
        var baseName = Path.GetFileNameWithoutExtension(sourcePath);
        if (string.IsNullOrEmpty(baseName))
            baseName = "redacted";
        var dest = Path.Combine(dir, $"{baseName}_redacted.mp4");
        for (var i = 2; File.Exists(dest); i++)
            dest = Path.Combine(dir, $"{baseName}_redacted_{i}.mp4");
        return dest;
    }

    private List<FileRow> RunnableFiles() =>
        _files.Where(f => f.Status is "Waiting" or "Failed" or "Cancelled").ToList();

    private void ShowExportProgressDialog(string title)
    {
        CloseExportProgressDialog();
        _exportDlg = new ProgressDialog { Owner = this };
        _exportDlg.SetTitle(title);
        _exportDlg.CancelRequested += () => _engine.CancelExport();
        _exportDlg.ShowDialog();
    }

    private void CloseExportProgressDialog()
    {
        if (_exportDlg == null)
            return;
        var dlg = _exportDlg;
        _exportDlg = null;
        dlg.AllowClose();
        dlg.Close();
        Activate();
    }

    private void OnExportFinished(bool ok, string path)
    {
        if (ok && !string.IsNullOrEmpty(path) && File.Exists(path))
            ok = FinishAudioOnExport(path, out path);

        var cancelled = !ok && path.Contains("cancel", StringComparison.OrdinalIgnoreCase);
        if (_batch != null)
        {
            if (_batchIndex < _batch.Count)
                _batch[_batchIndex].Status = ok ? "Done" : cancelled ? "Cancelled" : "Failed";
            RefreshFileSummary();
            if (!cancelled && _batchRunning)
            {
                _batchIndex++;
                if (StartCurrentBatchExport())
                    return;
            }
            var done = _batch.Count(f => f.Status == "Done");
            var failed = _batch.Count(f => f.Status == "Failed");
            var skipped = _batch.Count(f => f.Status == "Cancelled");
            var tooLong = _batch.Count(f => f.Status == "Too long");
            _batch = null;
            _batchRunning = false;
            CloseExportProgressDialog();
            RefreshProcessUi();
            var parts = new List<string> { L.F("msgProcessed", done) };
            if (failed > 0)
                parts.Add(L.F("msgFailedN", failed));
            if (skipped > 0)
                parts.Add(L.F("msgCancelledN", skipped));
            if (tooLong > 0)
                parts.Add(L.F("msgTooLongN", tooLong, _license.Current.MaxMinutes));
            AppDialog.Show(this, L.T("msgProcessAll"),
                string.Join(L.T("msgSummarySep"), parts) + L.T("msgSummaryEnd"));
            return;
        }

        CloseExportProgressDialog();
        var row = _files.FirstOrDefault(f => f.Status == "Processing")
            ?? FileList.SelectedItem as FileRow;
        if (row != null)
        {
            row.Status = ok ? "Done" : cancelled ? "Cancelled" : "Failed";
            RefreshFileSummary();
        }
        RefreshProcessUi();
        if (ok)
        {
            var finished = _license.Current.Watermark
                ? L.T("msgFinishedMark")
                : L.T("msgFinished");
            AppDialog.Show(this, L.T("msgProcess"), finished, path);
        }
        else
            AppDialog.Show(this, L.T("msgProcess"), cancelled ? L.T("msgCancelled") : path);
    }

    /// <summary>
    /// When the core DLL has no audio API yet, burn mute/beep ranges with ffmpeg after export.
    /// </summary>
    private bool FinishAudioOnExport(string outputPath, out string pathOrError)
    {
        pathOrError = outputPath;
        if (_audioRanges.Count == 0 || string.IsNullOrEmpty(_openedPath))
            return true;
        if (_engine.AudioApiAvailable)
            return true;

        var ffmpeg = FfmpegAudioRemux.FindFfmpeg();
        if (ffmpeg == null)
        {
            pathOrError = L.T("msgAudioFfmpegMissing");
            return false;
        }

        var temp = outputPath + ".aud.tmp.mp4";
        try
        {
            if (File.Exists(temp))
                File.Delete(temp);
            if (!FfmpegAudioRemux.Remux(ffmpeg, _openedPath, outputPath, temp, _audioRanges.ToList(),
                    out var err))
            {
                pathOrError = string.IsNullOrWhiteSpace(err) ? L.T("msgAudioRemuxFail") : err;
                return false;
            }
            File.Delete(outputPath);
            File.Move(temp, outputPath);
            pathOrError = outputPath;
            return true;
        }
        catch (Exception ex)
        {
            pathOrError = ex.Message;
            return false;
        }
        finally
        {
            try
            {
                if (File.Exists(temp))
                    File.Delete(temp);
            }
            catch
            {
                // ignore
            }
        }
    }

    private void OnPlayPause(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(_openedPath))
            return;
        if (_playing)
        {
            _engine.Pause();
            _previewAudio.Pause();
            _playing = false;
            PlayBtn.Content = "▶";
            Timeline.SetPosition(_currentFrame, follow: false, loadDetail: true);
        }
        else
        {
            _previewAudio.PlayFromFrame(_currentFrame);
            _engine.Play();
            _playing = true;
            PlayBtn.Content = "❚❚";
            SyncPreviewRedaction(_currentFrame);
        }
    }

    private void OnPreviewMute(object sender, RoutedEventArgs e)
    {
        var muted = PreviewMuteBtn.IsChecked == true;
        _previewAudio.IsMuted = muted;
        // Speaker / Mute glyphs (Segoe MDL2)
        PreviewMuteBtn.Content = muted ? "\uE74F" : "\uE767";
        SyncPreviewRedaction(_currentFrame);
    }

    private void OnTimelineSeek(long frame)
    {
        _currentFrame = Math.Clamp(frame, 0, Math.Max(0, _frameCount - 1));
        _engine.Seek(_currentFrame);
        _previewAudio.SeekFrame(_currentFrame);
        SyncPreviewRedaction(_currentFrame);
    }

    private void OnTimelinePosition(long frame)
    {
        _currentFrame = frame;
        UpdateTime(frame);
        if (!_playing)
            SyncPreviewRedaction(frame);
    }

    private void OnTimelineScrubStarted()
    {
        if (!_playing)
            return;
        _engine.Pause();
        _previewAudio.Pause();
        _playing = false;
        PlayBtn.Content = "▶";
    }

    private void OnRectDrawn(Int32Rect r)
    {
        if (r.Width < 2 || r.Height < 2)
            return;
        if (_follow)
            _engine.AddManual(r.X, r.Y, r.Width, r.Height);
        else
            _engine.AddStatic(r.X, r.Y, r.Width, r.Height, _currentFrame, -1);
    }

    private void OnCanvasClick(int x, int y)
    {
        if (!_reverse)
            return;
        _engine.EnrollKeepAt(x, y);
    }

    private void OnToolFollow(object sender, RoutedEventArgs e)
    {
        if (FollowBtn.IsChecked == true)
            PinBtn.IsChecked = false;
        SyncDrawMode();
    }

    private void OnToolPin(object sender, RoutedEventArgs e)
    {
        if (PinBtn.IsChecked == true)
            FollowBtn.IsChecked = false;
        SyncDrawMode();
    }

    private void SyncDrawMode()
    {
        if (!_reverse && FollowBtn.IsChecked != true && PinBtn.IsChecked != true)
            FollowBtn.IsChecked = true;
        var pin = PinBtn.IsChecked == true;
        var follow = FollowBtn.IsChecked == true;
        _follow = follow || !pin;
        Canvas.DrawEnabled = follow || pin;
        Canvas.FollowTool = _follow;
        Canvas.DrawColor = pin
            ? Color.FromRgb(0xC8, 0x4D, 0xFF)
            : Color.FromRgb(0x4C, 0x8D, 0xFF);
        Canvas.Cursor = Canvas.DrawEnabled ? Cursors.Cross : Cursors.Arrow;
    }

    private void OnJobRedact(object sender, RoutedEventArgs e)
    {
        if (!ConfirmLeaveKeep())
        {
            JobRedactBtn.IsChecked = false;
            JobKeepBtn.IsChecked = true;
            return;
        }
        SetReverse(false);
    }

    private void OnJobKeep(object sender, RoutedEventArgs e) => SetReverse(true);

    private bool ConfirmLeaveKeep()
    {
        if (!_reverse || !HasKeptPeople())
            return true;
        return AppDialog.Confirm(this, L.T("msgSwitchRedact"), L.T("msgDiscardKeep"));
    }

    private bool HasKeptPeople() => _tracks.Any(t => t.MatchTier is 1 or 2);

    private void SetReverse(bool reverse)
    {
        if (!reverse && _reverse)
        {
            foreach (var row in _tracks)
                _engine.SetTrackEnabled(row.Id, true);
            _engine.ClearGallery();
        }
        _reverse = reverse;
        _engine.SetFacePolicy(reverse ? 1 : 0);
        JobRedactBtn.IsChecked = !reverse;
        JobKeepBtn.IsChecked = reverse;
        DetectPanel.Visibility = reverse ? Visibility.Collapsed : Visibility.Visible;
        KeepPlatesCheck.Visibility = reverse ? Visibility.Visible : Visibility.Collapsed;
        if (reverse)
        {
            FollowBtn.IsChecked = false;
            PinBtn.IsChecked = false;
        }
        else if (FollowBtn.IsChecked != true && PinBtn.IsChecked != true)
        {
            FollowBtn.IsChecked = true;
        }
        MaskHeader.Visibility = reverse ? Visibility.Visible : Visibility.Collapsed;
        SyncDrawMode();
        ApplyJobCopy();
        foreach (var row in _tracks)
            row.SetJobPresentation(reverse);
        _trackView.Refresh();
        _maskSource.View.Refresh();
        ApplyDetect();
        RuntimeStatus.Text = _engine.RuntimeStatus();
        RefreshTargetEmpty();
        RefreshProcessUi();
    }

    private void RefreshProcessUi()
    {
        JobRedactBtn.IsEnabled = !_batchRunning;
        JobKeepBtn.IsEnabled = !_batchRunning;
        if (_reverse)
        {
            var ready = !string.IsNullOrEmpty(_openedPath) && HasKeptPeople();
            ProcessBtn.Content = L.T("processThis");
            ProcessBtn.IsEnabled = ready && !_batchRunning;
            ProcessBtn.ToolTip = ready ? L.T("tipKeepReady") : L.T("tipNeedFace");
            ProcessHint.Text = ready ? "" : L.T("hintClickFace");
            return;
        }
        var n = RunnableFiles().Count;
        ProcessBtn.Content = n > 1 ? L.F("processAllN", n) : L.T("processAll");
        ProcessBtn.IsEnabled = n > 0 && !_batchRunning;
        ProcessBtn.ToolTip = n > 0 ? L.T("tipExport") : L.T("tipAdd");
        if (n > 1 && !_license.Current.AllowBatch)
            ProcessHint.Text = L.T("hintOneAtATime");
        else if (_license.Current.Watermark && n > 0)
            ProcessHint.Text = L.T("hintWatermark");
        else
            ProcessHint.Text = "";
    }

    private string? LengthBlockMessage()
    {
        var max = _license.Current.MaxMinutes;
        if (max <= 0)
            return null;
        var seconds = _fps > 0.1 ? _frameCount / _fps : 0;
        if (seconds <= max * 60.0 + 1.0)
            return null;
        var shown = Math.Max(1, (int)Math.Ceiling(seconds / 60.0));
        return L.F("msgTooLong", shown, _license.Current.LocalizedName, max);
    }

    private void RefreshPlanUi()
    {
        var plan = _license.Current;
        PlanBtn.Content = plan.LocalizedName;
        PlanBtn.ToolTip = plan.IsPaid
            ? plan.LocalizedName + (string.IsNullOrWhiteSpace(plan.Email) ? "" : $" — {plan.Email}")
            : L.T("planFreeTip");
        ApplyAccountChip(plan);
        SignOutItem.Header = plan.IsSignedIn ? L.T("menuSignOut") : L.T("menuSignIn");
        SignOutItem.IsEnabled = !_signingIn;
    }

    private void ApplyAccountChip(Entitlements plan)
    {
        var signedIn = plan.IsSignedIn;
        var name = signedIn ? ShortAccountName(plan) : L.T("accountChipGuest");
        AccountName.Text = name;
        AccountName.Foreground = new SolidColorBrush(signedIn
            ? Color.FromRgb(0xE8, 0xEC, 0xF1)
            : Color.FromRgb(0x8B, 0x95, 0xA5));
        AccountAvatar.Fill = new SolidColorBrush(signedIn
            ? Color.FromRgb(0xE8, 0xA8, 0x38)
            : Color.FromRgb(0x24, 0x30, 0x41));
        AccountGuestIcon.Visibility = signedIn ? Visibility.Collapsed : Visibility.Visible;
        AccountInitial.Visibility = signedIn ? Visibility.Visible : Visibility.Collapsed;
        AccountInitial.Text = signedIn ? AccountInitialOf(name) : "";
        var email = plan.Email?.Trim();
        AccountChip.ToolTip = signedIn
            ? (string.IsNullOrEmpty(email) ? plan.LocalizedName : email)
            : L.T("accountGuest");
    }

    private static string ShortAccountName(Entitlements plan)
    {
        var email = plan.Email?.Trim();
        if (string.IsNullOrEmpty(email))
            return L.T("accountChipUser");
        var at = email.IndexOf('@');
        return at > 0 ? email[..at] : email;
    }

    private static string AccountInitialOf(string name)
    {
        var trimmed = name.Trim();
        if (trimmed.Length == 0)
            return "?";
        return char.ToUpperInvariant(trimmed[0]).ToString();
    }

    private void OnAccount(object sender, RoutedEventArgs e)
    {
        var dlg = new AccountWindow(_license) { Owner = this };
        dlg.ShowDialog();
        RefreshPlanUi();
        RefreshProcessUi();
    }

    private void OnUpgrade(object sender, RoutedEventArgs e) => _license.OpenPricing();

    private async void OnAccountSession(object sender, RoutedEventArgs e)
    {
        if (_license.Current.IsSignedIn)
        {
            if (!AppDialog.Confirm(this, L.T("dlgSignOut"), L.T("msgSignOutBody"), ok: L.T("dlgSignOut")))
                return;
            _license.SignOut();
            RefreshPlanUi();
            RefreshProcessUi();
            return;
        }

        if (_signingIn)
            return;
        _signIn?.Cancel();
        _signIn = new CancellationTokenSource();
        _signingIn = true;
        RefreshPlanUi();
        try
        {
            var result = await _license.SignInAsync(_signIn.Token);
            if (result.Activated)
            {
                try { await _license.RefreshOnlineAsync(_signIn.Token); }
                catch (OperationCanceledException) { }
            }
            else if (IsLoaded
                     && !string.Equals(result.Message, L.T("authCancelled"), StringComparison.Ordinal))
            {
                AppDialog.Show(this, L.T("accountTitle"), result.Message);
            }
        }
        catch (OperationCanceledException)
        {
        }
        finally
        {
            _signingIn = false;
            _signIn = null;
            if (IsLoaded)
            {
                RefreshPlanUi();
                RefreshProcessUi();
            }
        }
    }

    private void RefreshTargetEmpty()
    {
        if (_reverse)
        {
            TargetEmpty.Text = L.T("emptyKeep");
            TargetEmpty.Visibility = _tracks.Any(t => t.IsKeepEntry)
                ? Visibility.Collapsed : Visibility.Visible;
            var anyMask = _tracks.Any(t => t.IsMaskEntry);
            MaskEmpty.Visibility = anyMask ? Visibility.Collapsed : Visibility.Visible;
            return;
        }
        TargetEmpty.Text = L.T("emptyDetect");
        TargetEmpty.Visibility = _tracks.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void OnConfirmIdentity(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: int id })
            _engine.ConfirmIdentity(id);
    }

    private void OnRejectIdentity(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: int id })
            _engine.RejectIdentity(id);
    }

    private void OnTrackCheck(object sender, RoutedEventArgs e)
    {
        if (sender is CheckBox { DataContext: TrackRow row })
            _engine.SetTrackEnabled(row.Id, row.Enabled);
    }

    private void OnModeMosaic(object sender, RoutedEventArgs e) => SetMode(0);
    private void OnModeBlur(object sender, RoutedEventArgs e) => SetMode(1);
    private void OnModeSolid(object sender, RoutedEventArgs e) => SetMode(2);

    private void SetMode(int mode)
    {
        MosaicItem.IsChecked = mode == 0;
        BlurItem.IsChecked = mode == 1;
        SolidItem.IsChecked = mode == 2;
        _engine.SetMode(mode);
    }

    private void OnSettings(object sender, RoutedEventArgs e)
    {
        var gpu = AppDialog.Ask(this, L.T("msgSettings"), L.T("msgSettingsBody"),
            yes: L.T("dlgUseGpu"), no: L.T("dlgUseCpu"));
        _engine.SetGpu(gpu);
        RuntimeStatus.Text = _engine.RuntimeStatus();
    }

    private static string AppVersionText()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "version.ini");
        if (!File.Exists(path))
            return "";
        foreach (var line in File.ReadLines(path))
        {
            var text = line.Trim();
            if (!text.StartsWith("version", StringComparison.OrdinalIgnoreCase))
                continue;
            var split = text.IndexOf('=');
            if (split < 0)
                continue;
            return text[(split + 1)..].Trim();
        }
        return "";
    }

    private void OnAbout(object sender, RoutedEventArgs e)
    {
        var version = AppVersionText();
        var body = string.IsNullOrEmpty(version)
            ? L.T("msgAboutBody")
            : $"{L.T("msgVersion")} {version}\n\n{L.T("msgAboutBody")}";
        AppDialog.Show(this, L.T("msgAbout"), body, NativeEngine.HardwareId(), L.T("msgMachineId"),
            L.T("msgSourceCode"), "https://github.com/topworld87/VideoRedactionAPP");
    }

    private void OnLicenses(object sender, RoutedEventArgs e)
    {
        new LicensesWindow { Owner = this }.ShowDialog();
    }

    private void OnUserGuide(object sender, RoutedEventArgs e)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "help", "guide.html");
        if (!File.Exists(path))
        {
            AppDialog.Show(this, L.T("menuHelp"), L.T("msgGuideMissing"));
            return;
        }
        try
        {
            var uri = new Uri(path).AbsoluteUri + "#" + Uri.EscapeDataString(AppLocale.Code);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = uri,
                UseShellExecute = true
            });
        }
        catch
        {
            AppDialog.Show(this, L.T("menuHelp"), L.T("msgGuideMissing"));
        }
    }

    private void OnLanguagePress(object sender, MouseButtonEventArgs e)
    {
        if (!LangPopup.IsOpen)
            return;
        LangPopup.IsOpen = false;
        e.Handled = true;
    }

    private void OnLanguageClick(object sender, RoutedEventArgs e)
    {
        RebuildLanguageMenu();
        LangPopup.IsOpen = true;
    }

    private void RebuildLanguageMenu()
    {
        LangList.Children.Clear();
        foreach (var loc in AppLocale.All)
        {
            var selected = loc.Code == AppLocale.Code;
            var row = new Border
            {
                CornerRadius = new CornerRadius(8),
                Padding = new Thickness(10, 7, 10, 7),
                Margin = new Thickness(0, 1, 0, 1),
                Background = selected
                    ? new SolidColorBrush(Color.FromRgb(0x24, 0x30, 0x41))
                    : Brushes.Transparent,
                Cursor = Cursors.Hand
            };
            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var code = new TextBlock
            {
                Text = loc.Short,
                FontSize = 12,
                FontWeight = FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = new SolidColorBrush(selected
                    ? Color.FromRgb(0xE8, 0xA8, 0x38)
                    : Color.FromRgb(0x8B, 0x95, 0xA5))
            };
            var name = new TextBlock
            {
                Text = loc.Label,
                FontSize = 13,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xEC, 0xF1))
            };
            var mark = new TextBlock
            {
                Text = selected ? "✓" : "",
                Margin = new Thickness(12, 0, 2, 0),
                FontSize = 12,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xA8, 0x38))
            };
            Grid.SetColumn(name, 1);
            Grid.SetColumn(mark, 2);
            grid.Children.Add(code);
            grid.Children.Add(name);
            grid.Children.Add(mark);
            row.Child = grid;
            var idle = row.Background;
            row.MouseEnter += (_, _) =>
            {
                if (!selected)
                    row.Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x27, 0x33));
            };
            row.MouseLeave += (_, _) => row.Background = idle;
            var codeToSelect = loc.Code;
            row.MouseLeftButtonUp += (_, args) =>
            {
                args.Handled = true;
                LangPopup.IsOpen = false;
                AppLocale.Select(codeToSelect);
            };
            LangList.Children.Add(row);
        }
    }

    private void OnLicenseChanged()
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(OnLicenseChanged);
            return;
        }
        if (AccountChip == null)
            return;
        RefreshPlanUi();
    }

    private void OnLocaleChanged()
    {
        if (!IsLoaded && LangBtn == null)
            return;
        ApplyLocaleChrome();
    }

    private void ApplyLocaleChrome()
    {
        LangCodeText.Text = AppLocale.ShortLabel;
        ApplyJobCopy();
        RefreshFileSummary();
        RefreshTargetEmpty();
        RefreshProcessUi();
        RefreshPlanUi();
        foreach (var row in _files)
            row.NotifyLocale();
        foreach (var row in _tracks)
            row.NotifyLocale();
        foreach (var row in _audioRanges)
            row.RefreshLocale();
        Canvas.ApplyLocale();
        ApplyPreviewChrome();
        Timeline.InvalidateVisual();
    }

    private void OnPreviewMaximize(object sender, RoutedEventArgs e) =>
        SetPreviewExpanded(!_previewExpanded);

    private void SetPreviewExpanded(bool expanded)
    {
        _previewExpanded = expanded;
        var panels = expanded ? Visibility.Collapsed : Visibility.Visible;
        ProcessBar.Visibility = panels;
        FilePanel.Visibility = panels;
        TrackPanel.Visibility = panels;
        TimelineHost.Visibility = panels;
        PreviewToolBar.Visibility = panels;
        ApplyPreviewChrome();
    }

    private void ApplyPreviewChrome()
    {
        if (PreviewMaxBtn == null)
            return;
        PreviewMaxBtn.Content = _previewExpanded ? "\uE73F" : "\uE740";
        PreviewMaxBtn.ToolTip = L.T(_previewExpanded ? "previewRestore" : "previewMaximize");
    }

    private void ApplyJobCopy()
    {
        JobHint.Text = _reverse ? L.T("jobHintKeep") : L.T("jobHintRedact");
        TargetTitle.Text = _reverse ? L.T("keepClear") : L.T("detected");
        TargetHint.Text = _reverse ? L.T("targetKeep") : L.T("targetMask");
        ToolHint.Text = _reverse ? L.T("toolKeep") : L.T("toolMissed");
        MaskTitle.Text = L.T("maskTitle");
        MaskHint.Text = L.T("maskHint");
        MaskEmpty.Text = L.T("emptyMask");
        MaskToolLabel.Text = L.T("maskTool");
        MaskToolLabel.ToolTip = L.T("maskToolTip");
        MaskToolLabel.Visibility = _reverse ? Visibility.Visible : Visibility.Collapsed;
    }

    private static string VideoFilter() =>
        L.T("filterVideo") + "|*.mp4;*.avi;*.mov;*.mkv;*.wmv;*.m4v|" + L.T("filterAll") + "|*.*";

    private void OnExit(object sender, RoutedEventArgs e) => Close();
    private void OnMin(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void OnMax(object sender, RoutedEventArgs e) =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void OnCloseWin(object sender, RoutedEventArgs e) => Close();
}

public sealed class TrackRow : INotifyPropertyChanged
{
    private bool _enabled;
    private string _label;
    private int _matchTier;
    public TrackRow(int id, string label, bool enabled, int matchTier = 0, int kind = 0)
    {
        Id = id;
        Kind = kind;
        _label = label;
        _enabled = enabled;
        _matchTier = matchTier;
    }
    public int Id { get; }
    public int Kind { get; }
    public bool IsMaskEntry => Kind is 2 or 3;
    public Brush MaskBrush => Kind == 3 ? PinBrush : FollowBrush;
    private static readonly SolidColorBrush FollowBrush = Freeze(Color.FromRgb(0x4C, 0x8D, 0xFF));
    private static readonly SolidColorBrush PinBrush = Freeze(Color.FromRgb(0xC8, 0x4D, 0xFF));
    private static SolidColorBrush Freeze(Color color)
    {
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        return brush;
    }
    public string Label
    {
        get => L.Track(_label);
        set
        {
            if (_label == value)
                return;
            _label = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Label)));
        }
    }

    public void NotifyLocale() =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Label)));
    public int MatchTier
    {
        get => _matchTier;
        set
        {
            if (_matchTier == value)
                return;
            _matchTier = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(MatchTier)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(SuggestVisible)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsKeepEntry)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(KeepDotVisible)));
        }
    }
    public bool IsKeepEntry => _matchTier is 1 or 2 or 3;
    public Visibility KeepDotVisible =>
        _matchTier is 1 or 2 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility SuggestVisible => _matchTier == 3 ? Visibility.Visible : Visibility.Collapsed;
    private Visibility _maskCheckVisible = Visibility.Visible;
    private Visibility _keepLabelVisible = Visibility.Collapsed;
    public Visibility MaskCheckVisible
    {
        get => _maskCheckVisible;
        private set
        {
            if (_maskCheckVisible == value)
                return;
            _maskCheckVisible = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(MaskCheckVisible)));
        }
    }
    public Visibility KeepLabelVisible
    {
        get => _keepLabelVisible;
        private set
        {
            if (_keepLabelVisible == value)
                return;
            _keepLabelVisible = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(KeepLabelVisible)));
        }
    }
    public void SetJobPresentation(bool reverse)
    {
        MaskCheckVisible = reverse ? Visibility.Collapsed : Visibility.Visible;
        KeepLabelVisible = reverse ? Visibility.Visible : Visibility.Collapsed;
    }
    public bool Enabled
    {
        get => _enabled;
        set
        {
            if (_enabled == value)
                return;
            _enabled = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Enabled)));
        }
    }
    public event PropertyChangedEventHandler? PropertyChanged;
}

public sealed class FileRow : INotifyPropertyChanged
{
    private string _status = "Waiting";

    public FileRow(string path)
    {
        Id = Guid.NewGuid().ToString("N");
        Path = path;
        FileName = System.IO.Path.GetFileName(path);
    }

    public string Id { get; }
    public string Path { get; }
    public string FileName { get; }
    public string Status
    {
        get => _status;
        set
        {
            if (_status == value)
                return;
            _status = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Status)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusText)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusVisible)));
        }
    }

    public string StatusText => _status == "Waiting" ? "" : L.Status(_status);

    public Visibility StatusVisible =>
        _status == "Waiting" ? Visibility.Collapsed : Visibility.Visible;

    public void NotifyLocale() =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(StatusText)));

    public event PropertyChangedEventHandler? PropertyChanged;
}
