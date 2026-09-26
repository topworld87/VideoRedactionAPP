using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace VideoBlackout.Wpf;

/// <summary>
/// Desktop login matches the website bridge: a loopback callback receives a
/// one-time desktop token, then POST /api/desktop/exchange binds this HWID.
/// Free is the local default when there is no active license.
/// </summary>
public sealed class LicenseClient
{
    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    private readonly string _licensePath;
    private StoredLicense? _stored;

    public LicenseClient(string hwid)
    {
        Hwid = hwid ?? "";
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "VideoBlackout");
        Directory.CreateDirectory(dir);
        _licensePath = Path.Combine(dir, "license.json");
        Current = Entitlements.Free;
    }

    public string Hwid { get; }
    public Entitlements Current { get; private set; }
    public event Action? Changed;

    public static string BaseUrl
    {
        get
        {
            var raw = Environment.GetEnvironmentVariable("VIDEOREDACTION_BASE_URL");
            if (string.IsNullOrWhiteSpace(raw))
                return "https://videoredaction.io";
            return raw.Trim().TrimEnd('/');
        }
    }

    public void Load()
    {
        _stored = ReadStored();
        Publish(Entitlements.FromStored(_stored, Hwid));
    }

    public void SignOut()
    {
        _stored = null;
        Publish(Entitlements.Free);
        try
        {
            if (File.Exists(_licensePath))
                File.Delete(_licensePath);
        }
        catch
        {
            // The next launch still treats a bad file as Free.
        }
    }

    public void OpenPricing() => OpenPage($"/{AppLocale.Code}/pricing");

    public void OpenAccount() => OpenPage($"/{AppLocale.Code}/account");

    public async Task<SignInResult> SignInAsync(CancellationToken cancellationToken)
    {
        if (Hwid.Length < 8)
            return new SignInResult(false, L.T("authNoHwid"));

        var state = Convert.ToHexString(System.Security.Cryptography.RandomNumberGenerator.GetBytes(16));
        using var listener = new LoopbackListener();
        listener.Start();
        var url = $"{BaseUrl}/{AppLocale.Code}/login?desktop=1&port={listener.Port}&state={state}";
        OpenUrl(url);

        Dictionary<string, string>? form;
        try
        {
            form = await listener.WaitForTokenAsync(state, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return new SignInResult(false, L.T("authCancelled"));
        }

        if (form == null || !form.TryGetValue("desktopToken", out var token) || token.Length < 8)
            return new SignInResult(false, L.T("authNoToken"));

        form.TryGetValue("email", out var email);
        return await ExchangeAsync(token, email, cancellationToken).ConfigureAwait(false);
    }

    public async Task RefreshOnlineAsync(CancellationToken cancellationToken)
    {
        if (_stored == null || string.IsNullOrWhiteSpace(_stored.UserId))
            return;
        if (!string.Equals(_stored.Hwid, Hwid, StringComparison.Ordinal))
            return;

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(12) };
            var body = JsonSerializer.Serialize(new { hwid = Hwid, userId = _stored.UserId }, JsonOpts);
            using var res = await http.PostAsync(
                $"{BaseUrl}/api/desktop/check",
                new StringContent(body, Encoding.UTF8, "application/json"),
                cancellationToken).ConfigureAwait(false);
            var text = await res.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            if (res.StatusCode == HttpStatusCode.NotFound)
                return;
            if (res.StatusCode == HttpStatusCode.PaymentRequired)
            {
                RememberFree();
                return;
            }
            if (!res.IsSuccessStatusCode)
                return;
            var check = JsonSerializer.Deserialize<CheckResponse>(text, JsonOpts);
            if (check == null || !check.Ok)
                return;
            if (string.IsNullOrWhiteSpace(check.PlanId)
                || string.Equals(check.PlanId, "free", StringComparison.OrdinalIgnoreCase))
            {
                RememberFree();
                return;
            }
            _stored.PlanId = check.PlanId;
            if (DateTime.TryParse(check.PeriodEnd, null,
                    System.Globalization.DateTimeStyles.RoundtripKind, out var until))
                _stored.ValidUntil = until.ToUniversalTime();
            Save(_stored);
            Publish(Entitlements.FromStored(_stored, Hwid));
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch
        {
            // Offline: keep the cached license until validUntil.
        }
    }

    private async Task<SignInResult> ExchangeAsync(string desktopToken, string? email, CancellationToken cancellationToken)
    {
        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
            var body = JsonSerializer.Serialize(new { desktopToken, hwid = Hwid }, JsonOpts);
            using var res = await http.PostAsync(
                $"{BaseUrl}/api/desktop/exchange",
                new StringContent(body, Encoding.UTF8, "application/json"),
                cancellationToken).ConfigureAwait(false);
            var text = await res.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            var parsed = JsonSerializer.Deserialize<ExchangeResponse>(text, JsonOpts);
            var lic = parsed?.License;
            var error = parsed?.Error ?? "";
            if (res.StatusCode == HttpStatusCode.PaymentRequired
                && string.IsNullOrWhiteSpace(lic?.UserId))
                return RememberSignedIn(email, null);
            if (res.StatusCode == HttpStatusCode.Forbidden)
                return new SignInResult(false, L.T("authDeviceLimit"));
            if (res.StatusCode == HttpStatusCode.Unauthorized)
                return new SignInResult(false, L.T("authExpired"));
            if (!res.IsSuccessStatusCode || lic == null)
                return new SignInResult(false,
                    string.IsNullOrWhiteSpace(error) ? L.T("authActivateFail") : error);

            if (!string.Equals(lic.Hwid, Hwid, StringComparison.Ordinal))
                return new SignInResult(false, L.T("authWrongPc"));

            var stored = new StoredLicense
            {
                UserId = lic.UserId ?? "",
                Email = lic.Email,
                Plan = lic.Plan,
                PlanId = lic.PlanId,
                Hwid = Hwid,
                SignedIn = true,
                DeviceLimit = lic.DeviceLimit,
                ValidUntil = DateTime.TryParse(lic.ValidUntil, null,
                    System.Globalization.DateTimeStyles.RoundtripKind, out var until)
                    ? until.ToUniversalTime()
                    : DateTime.UtcNow.AddDays(30)
            };
            if (string.IsNullOrWhiteSpace(stored.UserId))
                return new SignInResult(false, L.T("authIncomplete"));
            Save(stored);
            _stored = stored;
            Publish(Entitlements.FromStored(_stored, Hwid));
            if (!Current.IsSignedIn)
                return new SignInResult(false, L.T("authInactive"));
            if (!Current.IsPaid)
                return new SignInResult(true, L.T("authNoSub"));
            return new SignInResult(true, string.IsNullOrWhiteSpace(Current.Email)
                ? L.F("authActivated", Current.LocalizedName)
                : L.F("authActivatedFor", Current.LocalizedName, Current.Email));
        }
        catch (OperationCanceledException)
        {
            return new SignInResult(false, L.T("authCancelled"));
        }
        catch
        {
            return new SignInResult(false, L.T("authOffline"));
        }
    }

    private SignInResult RememberSignedIn(string? email, string? userId)
    {
        email = string.IsNullOrWhiteSpace(email) ? null : email.Trim();
        userId = string.IsNullOrWhiteSpace(userId) ? null : userId.Trim();
        var stored = new StoredLicense
        {
            UserId = userId,
            Email = email,
            Plan = "free",
            PlanId = "free",
            Hwid = Hwid,
            SignedIn = true
        };
        Save(stored);
        _stored = stored;
        Publish(Entitlements.FromStored(_stored, Hwid));
        return new SignInResult(true, L.T("authNoSub"));
    }

    private void RememberFree()
    {
        if (_stored == null || (!_stored.SignedIn
            && string.IsNullOrWhiteSpace(_stored.UserId)
            && string.IsNullOrWhiteSpace(_stored.Email)))
        {
            SignOut();
            return;
        }
        _stored.Plan = "free";
        _stored.PlanId = "free";
        _stored.SignedIn = true;
        _stored.DeviceLimit = 0;
        _stored.ValidUntil = null;
        Save(_stored);
        Publish(Entitlements.FromStored(_stored, Hwid));
    }

    private void Publish(Entitlements next)
    {
        Current = next;
        Changed?.Invoke();
    }

    private void Save(StoredLicense stored)
    {
        File.WriteAllText(_licensePath, JsonSerializer.Serialize(stored, JsonOpts));
    }

    private StoredLicense? ReadStored()
    {
        try
        {
            if (!File.Exists(_licensePath))
                return null;
            return JsonSerializer.Deserialize<StoredLicense>(File.ReadAllText(_licensePath), JsonOpts);
        }
        catch
        {
            return null;
        }
    }

    private static void OpenPage(string path)
    {
        var url = BaseUrl + path;
        OpenUrl(url);
    }

    private static void OpenUrl(string url)
    {
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
        {
            FileName = url,
            UseShellExecute = true
        });
    }

    private sealed class LoopbackListener : IDisposable
    {
        private readonly TcpListener _listener;
        public int Port { get; private set; }

        public LoopbackListener()
        {
            _listener = new TcpListener(IPAddress.Loopback, 0);
        }

        public void Start()
        {
            _listener.Start();
            Port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        }

        public async Task<Dictionary<string, string>?> WaitForTokenAsync(string expectedState, CancellationToken ct)
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(TimeSpan.FromMinutes(8));
            try
            {
                while (!timeout.IsCancellationRequested)
                {
                    TcpClient client;
                    try
                    {
                        client = await _listener.AcceptTcpClientAsync(timeout.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                    {
                        return null;
                    }
                    using (client)
                    {
                        var form = await ReadCallbackAsync(client, expectedState).ConfigureAwait(false);
                        if (form != null)
                            return form;
                    }
                }
            }
            catch (OperationCanceledException)
            {
                return null;
            }
            return null;
        }

        private static async Task<Dictionary<string, string>?> ReadCallbackAsync(TcpClient client, string expectedState)
        {
            client.ReceiveTimeout = 8000;
            client.SendTimeout = 8000;
            using var stream = client.GetStream();
            var raw = await ReadHttpAsync(stream).ConfigureAwait(false);
            if (raw == null)
            {
                await WriteAsync(stream, 400, "Bad request").ConfigureAwait(false);
                return null;
            }
            var (path, body) = raw.Value;
            var query = "";
            var q = path.IndexOf('?');
            if (q >= 0)
            {
                query = path[(q + 1)..];
                path = path[..q];
            }
            if (!string.Equals(path, "/callback", StringComparison.Ordinal))
            {
                await WriteAsync(stream, 404, "Not Found").ConfigureAwait(false);
                return null;
            }
            var form = ParseForm(string.IsNullOrEmpty(body) ? query : body);
            form.TryGetValue("state", out var state);
            form.TryGetValue("desktopToken", out var token);
            if (!string.Equals(state, expectedState, StringComparison.Ordinal) ||
                string.IsNullOrWhiteSpace(token))
            {
                await WriteAsync(stream, 400, "Invalid callback").ConfigureAwait(false);
                return null;
            }
            await WriteAsync(stream, 200, """
                <!doctype html><html><head><meta charset="utf-8"><title>Signed in</title></head>
                <body style="font-family:Segoe UI,sans-serif;padding:32px">
                <h1>Signed in</h1>
                <p>Returning to Video Blackout…</p>
                <script>
                try { window.close(); } catch (e) {}
                setTimeout(function () {
                  try { window.close(); } catch (e) {}
                  document.body.innerHTML = '<h1>Signed in</h1><p>You can close this tab and return to Video Blackout.</p>';
                }, 400);
                </script>
                </body></html>
                """).ConfigureAwait(false);
            return form;
        }

        private static async Task<(string Path, string Body)?> ReadHttpAsync(NetworkStream stream)
        {
            var buf = new byte[8192];
            var acc = new MemoryStream();
            var headerEnd = -1;
            while (headerEnd < 0 && acc.Length < 65536)
            {
                var n = await stream.ReadAsync(buf).ConfigureAwait(false);
                if (n <= 0)
                    break;
                acc.Write(buf, 0, n);
                headerEnd = IndexOf(acc, "\r\n\r\n"u8);
            }
            if (headerEnd < 0)
                return null;
            var headerBytes = acc.ToArray();
            var header = Encoding.ASCII.GetString(headerBytes, 0, headerEnd);
            var first = header.Split("\r\n", 2)[0];
            var parts = first.Split(' ');
            if (parts.Length < 2)
                return null;
            var length = 0;
            foreach (var line in header.Split("\r\n"))
            {
                if (line.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase) &&
                    int.TryParse(line["Content-Length:".Length..].Trim(), out var parsed))
                    length = Math.Clamp(parsed, 0, 65536);
            }
            var bodyStart = headerEnd + 4;
            var have = headerBytes.Length - bodyStart;
            var body = new byte[length];
            if (have > 0)
                Buffer.BlockCopy(headerBytes, bodyStart, body, 0, Math.Min(have, length));
            var got = Math.Max(0, Math.Min(have, length));
            while (got < length)
            {
                var n = await stream.ReadAsync(body.AsMemory(got, length - got)).ConfigureAwait(false);
                if (n <= 0)
                    break;
                got += n;
            }
            return (parts[1], Encoding.UTF8.GetString(body, 0, got));
        }

        private static int IndexOf(MemoryStream acc, ReadOnlySpan<byte> needle)
        {
            var data = acc.GetBuffer();
            var len = (int)acc.Length;
            for (var i = 0; i + needle.Length <= len; i++)
            {
                if (data.AsSpan(i, needle.Length).SequenceEqual(needle))
                    return i;
            }
            return -1;
        }

        private static Dictionary<string, string> ParseForm(string raw)
        {
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var part in raw.Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                var i = part.IndexOf('=');
                var key = Uri.UnescapeDataString((i < 0 ? part : part[..i]).Replace('+', ' '));
                var value = i < 0 ? "" : Uri.UnescapeDataString(part[(i + 1)..].Replace('+', ' '));
                map[key] = value;
            }
            return map;
        }

        private static async Task WriteAsync(NetworkStream stream, int status, string body)
        {
            var reason = status switch
            {
                200 => "OK",
                400 => "Bad Request",
                404 => "Not Found",
                _ => "Error"
            };
            var type = body.Contains("<html", StringComparison.OrdinalIgnoreCase) ? "text/html; charset=utf-8" : "text/plain; charset=utf-8";
            var bytes = Encoding.UTF8.GetBytes(body);
            var head = $"HTTP/1.1 {status} {reason}\r\nContent-Type: {type}\r\nContent-Length: {bytes.Length}\r\nConnection: close\r\n\r\n";
            var headBytes = Encoding.ASCII.GetBytes(head);
            await stream.WriteAsync(headBytes).ConfigureAwait(false);
            await stream.WriteAsync(bytes).ConfigureAwait(false);
        }

        public void Dispose()
        {
            try { _listener.Stop(); } catch { /* already stopped */ }
        }
    }
}

public readonly record struct SignInResult(bool Activated, string Message);

public sealed class Entitlements
{
    public static Entitlements Free { get; } = new("free", "Free", false, 10, true, 0, null, null, null, false);

    public string PlanId { get; }
    public string DisplayName { get; }
    public bool AllowBatch { get; }
    public int MaxMinutes { get; }
    public bool Watermark { get; }
    public int DeviceLimit { get; }
    public string? Email { get; }
    public string? UserId { get; }
    public DateTime? ValidUntil { get; }
    public bool IsPaid => PlanId != "free";
    public bool IsSignedIn { get; }

    public string LocalizedName => PlanId switch
    {
        "free" => L.T("planFree"),
        "basic_month" => L.T("planBasic"),
        "annual" => L.T("planAnnual"),
        "pro_month" => L.T("planPro"),
        _ => DisplayName
    };

    private Entitlements(string planId, string displayName, bool allowBatch, int maxMinutes,
        bool watermark, int deviceLimit, string? email, string? userId, DateTime? validUntil, bool signedIn)
    {
        PlanId = planId;
        DisplayName = displayName;
        AllowBatch = allowBatch;
        MaxMinutes = maxMinutes;
        Watermark = watermark;
        DeviceLimit = deviceLimit;
        Email = email;
        UserId = userId;
        ValidUntil = validUntil;
        IsSignedIn = signedIn;
    }

    public string Summary
    {
        get
        {
            if (!IsPaid)
                return L.T("planSummaryFree");
            var length = MaxMinutes <= 0 ? L.T("planUnlimited") : L.F("planUpTo", MaxMinutes);
            var batch = AllowBatch ? L.T("planBatch") : L.T("planOne");
            var mark = Watermark ? L.T("planWatermark") : L.T("planNoWatermark");
            var devices = DeviceLimit == 1 ? L.T("planOneDevice") : L.F("planDevices", DeviceLimit);
            return L.F("planSummary", batch, length, mark, devices);
        }
    }

    internal static Entitlements FromStored(StoredLicense? stored, string hwid)
    {
        var known = stored != null && (stored.SignedIn
            || !string.IsNullOrWhiteSpace(stored.UserId)
            || !string.IsNullOrWhiteSpace(stored.Email));
        if (!known || stored == null)
            return Free;
        if (!string.Equals(stored.Hwid, hwid, StringComparison.Ordinal))
            return Free;
        if (IsExpired(stored.ValidUntil))
            return SignedInFree(stored.Email, stored.UserId);
        var plan = ForPlan(stored.PlanId, stored.Plan, stored.DeviceLimit, stored.Email, stored.UserId, stored.ValidUntil);
        return plan.IsSignedIn ? plan : SignedInFree(stored.Email, stored.UserId);
    }

    public static Entitlements ForPlan(string? planId, string? plan, int deviceLimit,
        string? email, string? userId, DateTime? validUntil)
    {
        var id = (planId ?? "").Trim().ToLowerInvariant();
        var tier = (plan ?? "").Trim().ToLowerInvariant();
        var pro = id is "annual" or "pro_month" || id.StartsWith("pro", StringComparison.Ordinal) || tier == "pro";
        var basic = id is "basic_month" || id.StartsWith("basic", StringComparison.Ordinal) || tier == "basic";
        if (pro)
        {
            var name = id == "annual" ? "Annual" : "Pro";
            var seats = deviceLimit > 0 ? deviceLimit : 2;
            return new(id == "annual" ? "annual" : "pro_month", name, true, 0, false, seats, email, userId, validUntil, true);
        }
        if (basic)
        {
            var seats = deviceLimit > 0 ? deviceLimit : 1;
            return new("basic_month", "Basic", false, 10, false, seats, email, userId, validUntil, true);
        }
        if (string.IsNullOrWhiteSpace(userId) && string.IsNullOrWhiteSpace(email))
            return Free;
        return SignedInFree(email, userId);
    }

    private static Entitlements SignedInFree(string? email, string? userId) =>
        new("free", "Free", false, 10, true, 0, email, userId, null, true);

    private static bool IsExpired(DateTime? until)
    {
        if (!until.HasValue)
            return false;
        var utc = until.Value.Kind == DateTimeKind.Utc ? until.Value : until.Value.ToUniversalTime();
        return utc < DateTime.UtcNow.AddMinutes(-2);
    }
}

internal sealed class StoredLicense
{
    public string? UserId { get; set; }
    public string? Email { get; set; }
    public string? Plan { get; set; }
    public string? PlanId { get; set; }
    public string? Hwid { get; set; }
    public bool SignedIn { get; set; }
    public int DeviceLimit { get; set; }
    public DateTime? ValidUntil { get; set; }
}

internal sealed class ExchangeResponse
{
    public string? Error { get; set; }
    public LicensePayload? License { get; set; }
}

internal sealed class LicensePayload
{
    public string? UserId { get; set; }
    public string? Email { get; set; }
    public string? Plan { get; set; }
    public string? PlanId { get; set; }
    public string? Hwid { get; set; }
    public int DeviceLimit { get; set; }
    public string? ValidUntil { get; set; }
}

internal sealed class CheckResponse
{
    public bool Ok { get; set; }
    public string? PlanId { get; set; }
    public string? PeriodEnd { get; set; }
    public string? Error { get; set; }
}
