using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Media;

namespace VideoBlackout.Wpf;

public sealed class AudioRangeMark : INotifyPropertyChanged
{
    private double _startSec;
    private double _endSec;
    private int _effect;
    private bool _selected;

    public int Id { get; init; }

    public double StartSec
    {
        get => _startSec;
        set { if (Set(ref _startSec, value)) Raise(nameof(Label)); }
    }

    public double EndSec
    {
        get => _endSec;
        set { if (Set(ref _endSec, value)) Raise(nameof(Label)); }
    }

    /// <summary>0 = mute, 1 = beep.</summary>
    public int Effect
    {
        get => _effect;
        set
        {
            if (!Set(ref _effect, value))
                return;
            Raise(nameof(Label));
            Raise(nameof(EffectBrush));
            Raise(nameof(EffectName));
        }
    }

    public bool Selected
    {
        get => _selected;
        set => Set(ref _selected, value);
    }

    public string EffectName => Effect == 1 ? L.T("audioBeep") : L.T("audioMute");

    public Brush EffectBrush => Effect == 1
        ? new SolidColorBrush(Color.FromRgb(0xE8, 0xA8, 0x38))
        : new SolidColorBrush(Color.FromRgb(0x2E, 0xC4, 0xA8));

    public string Label
    {
        get
        {
            static string Fmt(double sec)
            {
                if (sec < 0)
                    sec = 0;
                var whole = (int)Math.Floor(sec);
                var ms = (int)Math.Round((sec - whole) * 100);
                if (ms >= 100)
                {
                    whole++;
                    ms = 0;
                }
                return $"{whole / 3600:00}:{whole / 60 % 60:00}:{whole % 60:00}.{ms:00}";
            }
            return $"{Fmt(StartSec)} – {Fmt(EndSec)} · {EffectName}";
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public void RefreshLocale()
    {
        Raise(nameof(EffectName));
        Raise(nameof(Label));
    }

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return false;
        field = value;
        Raise(name);
        return true;
    }

    private void Raise(string? name) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
