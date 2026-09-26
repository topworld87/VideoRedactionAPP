using System.Diagnostics;
using System.IO;
using System.Text;

namespace VideoBlackout.Wpf;

/// <summary>
/// Applies mute/beep ranges with ffmpeg after video export.
/// Mirrors native FfmpegAudioTools::remuxWithAudioRedaction.
/// </summary>
internal static class FfmpegAudioRemux
{
    public static string? FindFfmpeg()
    {
        var appDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        foreach (var cand in new[]
                 {
                     Path.Combine(appDir, "ffmpeg.exe"),
                     Path.Combine(appDir, "tools", "ffmpeg", "ffmpeg.exe"),
                     Path.Combine(appDir, "..", "app", "tools", "ffmpeg", "ffmpeg.exe"),
                 })
        {
            var full = Path.GetFullPath(cand);
            if (File.Exists(full))
                return full;
        }
        return null;
    }

    public static bool Remux(string ffmpegPath, string sourceVideo, string redactedVideo,
        string outputPath, IReadOnlyList<AudioRangeMark> ranges, out string error)
    {
        error = "";
        if (!File.Exists(ffmpegPath))
        {
            error = "ffmpeg.exe not found";
            return false;
        }
        if (!File.Exists(sourceVideo) || !File.Exists(redactedVideo))
        {
            error = "Input file missing for audio remux";
            return false;
        }

        var muteEnable = OrBetween(ranges, beepsOnly: false);
        var beepEnable = OrBetween(ranges, beepsOnly: true);
        var args = new List<string> { "-y", "-i", redactedVideo, "-i", sourceVideo };

        if (string.IsNullOrEmpty(muteEnable))
        {
            args.AddRange(["-map", "0:v:0", "-map", "1:a:0?", "-c:v", "copy", "-c:a", "aac",
                "-shortest", outputPath]);
        }
        else if (string.IsNullOrEmpty(beepEnable))
        {
            var af = $"volume=enable='{muteEnable}':volume=0";
            args.AddRange(["-filter_complex", $"[1:a]{af}[aout]", "-map", "0:v:0", "-map", "[aout]",
                "-c:v", "copy", "-c:a", "aac", "-shortest", outputPath]);
        }
        else
        {
            var beepVolume = $"if({beepEnable}\\,0.25\\,0)";
            var fc =
                $"[1:a]volume=enable='{muteEnable}':volume=0[a0];sine=frequency=1000:sample_rate=44100[braw];" +
                $"[braw]volume=eval=frame:volume='{beepVolume}'[beep];" +
                "[a0][beep]amix=inputs=2:duration=first:dropout_transition=0[aout]";
            args.AddRange(["-filter_complex", fc, "-map", "0:v:0", "-map", "[aout]",
                "-c:v", "copy", "-c:a", "aac", "-shortest", outputPath]);
        }

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = ffmpegPath,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
            };
            foreach (var a in args)
                psi.ArgumentList.Add(a);
            using var proc = Process.Start(psi);
            if (proc == null)
            {
                error = "Failed to start ffmpeg";
                return false;
            }
            var stderr = proc.StandardError.ReadToEnd();
            proc.WaitForExit(3_600_000);
            if (proc.ExitCode != 0 || !File.Exists(outputPath))
            {
                error = stderr.Length > 500 ? stderr[^500..] : stderr;
                if (string.IsNullOrWhiteSpace(error))
                    error = "ffmpeg failed";
                return false;
            }
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    private static string OrBetween(IReadOnlyList<AudioRangeMark> ranges, bool beepsOnly)
    {
        var parts = new List<string>();
        foreach (var r in ranges)
        {
            if (r.EndSec <= r.StartSec || r.StartSec < 0)
                continue;
            if (beepsOnly && r.Effect != 1)
                continue;
            parts.Add(Between(r.StartSec, r.EndSec));
        }
        return string.Join("+", parts);
    }

    private static string Between(double startSec, double endSec) =>
        $"between(t\\,{startSec:0.###}\\,{endSec:0.###})";
}
