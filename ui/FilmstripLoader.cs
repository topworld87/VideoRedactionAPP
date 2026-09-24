using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace VideoBlackout.Wpf;

internal sealed class FilmThumb
{
    public FilmThumb(long frame, long span, ImageSource image)
    {
        Frame = frame;
        Span = span;
        Image = image;
    }

    public long Frame { get; }
    public long Span { get; }
    public ImageSource Image { get; }
}

/// <summary>
/// Builds a filmstrip with ffmpeg. Short clips are sampled in one pass.
/// Longer videos get a sparse strip first, then a denser window while zoomed.
/// </summary>
internal sealed class FilmstripLoader
{
    private readonly Dispatcher _ui;
    private readonly object _gate = new();
    private Process? _proc;
    private Task _chain = Task.CompletedTask;
    private string? _workDir;
    private string? _ffmpeg;
    private string? _video;
    private double _fps = 25;
    private int _gen;
    private int _detailTicket;
    private int _runningTicket = -1;
    private int _disposed;
    private long _haveStart;
    private long _haveEnd = -1;
    private long _haveSpan = -1;

    public FilmstripLoader(Dispatcher ui) => _ui = ui;

    public IReadOnlyList<FilmThumb> Coarse { get; private set; } = Array.Empty<FilmThumb>();
    public IReadOnlyList<FilmThumb> Detail { get; private set; } = Array.Empty<FilmThumb>();
    public string? Status { get; private set; }
    public event Action? Updated;

    public long CoarseSpan
    {
        get
        {
            var list = Coarse;
            if (list.Count == 0)
                return 0;
            var min = long.MaxValue;
            foreach (var thumb in list)
            {
                if (thumb.Span < min)
                    min = thumb.Span;
            }
            return min == long.MaxValue ? 0 : min;
        }
    }

    public void Start(string video, double fps, long frames)
    {
        Stop();
        if (Volatile.Read(ref _disposed) != 0 || frames <= 0 || !File.Exists(video))
            return;

        var ffmpeg = FindFfmpeg();
        if (ffmpeg == null)
        {
            Status = "Filmstrip needs ffmpeg.exe next to the app";
            Updated?.Invoke();
            return;
        }

        var gen = Interlocked.Increment(ref _gen);
        _ffmpeg = ffmpeg;
        _video = video;
        _fps = fps > 0.1 ? fps : 25;
        _workDir = Path.Combine(Path.GetTempPath(), "VideoBlackout", "strip", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_workDir);
        Status = "Loading thumbnails…";
        Updated?.Invoke();
        var dir = _workDir;
        Enqueue(gen, 0, () => RunCoarse(gen, ffmpeg, video, fps, frames, dir));
    }

    public void RequestDetail(long start, long end, long span)
    {
        if (Volatile.Read(ref _disposed) != 0 || span < 1 || end <= start || string.IsNullOrEmpty(_ffmpeg) || _workDir == null)
            return;
        if (CoarseSpan > 0 && span >= CoarseSpan)
            return;
        if (_haveSpan == span && start >= _haveStart && end <= _haveEnd && Detail.Count > 0)
            return;

        var ticket = Interlocked.Increment(ref _detailTicket);
        _haveStart = start;
        _haveEnd = end;
        _haveSpan = span;
        if (Volatile.Read(ref _runningTicket) > 0)
            TryKill();
        var gen = Volatile.Read(ref _gen);
        var ffmpeg = _ffmpeg;
        var dir = _workDir;
        Enqueue(gen, ticket, () => RunDetail(gen, ticket, ffmpeg!, dir!, start, end, span));
    }

    public void Stop()
    {
        Interlocked.Increment(ref _gen);
        Interlocked.Increment(ref _detailTicket);
        TryKill();
        Coarse = Array.Empty<FilmThumb>();
        Detail = Array.Empty<FilmThumb>();
        _haveSpan = -1;
        Status = null;
        var dir = _workDir;
        _workDir = null;
        DeleteLater(dir);
        Updated?.Invoke();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Interlocked.Increment(ref _gen);
        TryKill();
        var dir = _workDir;
        _workDir = null;
        var chain = _chain;
        _ = Task.Run(() =>
        {
            try { chain.Wait(8000); } catch { /* loader is going away */ }
            TryDelete(dir);
        });
    }

    private void Enqueue(int gen, int ticket, Action work)
    {
        _chain = _chain.ContinueWith(_ =>
        {
            if (!Alive(gen, ticket))
                return;
            Volatile.Write(ref _runningTicket, ticket);
            try { work(); }
            catch { /* one bad file should not kill the strip */ }
            finally
            {
                if (Volatile.Read(ref _runningTicket) == ticket)
                    Volatile.Write(ref _runningTicket, -1);
            }
        }, CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
    }

    private void RunCoarse(int gen, string ffmpeg, string video, double fps, long frames, string dir)
    {
        var coarseDir = Path.Combine(dir, "c");
        Directory.CreateDirectory(coarseDir);
        var raw = new List<RawThumb>();
        if (frames <= 600)
            ExtractPass(gen, 0, ffmpeg, video, fps, frames, coarseDir, raw);
        else
            ExtractSparse(gen, ffmpeg, video, fps, frames, coarseDir, raw);

        if (!Alive(gen, 0))
            return;
        Publish(gen, 0, raw, detail: false, emptyStatus: raw.Count == 0 ? "Could not read preview frames" : null);
    }

    private void ExtractPass(int gen, int ticket, string ffmpeg, string video, double fps, long frames,
        string dir, List<RawThumb> raw)
    {
        const int target = 80;
        var span = Math.Max(1, (frames + target - 1) / target);
        var count = (int)Math.Ceiling(frames / (double)span);
        var filter = span <= 1
            ? "scale=180:-2"
            : "fps=" + (fps / span).ToString("0.####", CultureInfo.InvariantCulture) + ",scale=180:-2";
        var pattern = Path.Combine(dir, "c_%04d.jpg");
        if (!Run(gen, ticket, ffmpeg, video, startSec: null, frameCount: null, duration: null, filter, pattern, timeoutMs: 120000))
            return;
        TakeFiles(dir, "c_*.jpg", 0, span, count, raw);
    }

    private void ExtractSparse(int gen, string ffmpeg, string video, double fps, long frames,
        string dir, List<RawThumb> raw)
    {
        const int count = 18;
        for (var i = 0; i < count; i++)
        {
            if (!Alive(gen, 0))
                return;
            var a = i * frames / count;
            var b = (i + 1) * frames / count;
            var span = Math.Max(1, b - a);
            var mid = Math.Min(frames - 1, a + span / 2);
            var file = Path.Combine(dir, $"c_{i:0000}.jpg");
            var sec = mid / Math.Max(0.1, fps);
            if (!Run(gen, 0, ffmpeg, video, sec, 1, null, "scale=180:-2", file, 20000))
                continue;
            if (!File.Exists(file))
                continue;
            raw.Add(new RawThumb(a, span, File.ReadAllBytes(file)));
            if (i == count - 1 || i % 4 == 3)
                Publish(gen, 0, raw.ToList(), detail: false, emptyStatus: null);
        }
    }

    private void RunDetail(int gen, int ticket, string ffmpeg, string root, long start, long end, long span)
    {
        var video = _video;
        if (string.IsNullOrEmpty(video))
            return;
        var dir = Path.Combine(root, "d");
        if (Directory.Exists(dir))
            Directory.Delete(dir, recursive: true);
        Directory.CreateDirectory(dir);
        var fps = _fps > 0.1 ? _fps : 25;
        var window = Math.Max(1, end - start);
        var count = (int)Math.Ceiling(window / (double)span);
        var startSec = start / fps;
        string filter;
        int? frameCount = null;
        double? duration = null;
        if (span <= 1)
        {
            filter = "scale=180:-2";
            frameCount = count;
        }
        else
        {
            filter = "fps=" + (fps / span).ToString("0.####", CultureInfo.InvariantCulture) + ",scale=180:-2";
            duration = window / fps;
            frameCount = count;
        }
        var pattern = Path.Combine(dir, "d_%04d.jpg");
        var raw = new List<RawThumb>();
        if (Run(gen, ticket, ffmpeg, video, startSec, frameCount, duration, filter, pattern, 60000))
            TakeFiles(dir, "d_*.jpg", start, span, count, raw);
        if (!Alive(gen, ticket))
            return;
        Publish(gen, ticket, raw, detail: true, emptyStatus: null);
    }

    private static void TakeFiles(string dir, string pattern, long origin, long span, int count, List<RawThumb> raw)
    {
        if (!Directory.Exists(dir))
            return;
        var files = new DirectoryInfo(dir).GetFiles(pattern)
            .OrderBy(f => f.Name, StringComparer.Ordinal)
            .Take(count)
            .ToArray();
        for (var i = 0; i < files.Length; i++)
        {
            byte[] bytes;
            try { bytes = File.ReadAllBytes(files[i].FullName); }
            catch { continue; }
            if (bytes.Length == 0)
                continue;
            raw.Add(new RawThumb(origin + i * span, span, bytes));
        }
    }

    private bool Run(int gen, int ticket, string ffmpeg, string video, double? startSec, int? frameCount,
        double? duration, string filter, string output, int timeoutMs)
    {
        if (!Alive(gen, ticket) || string.IsNullOrEmpty(video))
            return false;
        var psi = new ProcessStartInfo(ffmpeg)
        {
            CreateNoWindow = true,
            UseShellExecute = false,
            RedirectStandardError = true,
            RedirectStandardOutput = true
        };
        psi.ArgumentList.Add("-hide_banner");
        psi.ArgumentList.Add("-loglevel");
        psi.ArgumentList.Add("error");
        psi.ArgumentList.Add("-nostdin");
        psi.ArgumentList.Add("-y");
        if (startSec.HasValue)
        {
            psi.ArgumentList.Add("-ss");
            psi.ArgumentList.Add(startSec.Value.ToString("0.###", CultureInfo.InvariantCulture));
        }
        psi.ArgumentList.Add("-i");
        psi.ArgumentList.Add(video);
        psi.ArgumentList.Add("-map");
        psi.ArgumentList.Add("0:v:0");
        psi.ArgumentList.Add("-an");
        if (duration.HasValue)
        {
            psi.ArgumentList.Add("-t");
            psi.ArgumentList.Add(duration.Value.ToString("0.###", CultureInfo.InvariantCulture));
        }
        if (frameCount.HasValue)
        {
            psi.ArgumentList.Add("-frames:v");
            psi.ArgumentList.Add(frameCount.Value.ToString(CultureInfo.InvariantCulture));
        }
        psi.ArgumentList.Add("-vf");
        psi.ArgumentList.Add(filter);
        psi.ArgumentList.Add("-q:v");
        psi.ArgumentList.Add("6");
        psi.ArgumentList.Add(output);

        using var proc = new Process { StartInfo = psi };
        lock (_gate)
        {
            if (!Alive(gen, ticket))
                return false;
            _proc = proc;
        }
        try
        {
            if (!proc.Start())
                return false;
        }
        catch
        {
            return false;
        }
        try { proc.PriorityClass = ProcessPriorityClass.BelowNormal; } catch { /* ignore */ }

        var errTask = proc.StandardError.ReadToEndAsync();
        var outTask = proc.StandardOutput.ReadToEndAsync();
        if (!proc.WaitForExit(timeoutMs))
            TryKill();
        try { proc.WaitForExit(2000); } catch { /* ignore */ }
        try { Task.WaitAll([errTask, outTask], 2000); } catch { /* ignore */ }
        lock (_gate)
        {
            if (ReferenceEquals(_proc, proc))
                _proc = null;
        }
        return Alive(gen, ticket) && proc.ExitCode == 0;
    }

    private void Publish(int gen, int ticket, List<RawThumb> raw, bool detail, string? emptyStatus)
    {
        var snapshot = raw.ToArray();
        _ui.BeginInvoke(() =>
        {
            if (!Alive(gen, ticket))
                return;
            var thumbs = new List<FilmThumb>(snapshot.Length);
            foreach (var item in snapshot)
            {
                var image = Decode(item.Bytes);
                if (image == null)
                    continue;
                thumbs.Add(new FilmThumb(item.Frame, item.Span, image));
            }
            if (detail)
            {
                Detail = thumbs;
                if (thumbs.Count == 0)
                    _haveSpan = -1;
            }
            else
            {
                Coarse = thumbs;
                Status = thumbs.Count == 0 ? emptyStatus : null;
            }
            Updated?.Invoke();
        });
    }

    private static ImageSource? Decode(byte[] bytes)
    {
        try
        {
            var bmp = new BitmapImage();
            using var ms = new MemoryStream(bytes);
            bmp.BeginInit();
            bmp.CacheOption = BitmapCacheOption.OnLoad;
            bmp.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
            bmp.StreamSource = ms;
            bmp.DecodePixelWidth = 180;
            bmp.EndInit();
            bmp.Freeze();
            return bmp;
        }
        catch
        {
            return null;
        }
    }

    private bool Alive(int gen, int ticket) =>
        Volatile.Read(ref _gen) == gen &&
        Volatile.Read(ref _disposed) == 0 &&
        (ticket == 0 || Volatile.Read(ref _detailTicket) == ticket);

    private void TryKill()
    {
        Process? proc;
        lock (_gate)
            proc = _proc;
        if (proc == null)
            return;
        try
        {
            if (!proc.HasExited)
                proc.Kill(entireProcessTree: true);
        }
        catch { /* already gone */ }
    }

    private static void DeleteLater(string? dir)
    {
        if (string.IsNullOrEmpty(dir))
            return;
        _ = Task.Run(() =>
        {
            Thread.Sleep(400);
            TryDelete(dir);
        });
    }

    private static void TryDelete(string? dir)
    {
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
            return;
        try { Directory.Delete(dir, recursive: true); } catch { /* file still open */ }
    }

    private string? FindFfmpeg()
    {
        if (!string.IsNullOrEmpty(_ffmpeg) && File.Exists(_ffmpeg))
            return _ffmpeg;
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            var dir = new DirectoryInfo(start);
            for (var i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
            {
                var hit = FirstExisting(
                    Path.Combine(dir.FullName, "ffmpeg.exe"),
                    Path.Combine(dir.FullName, "tools", "ffmpeg", "ffmpeg.exe"),
                    Path.Combine(dir.FullName, "app", "tools", "ffmpeg", "ffmpeg.exe"));
                if (hit != null)
                    return _ffmpeg = hit;
                hit = FirstExtract(Path.Combine(dir.FullName, "app", "tools", "ffmpeg_extract"))
                      ?? FirstExtract(Path.Combine(dir.FullName, "tools", "ffmpeg_extract"));
                if (hit != null)
                    return _ffmpeg = hit;
            }
        }
        var pathEnv = Environment.GetEnvironmentVariable("PATH");
        if (!string.IsNullOrEmpty(pathEnv))
        {
            foreach (var part in pathEnv.Split(Path.PathSeparator))
            {
                var hit = Path.Combine(part.Trim(), "ffmpeg.exe");
                if (File.Exists(hit))
                    return _ffmpeg = hit;
            }
        }
        return null;
    }

    private static string? FirstExisting(params string[] paths)
    {
        foreach (var path in paths)
        {
            if (File.Exists(path))
                return path;
        }
        return null;
    }

    private static string? FirstExtract(string root)
    {
        if (!Directory.Exists(root))
            return null;
        foreach (var child in Directory.EnumerateDirectories(root))
        {
            var exe = Path.Combine(child, "bin", "ffmpeg.exe");
            if (File.Exists(exe))
                return exe;
        }
        return null;
    }

    private sealed class RawThumb
    {
        public RawThumb(long frame, long span, byte[] bytes)
        {
            Frame = frame;
            Span = span;
            Bytes = bytes;
        }

        public long Frame { get; }
        public long Span { get; }
        public byte[] Bytes { get; }
    }
}
