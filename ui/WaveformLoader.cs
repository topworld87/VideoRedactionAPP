using System.Diagnostics;
using System.IO;
using System.Windows.Threading;

namespace VideoBlackout.Wpf;

/// <summary>
/// Builds a CapCut-style amplitude envelope via ffmpeg (mono PCM peaks).
/// Does not load the whole soundtrack — streams and downsamples to a fixed peak array.
/// </summary>
internal sealed class WaveformLoader : IDisposable
{
    private const int SampleRate = 8000;
    private const int MaxBuckets = 6000;
    private const int MinBuckets = 240;

    private readonly Dispatcher _ui;
    private readonly object _gate = new();
    private Process? _proc;
    private CancellationTokenSource? _cts;
    private int _gen;
    private int _disposed;
    private float[] _peaks = Array.Empty<float>();

    public WaveformLoader(Dispatcher ui) => _ui = ui;

    public IReadOnlyList<float> Peaks
    {
        get
        {
            lock (_gate)
                return _peaks;
        }
    }

    public bool Ready { get; private set; }
    public bool HasAudio { get; private set; }
    public string? Status { get; private set; }
    public event Action? Updated;

    public void Start(string video, double durationSec)
    {
        Stop();
        if (Volatile.Read(ref _disposed) != 0 || string.IsNullOrEmpty(video) || !File.Exists(video))
            return;
        if (durationSec < 0.05)
            durationSec = 1;

        var ffmpeg = FfmpegAudioRemux.FindFfmpeg();
        if (ffmpeg == null)
        {
            Status = null;
            Ready = true;
            HasAudio = false;
            Updated?.Invoke();
            return;
        }

        var gen = Interlocked.Increment(ref _gen);
        var buckets = BucketCount(durationSec);
        var peaks = new float[buckets];
        Ready = false;
        HasAudio = false;
        Status = null;
        lock (_gate)
            _peaks = peaks;
        Updated?.Invoke();

        var cts = new CancellationTokenSource();
        _cts = cts;
        _ = Task.Run(() => Extract(gen, ffmpeg, video, durationSec, peaks, cts.Token), cts.Token);
    }

    public void Stop()
    {
        Interlocked.Increment(ref _gen);
        try
        {
            _cts?.Cancel();
        }
        catch
        {
            // ignore
        }
        _cts = null;
        TryKill();
        Ready = false;
        HasAudio = false;
        Status = null;
        lock (_gate)
            _peaks = Array.Empty<float>();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        Stop();
    }

    private void Extract(int gen, string ffmpeg, string video, double durationSec, float[] peaks,
        CancellationToken token)
    {
        var samplesPerBucket = Math.Max(1,
            (int)Math.Ceiling(SampleRate * durationSec / peaks.Length));
        var psi = new ProcessStartInfo
        {
            FileName = ffmpeg,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        // Low-rate mono PCM; ignore video. Missing audio → empty stdout / fail.
        foreach (var a in new[]
                 {
                     "-hide_banner", "-nostdin", "-v", "error",
                     "-i", video,
                     "-vn", "-ac", "1", "-ar", SampleRate.ToString(),
                     "-f", "s16le", "-acodec", "pcm_s16le",
                     "pipe:1"
                 })
            psi.ArgumentList.Add(a);

        Process? proc = null;
        try
        {
            proc = Process.Start(psi);
            if (proc == null)
            {
                Finish(gen, peaks, gotAudio: false);
                return;
            }
            lock (_gate)
                _proc = proc;

            // Drain stderr so the pipe cannot block.
            _ = proc.StandardError.BaseStream.CopyToAsync(Stream.Null, token);

            var buf = new byte[8192];
            var sampleAcc = 0;
            var bucket = 0;
            var peak = 0f;
            var any = false;
            var lastPublish = 0;
            var stdout = proc.StandardOutput.BaseStream;

            while (!token.IsCancellationRequested && bucket < peaks.Length)
            {
                var n = stdout.Read(buf, 0, buf.Length);
                if (n <= 0)
                    break;
                any = true;
                for (var i = 0; i + 1 < n && bucket < peaks.Length; i += 2)
                {
                    short s = (short)(buf[i] | (buf[i + 1] << 8));
                    var a = Math.Abs(s / 32768f);
                    if (a > peak)
                        peak = a;
                    sampleAcc++;
                    if (sampleAcc < samplesPerBucket)
                        continue;
                    peaks[bucket++] = peak;
                    sampleAcc = 0;
                    peak = 0f;
                    if (bucket - lastPublish >= 64 || bucket == peaks.Length)
                    {
                        lastPublish = bucket;
                        Publish(gen, peaks, partial: true, gotAudio: true);
                    }
                }
            }

            if (bucket < peaks.Length && peak > 0)
                peaks[bucket++] = peak;
            // Stretch remaining empty tail if decode ended early.
            if (bucket > 0 && bucket < peaks.Length)
            {
                var last = peaks[bucket - 1];
                for (var i = bucket; i < peaks.Length; i++)
                    peaks[i] = last * 0.15f;
            }

            try
            {
                proc.WaitForExit(15_000);
            }
            catch
            {
                // ignore
            }

            Finish(gen, peaks, gotAudio: any && bucket > 0);
        }
        catch (Exception)
        {
            if (Volatile.Read(ref _gen) == gen)
                Finish(gen, peaks, gotAudio: false);
        }
        finally
        {
            lock (_gate)
            {
                if (ReferenceEquals(_proc, proc))
                    _proc = null;
            }
            try
            {
                proc?.Dispose();
            }
            catch
            {
                // ignore
            }
        }
    }

    private void Finish(int gen, float[] peaks, bool gotAudio)
    {
        if (Volatile.Read(ref _gen) != gen)
            return;
        Normalize(peaks);
        Publish(gen, peaks, partial: false, gotAudio: gotAudio);
    }

    private void Publish(int gen, float[] peaks, bool partial, bool gotAudio)
    {
        if (Volatile.Read(ref _gen) != gen)
            return;
        lock (_gate)
            _peaks = peaks;
        _ui.BeginInvoke(() =>
        {
            if (Volatile.Read(ref _gen) != gen)
                return;
            HasAudio = gotAudio;
            Ready = !partial;
            Updated?.Invoke();
        });
    }

    private static void Normalize(float[] peaks)
    {
        var max = 0f;
        foreach (var p in peaks)
            if (p > max)
                max = p;
        if (max < 1e-4f)
            return;
        // Soft normalize so quiet clips still show shape; leave a little headroom.
        var scale = 0.92f / max;
        for (var i = 0; i < peaks.Length; i++)
            peaks[i] = Math.Clamp(peaks[i] * scale, 0f, 1f);
    }

    private static int BucketCount(double durationSec)
    {
        // ~20 envelopes/sec, clamped — enough when zoomed without huge RAM.
        var n = (int)Math.Round(durationSec * 20);
        return Math.Clamp(n, MinBuckets, MaxBuckets);
    }

    private void TryKill()
    {
        Process? proc;
        lock (_gate)
        {
            proc = _proc;
            _proc = null;
        }
        if (proc == null)
            return;
        try
        {
            if (!proc.HasExited)
                proc.Kill(entireProcessTree: true);
        }
        catch
        {
            // ignore
        }
        try
        {
            proc.Dispose();
        }
        catch
        {
            // ignore
        }
    }
}
