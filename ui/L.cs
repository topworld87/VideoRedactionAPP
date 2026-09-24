using System.ComponentModel;
using System.Windows.Data;

namespace VideoBlackout.Wpf;

/// <summary>
/// UI strings. English is filled in; missing keys fall back to the key itself.
/// </summary>
public sealed class LocTable : INotifyPropertyChanged
{
    private static readonly Dictionary<string, string> English = new()
    {
        ["menuFile"] = "_File",
        ["menuOpen"] = "_Open...",
        ["menuAddFiles"] = "Add to _File List...",
        ["menuExit"] = "E_xit",
        ["menuAccount"] = "_Account",
        ["menuAccountItem"] = "_Account...",
        ["menuUpgrade"] = "_Upgrade...",
        ["menuSignIn"] = "Sign _in",
        ["menuSignOut"] = "Sign _out",
        ["menuEdit"] = "_Edit",
        ["menuMask"] = "Mask style",
        ["menuMosaic"] = "Mosaic",
        ["menuBlur"] = "Blur",
        ["menuSolid"] = "Solid color",
        ["menuSettings"] = "_Settings...",
        ["menuHelp"] = "_Help",
        ["menuGuide"] = "User _guide",
        ["menuDump"] = "Dump detect _debug (F8)",
        ["menuLicenses"] = "Open source _licenses",
        ["menuAbout"] = "_About",
        ["langTip"] = "Language",
        ["filesTitle"] = "FILES",
        ["jobTitle"] = "JOB",
        ["redactAll"] = "Redact all",
        ["keepPeople"] = "Keep people",
        ["detectTitle"] = "DETECT",
        ["detectBoth"] = "Faces and plates",
        ["detectFaces"] = "Faces only",
        ["detectPlates"] = "Plates only",
        ["keepPlates"] = "Also redact plates",
        ["addFiles"] = "Add...",
        ["removeFile"] = "Remove",
        ["clearDone"] = "Clear done",
        ["clearAll"] = "Clear all",
        ["tipAddFiles"] = "Add one or more videos to the list.",
        ["tipRemoveFile"] = "Take the selected video off the list. The file stays on disk.",
        ["tipClearDone"] = "Take finished and cancelled videos off the list. Files stay on disk.",
        ["tipClearAll"] = "Take every video off the list. Files stay on disk.",
        ["fileCol"] = "FILE",
        ["samePerson"] = "Same person",
        ["notThem"] = "Not them",
        ["boxFollow"] = "Box & follow",
        ["boxPin"] = "Box & pin",
        ["tipBoxFollow"] = "Draw a box around a missed face or plate. The box follows it, and export masks it.",
        ["tipBoxPin"] = "Draw a box that stays in place until the video ends. Export masks it.",
        ["dropHere"] = "Drop videos or a folder here",
        ["fileSummary"] = "{0} in list · {1} waiting · {2} done · {3} failed",
        ["msgApp"] = "Video Blackout",
        ["msgDetectDebug"] = "Detect debug",
        ["msgOpenPause"] = "Open a video and pause on the failing frame first.",
        ["msgDumpFailed"] = "Dump failed. Play or seek once so a frame is cached, then try again.",
        ["msgDumpSaved"] = "Saved frame.png / overlay.png / report.txt — send this folder.",
        ["msgOpenFail"] = "Could not open video.",
        ["msgKeepPeople"] = "Keep people",
        ["msgClickFace"] = "Click a face on the video first.",
        ["msgOpenFirst"] = "Open a video first.",
        ["processThis"] = "Process this video",
        ["msgRedactAll"] = "Redact all",
        ["msgAddFirst"] = "Add videos to the list first.",
        ["msgBatch"] = "Batch",
        ["msgBatchLimit"] = "{0} processes one video at a time.",
        ["dlgUpgrade"] = "Upgrade",
        ["dlgClose"] = "Close",
        ["msgProcessAll"] = "Process all",
        ["msgProcessConfirm"] = "Process {0} videos next to the originals as *_redacted.mp4?",
        ["msgStartFail"] = "Could not start processing.",
        ["msgExportStartFail"] = "Export could not start. Open a video first.",
        ["filterMp4"] = "MP4 Video (*.mp4)",
        ["filterVideo"] = "Video files",
        ["filterAll"] = "All files",
        ["msgProcessed"] = "{0} processed",
        ["msgFailedN"] = "{0} failed",
        ["msgCancelledN"] = "{0} cancelled",
        ["msgTooLongN"] = "{0} over {1} min",
        ["msgSummarySep"] = ", ",
        ["msgSummaryEnd"] = ".",
        ["msgFinishedMark"] = "Processing finished. This export includes the free watermark.",
        ["msgFinished"] = "Processing finished.",
        ["msgProcess"] = "Process",
        ["msgCancelled"] = "Processing cancelled.",
        ["msgSwitchRedact"] = "Switch to Redact all",
        ["msgDiscardKeep"] = "Kept people on this video will be discarded.",
        ["tipKeepReady"] = "Export this video, keeping selected people clear.",
        ["tipNeedFace"] = "Click a face on the video first.",
        ["hintClickFace"] = "Click a face first",
        ["processAll"] = "Process all",
        ["processAllN"] = "Process all ({0})",
        ["tipExport"] = "Export waiting videos.",
        ["tipAdd"] = "Add videos to the list.",
        ["hintOneAtATime"] = "This plan processes one video at a time",
        ["hintWatermark"] = "Free exports include a watermark",
        ["msgTooLong"] = "This video is about {0} min. {1} allows up to {2} min.",
        ["planFreeTip"] = "Free plan",
        ["dlgSignOut"] = "Sign out",
        ["msgSignOutBody"] = "Sign out on this PC? Free limits will apply.",
        ["emptyKeep"] = "No one kept yet. Click a face on the video.",
        ["emptyDetect"] = "Play the video to see detections. Empty is OK — export still scans the file.",
        ["msgSettings"] = "Settings",
        ["msgSettingsBody"] = "Use GPU (DirectML) when available?\n\nIf GPU setup fails, the engine falls back to CPU and will be much slower.",
        ["dlgUseGpu"] = "Use GPU",
        ["dlgUseCpu"] = "Use CPU",
        ["msgAbout"] = "About",
        ["msgVersion"] = "Version",
        ["msgLicenses"] = "Open source licenses",
        ["dlgViewLicense"] = "View AGPL-3.0",
        ["msgLicenseMissing"] = "The license notice was not found next to the app.",
        ["msgAboutBody"] = "Video Blackout is free software under the GNU AGPL v3. There is no warranty. You may redistribute it under that license. The face model yolo11n-face.onnx is AGPL-3.0, so this whole application is too. Details are under Help → Open source licenses.",
        ["msgSourceCode"] = "Source code",
        ["msgGuideMissing"] = "The user guide was not found next to the app.",
        ["msgMachineId"] = "Machine ID",
        ["jobHintKeep"] = "Click faces to choose who stays clear. Export masks everyone else. This file only.",
        ["jobHintRedact"] = "Mask every detected face and plate, then process the list.",
        ["keepClear"] = "KEEP",
        ["maskTitle"] = "MASK",
        ["detected"] = "DETECTED",
        ["targetKeep"] = "Green boxes stay clear. Drag through the video to see the same person recognized.",
        ["maskHint"] = "Blue boxes follow. Purple boxes stay put. Both are masked on export.",
        ["emptyMask"] = "None yet. Draw a box only if the AI missed someone.",
        ["targetMask"] = "Checked items are masked. Uncheck one to leave it clear on this video.",
        ["toolKeep"] = "Click a face to keep",
        ["maskTool"] = "Will be masked",
        ["maskToolTip"] = "Missed face or plate — will be masked",
        ["toolMissed"] = "If AI missed a face or plate, draw a box:",
        ["timelineZoomHint"] = "Ctrl + scroll wheel to zoom",
        ["previewMaximize"] = "Maximize preview",
        ["previewRestore"] = "Restore preview",
        ["emptyDrop"] = "Drop videos to redact",
        ["emptyStep1"] = "1. Choose Redact all or Keep people",
        ["emptyStep2"] = "2. Add files, then process",
        ["emptyKeepLine"] = "Keep people: click faces on one video",
        ["emptyBrowse"] = "Click to browse, or drag a file here",
        ["statusWaiting"] = "Waiting",
        ["statusDone"] = "Done",
        ["statusFailed"] = "Failed",
        ["statusCancelled"] = "Cancelled",
        ["statusTooLong"] = "Too long",
        ["statusProcessing"] = "Processing",
        ["dlgOk"] = "OK",
        ["dlgCancel"] = "Cancel",
        ["progressTitle"] = "Process",
        ["progressBody"] = "Processing the video with your current settings.\nPlease wait — the main window is locked until this finishes.",
        ["progressCancelling"] = "Cancelling...",
        ["accountTitle"] = "Account",
        ["accountHwid"] = "HARDWARE ID",
        ["accountCopy"] = "Copy",
        ["accountSignIn"] = "Sign in",
        ["accountSignInAgain"] = "Sign in again",
        ["accountUpgrade"] = "Upgrade",
        ["accountManage"] = "Manage devices",
        ["accountSignOut"] = "Sign out",
        ["accountGuest"] = "Not signed in. Free works without an account.",
        ["accountChipGuest"] = "Not signed in",
        ["accountChipUser"] = "Signed in",
        ["accountUntil"] = "Active until {0}",
        ["accountWaiting"] = "Waiting for the browser sign-in…",
        ["accountCancelled"] = "Sign-in cancelled.",
        ["accountSignedOut"] = "Signed out. Free limits apply.",
        ["accountCopied"] = "Hardware ID copied.",
        ["accountUnavailable"] = "(unavailable)",
        ["planFree"] = "Free",
        ["planBasic"] = "Basic",
        ["planPro"] = "Pro",
        ["planAnnual"] = "Annual",
        ["planSummaryFree"] = "One video at a time, up to 10 minutes, with a watermark on export.",
        ["planUnlimited"] = "unlimited length",
        ["planUpTo"] = "up to {0} minutes",
        ["planBatch"] = "Batch folder processing",
        ["planOne"] = "One video at a time",
        ["planWatermark"] = "watermark on export",
        ["planNoWatermark"] = "no watermark",
        ["planOneDevice"] = "1 device",
        ["planDevices"] = "{0} devices",
        ["planSummary"] = "{0}, {1}, {2}, {3}.",
        ["authNoHwid"] = "Could not read this PC's hardware ID.",
        ["authCancelled"] = "Sign-in cancelled.",
        ["authNoToken"] = "The browser did not return a sign-in token.",
        ["authNoSub"] = "This account has no active subscription. Free limits still apply.",
        ["authDeviceLimit"] = "This account is already using its device limit. Remove a PC on the website, then sign in again.",
        ["authExpired"] = "That sign-in expired. Try again.",
        ["authActivateFail"] = "Could not activate this PC.",
        ["authWrongPc"] = "The license was issued for a different PC.",
        ["authIncomplete"] = "The license response was incomplete.",
        ["authInactive"] = "The subscription on this account is not active.",
        ["authActivated"] = "Activated {0}.",
        ["authActivatedFor"] = "Activated {0} for {1}.",
        ["authOffline"] = "Could not reach videoredaction.io. Check the connection and try again.",
    };

    public event PropertyChangedEventHandler? PropertyChanged;

    public string this[string key]
    {
        get
        {
            if (!string.Equals(L.Code, "en", StringComparison.Ordinal)
                && LocalePacks.TryGet(L.Code, key, out var translated))
                return translated;
            return English.TryGetValue(key, out var text) ? text : key;
        }
    }

    public void Raise() =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(Binding.IndexerName));
}

public static class L
{
    public static LocTable Current { get; } = new();
    public static string Code { get; private set; } = "en";
    public static event Action? Changed;

    public static void Use(string code)
    {
        Code = string.IsNullOrWhiteSpace(code) ? "en" : code;
        Current.Raise();
        Changed?.Invoke();
    }

    public static string T(string key) => Current[key];

    public static string F(string key, params object[] args)
    {
        try { return string.Format(T(key), args); }
        catch (FormatException) { return T(key); }
    }

    public static string Status(string status) => status switch
    {
        "Waiting" => T("statusWaiting"),
        "Done" => T("statusDone"),
        "Failed" => T("statusFailed"),
        "Cancelled" => T("statusCancelled"),
        "Too long" => T("statusTooLong"),
        "Processing" => T("statusProcessing"),
        _ => status
    };

    public static string Track(string label) => label;
}
