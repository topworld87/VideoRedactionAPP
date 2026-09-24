using System.IO;

namespace VideoBlackout.Wpf;

static class OpenSourceNotice
{
    public static string Load()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "legal", "NOTICE.txt");
        if (!File.Exists(path))
            return L.T("msgLicenseMissing");
        var text = File.ReadAllText(path);
        return Section(text, L.Code) ?? Section(text, "en") ?? text.Trim();
    }

    public static string LicensePath(string fileName) =>
        Path.Combine(AppContext.BaseDirectory, "legal", fileName);

    private static string? Section(string text, string code)
    {
        var marker = "===" + " " + code + " " + "===";
        var start = text.IndexOf(marker, StringComparison.Ordinal);
        if (start < 0)
            return null;
        start += marker.Length;
        var next = text.IndexOf("\n===", start, StringComparison.Ordinal);
        var body = next < 0 ? text[start..] : text[start..next];
        return body.Trim();
    }
}
