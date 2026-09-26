using System.Runtime.InteropServices;

namespace VideoBlackout.Wpf;

internal sealed class NativeEngine : IDisposable
{
    private const string Dll = "VideoBlackoutCore.dll";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void FrameFn(IntPtr user, IntPtr rgb, int width, int height, int stride,
        long frameIndex, double ptsSec);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void TracksFn(IntPtr user, IntPtr tracks, int count);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MarksFn(IntPtr user, IntPtr frames, int count);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ErrorFn(IntPtr user, IntPtr messageUtf8);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void OpenedFn(IntPtr user, double fps, long frames, int width, int height);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void VoidFn(IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ProgressFn(IntPtr user, int percent, IntPtr statusUtf8);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void DoneFn(IntPtr user, int ok, IntPtr pathOrErrorUtf8);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct TrackC
    {
        public int Id;
        public int Type;
        public int X;
        public int Y;
        public int W;
        public int H;
        public int Enabled;
        public int Lost;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)]
        public string Label;
        public int IdentityId;
        public int MatchTier;
        public float ReidScore;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct AudioRangeC
    {
        public int Id;
        public double StartSec;
        public double EndSec;
        public int Effect;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr vb_session_create();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_destroy(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_set_user(IntPtr session, IntPtr user);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_frame(IntPtr session, FrameFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_tracks(IntPtr session, TracksFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_marks(IntPtr session, MarksFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_error(IntPtr session, ErrorFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_opened(IntPtr session, OpenedFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_play_finished(IntPtr session, VoidFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_export_progress(IntPtr session, ProgressFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_session_on_export_done(IntPtr session, DoneFn fn);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern int vb_open(IntPtr session, string videoUtf8);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_close(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_play(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_pause(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_seek(IntPtr session, long frameIndex);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_mode(IntPtr session, int mode);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_faces(IntPtr session, int enabled);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_yolo(IntPtr session, int enabled);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_gpu(IntPtr session, int enabled);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_score(IntPtr session, float score);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_add_manual(IntPtr session, int x, int y, int w, int h);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_add_static(IntPtr session, int x, int y, int w, int h,
        long startFrame, long endFrame);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_track_enabled(IntPtr session, int id, int enabled);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_set_face_policy(IntPtr session, int policy);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_enroll_keep_at(IntPtr session, int x, int y);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_confirm_identity(IntPtr session, int trackId);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_reject_identity(IntPtr session, int trackId);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_clear_gallery(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "vb_add_audio_range")]
    private static extern int vb_add_audio_range_raw(IntPtr session, double startSec, double endSec,
        int effect);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "vb_update_audio_range")]
    private static extern int vb_update_audio_range_raw(IntPtr session, int id, double startSec,
        double endSec, int effect);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "vb_remove_audio_range")]
    private static extern void vb_remove_audio_range_raw(IntPtr session, int id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, EntryPoint = "vb_clear_audio_ranges")]
    private static extern void vb_clear_audio_ranges_raw(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr vb_runtime_status(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr vb_dump_detect_debug(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    private static extern int vb_export(IntPtr session, string outputUtf8, int watermark);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern void vb_cancel_export(IntPtr session);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr vb_hwid();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr vb_hardware_summary();

    public event Action<IntPtr, int, int, int, long, double>? FrameReady;
    public event Action<TrackC[]>? TracksUpdated;
    public event Action<long[]>? MarksUpdated;
    public event Action<string>? ErrorOccurred;
    public event Action<double, long, int, int>? Opened;
    public event Action? PlayFinished;
    public event Action<int, string>? ExportProgress;
    public event Action<bool, string>? ExportDone;

    private readonly IntPtr _session;
    private readonly FrameFn _frame;
    private readonly TracksFn _tracks;
    private readonly MarksFn _marks;
    private readonly ErrorFn _error;
    private readonly OpenedFn _opened;
    private readonly VoidFn _finished;
    private readonly ProgressFn _progress;
    private readonly DoneFn _done;
    private bool _disposed;

    public NativeEngine()
    {
        _session = vb_session_create();
        _frame = OnFrame;
        _tracks = OnTracks;
        _marks = OnMarks;
        _error = OnError;
        _opened = OnOpened;
        _finished = OnFinished;
        _progress = OnProgress;
        _done = OnDoneNative;
        vb_session_on_frame(_session, _frame);
        vb_session_on_tracks(_session, _tracks);
        vb_session_on_marks(_session, _marks);
        vb_session_on_error(_session, _error);
        vb_session_on_opened(_session, _opened);
        vb_session_on_play_finished(_session, _finished);
        vb_session_on_export_progress(_session, _progress);
        vb_session_on_export_done(_session, _done);
    }

    public bool Open(string path) => vb_open(_session, path) != 0;
    public void Close() => vb_close(_session);
    public void Play() => vb_play(_session);
    public void Pause() => vb_pause(_session);
    public void Seek(long frame) => vb_seek(_session, frame);
    public void SetMode(int mode) => vb_set_mode(_session, mode);
    public void SetFaces(bool on) => vb_set_faces(_session, on ? 1 : 0);
    public void SetYolo(bool on) => vb_set_yolo(_session, on ? 1 : 0);
    public void SetGpu(bool on) => vb_set_gpu(_session, on ? 1 : 0);
    public void SetScore(float v) => vb_set_score(_session, v);
    public void AddManual(int x, int y, int w, int h) => vb_add_manual(_session, x, y, w, h);
    public void AddStatic(int x, int y, int w, int h, long start, long end) =>
        vb_add_static(_session, x, y, w, h, start, end);
    public void SetTrackEnabled(int id, bool enabled) =>
        vb_set_track_enabled(_session, id, enabled ? 1 : 0);
    public void SetFacePolicy(int policy) => vb_set_face_policy(_session, policy);
    public void EnrollKeepAt(int x, int y) => vb_enroll_keep_at(_session, x, y);
    public void ConfirmIdentity(int trackId) => vb_confirm_identity(_session, trackId);
    public void RejectIdentity(int trackId) => vb_reject_identity(_session, trackId);
    public void ClearGallery() => vb_clear_gallery(_session);

    /// <summary>False when the loaded core DLL predates the audio API.</summary>
    public bool AudioApiAvailable { get; private set; } = true;

    public int AddAudioRange(double startSec, double endSec, int effect)
    {
        if (!AudioApiAvailable)
            return 0;
        try
        {
            return vb_add_audio_range_raw(_session, startSec, endSec, effect);
        }
        catch (EntryPointNotFoundException)
        {
            AudioApiAvailable = false;
            return 0;
        }
    }

    public bool UpdateAudioRange(int id, double startSec, double endSec, int effect)
    {
        if (!AudioApiAvailable)
            return false;
        try
        {
            return vb_update_audio_range_raw(_session, id, startSec, endSec, effect) != 0;
        }
        catch (EntryPointNotFoundException)
        {
            AudioApiAvailable = false;
            return false;
        }
    }

    public void RemoveAudioRange(int id)
    {
        if (!AudioApiAvailable)
            return;
        try
        {
            vb_remove_audio_range_raw(_session, id);
        }
        catch (EntryPointNotFoundException)
        {
            AudioApiAvailable = false;
        }
    }

    public void ClearAudioRanges()
    {
        if (!AudioApiAvailable)
            return;
        try
        {
            vb_clear_audio_ranges_raw(_session);
        }
        catch (EntryPointNotFoundException)
        {
            AudioApiAvailable = false;
        }
    }

    public string RuntimeStatus()
    {
        var p = vb_runtime_status(_session);
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    public string DumpDetectDebug()
    {
        var p = vb_dump_detect_debug(_session);
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    public bool Export(string output, bool watermark) =>
        vb_export(_session, output, watermark ? 1 : 0) != 0;
    public void CancelExport() => vb_cancel_export(_session);

    public static string HardwareId()
    {
        var p = vb_hwid();
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(p) ?? "";
    }

    public static string HardwareSummary()
    {
        var p = vb_hardware_summary();
        return p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
    }

    private void OnFrame(IntPtr user, IntPtr rgb, int width, int height, int stride,
        long frameIndex, double ptsSec) =>
        FrameReady?.Invoke(rgb, width, height, stride, frameIndex, ptsSec);

    private void OnTracks(IntPtr user, IntPtr tracks, int count)
    {
        var arr = new TrackC[Math.Max(0, count)];
        var size = Marshal.SizeOf<TrackC>();
        for (var i = 0; i < arr.Length; i++)
            arr[i] = Marshal.PtrToStructure<TrackC>(tracks + i * size);
        TracksUpdated?.Invoke(arr);
    }

    private void OnMarks(IntPtr user, IntPtr frames, int count)
    {
        var arr = new long[Math.Max(0, count)];
        Marshal.Copy(frames, arr, 0, arr.Length);
        MarksUpdated?.Invoke(arr);
    }

    private void OnError(IntPtr user, IntPtr messageUtf8) =>
        ErrorOccurred?.Invoke(Marshal.PtrToStringUTF8(messageUtf8) ?? "Error");

    private void OnOpened(IntPtr user, double fps, long frames, int width, int height) =>
        Opened?.Invoke(fps, frames, width, height);

    private void OnFinished(IntPtr user) => PlayFinished?.Invoke();

    private void OnProgress(IntPtr user, int percent, IntPtr statusUtf8) =>
        ExportProgress?.Invoke(percent, Marshal.PtrToStringUTF8(statusUtf8) ?? "");

    private void OnDoneNative(IntPtr user, int ok, IntPtr pathOrErrorUtf8) =>
        ExportDone?.Invoke(ok != 0, Marshal.PtrToStringUTF8(pathOrErrorUtf8) ?? "");

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        vb_session_destroy(_session);
        GC.KeepAlive(_frame);
        GC.KeepAlive(_tracks);
        GC.KeepAlive(_marks);
        GC.KeepAlive(_error);
        GC.KeepAlive(_opened);
        GC.KeepAlive(_finished);
        GC.KeepAlive(_progress);
        GC.KeepAlive(_done);
    }
}
