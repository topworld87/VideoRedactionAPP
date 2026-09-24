using System.IO;

namespace VideoBlackout.Wpf;

public readonly record struct LocaleOption(string Code, string Label, string Short);

/// <summary>
/// Same locales as the website homepage language switcher.
/// </summary>
public static class AppLocale
{
    public static readonly LocaleOption[] All =
    [
        new("en", "English", "EN"),
        new("de", "Deutsch", "DE"),
        new("fr", "Français", "FR"),
        new("es", "Español", "ES"),
        new("pt-BR", "Português (Brasil)", "PT"),
        new("ja", "日本語", "JA"),
        new("zh-CN", "简体中文", "简体"),
        new("zh-TW", "繁體中文", "繁體"),
    ];

    public static string Code { get; private set; } = "en";

    public static string ShortLabel =>
        All.First(l => l.Code == Code).Short;

    public static void Load()
    {
        Code = Normalize(ReadSaved()) ?? "en";
        L.Use(Code);
    }

    public static void Select(string code)
    {
        Code = Normalize(code) ?? "en";
        Save(Code);
        L.Use(Code);
    }

    public static string? Normalize(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
            return null;
        var s = raw.Trim().Replace('_', '-');
        var exact = All.FirstOrDefault(l => l.Code.Equals(s, StringComparison.OrdinalIgnoreCase));
        if (exact.Code != null)
            return exact.Code;
        var lower = s.ToLowerInvariant();
        if (lower.StartsWith("zh-tw") || lower.StartsWith("zh-hk") || lower.StartsWith("zh-mo")
            || lower.Contains("hant"))
            return "zh-TW";
        if (lower.StartsWith("zh")) return "zh-CN";
        if (lower.StartsWith("pt")) return "pt-BR";
        if (lower.StartsWith("ja")) return "ja";
        if (lower.StartsWith("de")) return "de";
        if (lower.StartsWith("fr")) return "fr";
        if (lower.StartsWith("es")) return "es";
        if (lower.StartsWith("en")) return "en";
        return null;
    }

    private static string FilePath =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "Video Blackout",
            "language.txt");

    private static string? ReadSaved()
    {
        try
        {
            return File.Exists(FilePath) ? File.ReadAllText(FilePath).Trim() : null;
        }
        catch
        {
            return null;
        }
    }

    private static void Save(string code)
    {
        try
        {
            var dir = Path.GetDirectoryName(FilePath);
            if (!string.IsNullOrEmpty(dir))
                Directory.CreateDirectory(dir);
            File.WriteAllText(FilePath, code);
        }
        catch
        {
            // The choice still applies for this session.
        }
    }
}
