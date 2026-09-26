using System.IO;
using System.Windows.Media;
using System.Windows.Threading;

namespace VideoBlackout.Wpf;

/// <summary>
/// Preview soundtrack via MediaPlayer. Native preview only paints frames.
/// While the playhead sits inside a marked mute/beep span, preview audio is silenced
/// (and a tone plays for beep spans).
/// </summary>
internal sealed class PreviewAudio : IDisposable
{
    private readonly MediaPlayer _player = new();
    private readonly MediaPlayer _beep = new();
    private readonly DispatcherTimer _openWatch;
    private string? _path;
    private double _fps = 25;
    private bool _wantPlay;
    private bool _opened;
    private bool _disposed;
    private bool _userMuted;
    private bool _redactSilenced;
    private bool _beepActive;
    private bool _beepReady;
    private static string? _beepPath;

    public bool IsMuted
    {
        get => _userMuted;
        set
        {
            _userMuted = value;
            ApplyOutput();
        }
    }

    public PreviewAudio()
    {
        _player.MediaOpened += (_, _) =>
        {
            _opened = true;
            ApplyOutput();
            if (_wantPlay)
                _player.Play();
        };
        _player.MediaFailed += (_, _) =>
        {
            _opened = false;
            _wantPlay = false;
        };
        _beep.MediaEnded += (_, _) =>
        {
            if (!_beepActive || !_wantPlay)
                return;
            try
            {
                _beep.Position = TimeSpan.Zero;
                _beep.Play();
            }
            catch
            {
                // ignore
            }
        };
        _openWatch = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(80) };
        _openWatch.Tick += (_, _) =>
        {
            if (!_wantPlay || !_opened)
                return;
            _openWatch.Stop();
            ApplyOutput();
            _player.Play();
        };
    }

    public void Open(string? path, double fps)
    {
        Stop();
        _path = path;
        _fps = fps > 0.1 ? fps : 25;
        _opened = false;
        _redactSilenced = false;
        _beepActive = false;
        if (string.IsNullOrEmpty(path))
            return;
        try
        {
            _player.Open(new Uri(path, UriKind.Absolute));
            _player.Volume = 1.0;
            ApplyOutput();
            EnsureBeep();
        }
        catch
        {
            _path = null;
            _opened = false;
        }
    }

    public void PlayFromFrame(long frame)
    {
        if (string.IsNullOrEmpty(_path))
            return;
        _wantPlay = true;
        SeekFrame(frame);
        if (_opened)
        {
            ApplyOutput();
            _player.Play();
        }
        else
        {
            _openWatch.Stop();
            _openWatch.Start();
        }
    }

    public void Pause()
    {
        _wantPlay = false;
        _openWatch.Stop();
        StopBeep();
        try
        {
            _player.Pause();
        }
        catch
        {
            // ignore
        }
    }

    public void SeekFrame(long frame)
    {
        if (string.IsNullOrEmpty(_path))
            return;
        var sec = Math.Max(0, frame / _fps);
        try
        {
            _player.Position = TimeSpan.FromSeconds(sec);
        }
        catch
        {
            // ignore
        }
    }

    /// <summary>
    /// Sync preview mute/beep with marked ranges at the current playhead.
    /// effect: 0 mute, 1 beep; null = outside any span.
    /// </summary>
    public void ApplyRedactionAt(double timeSec, IReadOnlyList<AudioRangeMark> ranges)
    {
        AudioRangeMark? hit = null;
        foreach (var r in ranges)
        {
            if (timeSec >= r.StartSec && timeSec < r.EndSec)
            {
                hit = r;
                break;
            }
        }

        _redactSilenced = hit != null;
        ApplyOutput();

        var wantBeep = hit is { Effect: 1 } && _wantPlay && !_userMuted;
        if (wantBeep)
            StartBeep();
        else
            StopBeep();
    }

    public void ClearRedaction()
    {
        _redactSilenced = false;
        StopBeep();
        ApplyOutput();
    }

    public void Stop()
    {
        _wantPlay = false;
        _openWatch.Stop();
        StopBeep();
        _redactSilenced = false;
        try
        {
            _player.Stop();
            _player.Close();
        }
        catch
        {
            // ignore
        }
        _opened = false;
        _path = null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        Stop();
        try
        {
            _beep.Close();
        }
        catch
        {
            // ignore
        }
    }

    private void ApplyOutput()
    {
        try
        {
            var silence = _userMuted || _redactSilenced;
            _player.IsMuted = silence;
            _player.Volume = silence ? 0 : 1.0;
        }
        catch
        {
            // ignore
        }
    }

    private void EnsureBeep()
    {
        if (_beepReady)
            return;
        try
        {
            var path = EnsureBeepFile();
            if (path == null)
                return;
            _beep.Open(new Uri(path, UriKind.Absolute));
            _beep.Volume = 0.28;
            _beepReady = true;
        }
        catch
        {
            _beepReady = false;
        }
    }

    private void StartBeep()
    {
        if (!_beepReady)
            EnsureBeep();
        if (!_beepReady)
            return;
        _beepActive = true;
        try
        {
            _beep.Position = TimeSpan.Zero;
            _beep.Play();
        }
        catch
        {
            _beepActive = false;
        }
    }

    private void StopBeep()
    {
        _beepActive = false;
        try
        {
            _beep.Stop();
        }
        catch
        {
            // ignore
        }
    }

    private static string? EnsureBeepFile()
    {
        if (!string.IsNullOrEmpty(_beepPath) && File.Exists(_beepPath))
            return _beepPath;
        try
        {
            var dir = Path.Combine(Path.GetTempPath(), "VideoBlackout");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, "preview-beep.wav");
            if (!File.Exists(path))
                WriteBeepWav(path);
            _beepPath = path;
            return path;
        }
        catch
        {
            return null;
        }
    }

    private static void WriteBeepWav(string path)
    {
        const int sampleRate = 22050;
        const double seconds = 0.35;
        const double freq = 1000;
        var count = (int)(sampleRate * seconds);
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        var dataBytes = count * 2;
        bw.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
        bw.Write(36 + dataBytes);
        bw.Write(System.Text.Encoding.ASCII.GetBytes("WAVE"));
        bw.Write(System.Text.Encoding.ASCII.GetBytes("fmt "));
        bw.Write(16);
        bw.Write((short)1);
        bw.Write((short)1);
        bw.Write(sampleRate);
        bw.Write(sampleRate * 2);
        bw.Write((short)2);
        bw.Write((short)16);
        bw.Write(System.Text.Encoding.ASCII.GetBytes("data"));
        bw.Write(dataBytes);
        for (var i = 0; i < count; i++)
        {
            var t = i / (double)sampleRate;
            var env = 1.0;
            if (t < 0.02)
                env = t / 0.02;
            else if (t > seconds - 0.04)
                env = Math.Max(0, (seconds - t) / 0.04);
            var sample = (short)(Math.Sin(2 * Math.PI * freq * t) * 0.45 * env * short.MaxValue);
            bw.Write(sample);
        }
    }
}
