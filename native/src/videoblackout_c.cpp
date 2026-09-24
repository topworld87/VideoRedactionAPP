#include "vb/videoblackout_c.h"

#include "vb/ExportEngine.h"
#include "vb/FfmpegAudioTools.h"
#include "vb/HardwareInfo.h"
#include "vb/PreviewEngine.h"
#include "vb/Settings.h"
#include "vb/platform/HwId.h"

#include <cstring>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace {

struct Session {
  vb::PreviewEngine preview;
  vb::ExportEngine exporter;
  void* user = nullptr;
  VbFrameFn onFrame = nullptr;
  VbTracksFn onTracks = nullptr;
  VbMarksFn onMarks = nullptr;
  VbErrorFn onError = nullptr;
  VbOpenedFn onOpened = nullptr;
  VbVoidFn onPlayFinished = nullptr;
  VbProgressFn onExportProgress = nullptr;
  VbDoneFn onExportDone = nullptr;
  std::string videoPath;
  int mode = 0;
  bool faces = true;
  bool yolo = true;
  std::mutex mu;

  Session() {
    vb::Settings::instance().load();
    preview.setCallbacks(
        [this](const vb::FramePacket& p) {
          if (!onFrame || p.rgb.empty())
            return;
          onFrame(user, p.rgb.data, p.rgb.cols, p.rgb.rows,
                  static_cast<int>(p.rgb.step), p.frameIndex, p.ptsSec);
        },
        [this](const std::vector<vb::TrackObject>& tracks) {
          if (!onTracks)
            return;
          std::vector<VbTrackC> out(tracks.size());
          for (size_t i = 0; i < tracks.size(); ++i) {
            const auto& t = tracks[i];
            out[i].id = t.id;
            out[i].type = static_cast<int>(t.type);
            out[i].x = t.rect.x;
            out[i].y = t.rect.y;
            out[i].w = t.rect.width;
            out[i].h = t.rect.height;
            out[i].enabled = t.enabled ? 1 : 0;
            out[i].lost = t.lost ? 1 : 0;
            std::memset(out[i].label, 0, sizeof(out[i].label));
            std::strncpy(out[i].label, t.label.c_str(), sizeof(out[i].label) - 1);
            out[i].identityId = t.identityId;
            out[i].matchTier = static_cast<int>(t.matchTier);
            out[i].reidScore = t.reidScore;
          }
          onTracks(user, out.empty() ? nullptr : out.data(),
                   static_cast<int>(out.size()));
        },
        [this](const std::vector<int64_t>& marks) {
          if (!onMarks)
            return;
          onMarks(user, reinterpret_cast<const long long*>(marks.data()),
                  static_cast<int>(marks.size()));
        },
        [this](const std::string& msg) {
          if (onError)
            onError(user, msg.c_str());
        },
        [this](double fps, int64_t frames, int w, int h) {
          if (onOpened)
            onOpened(user, fps, frames, w, h);
        },
        [this]() {
          if (onPlayFinished)
            onPlayFinished(user);
        });
  }
};

Session* as(VbSession s) { return static_cast<Session*>(s); }

}  // namespace

extern "C" {

VbSession vb_session_create(void) { return new Session(); }

void vb_session_destroy(VbSession session) {
  auto* s = as(session);
  if (!s)
    return;
  s->preview.closeVideo();
  s->exporter.requestCancel();
  s->exporter.join();
  delete s;
}

void vb_session_set_user(VbSession session, void* user) {
  if (auto* s = as(session))
    s->user = user;
}
void vb_session_on_frame(VbSession session, VbFrameFn fn) {
  if (auto* s = as(session))
    s->onFrame = fn;
}
void vb_session_on_tracks(VbSession session, VbTracksFn fn) {
  if (auto* s = as(session))
    s->onTracks = fn;
}
void vb_session_on_marks(VbSession session, VbMarksFn fn) {
  if (auto* s = as(session))
    s->onMarks = fn;
}
void vb_session_on_error(VbSession session, VbErrorFn fn) {
  if (auto* s = as(session))
    s->onError = fn;
}
void vb_session_on_opened(VbSession session, VbOpenedFn fn) {
  if (auto* s = as(session))
    s->onOpened = fn;
}
void vb_session_on_play_finished(VbSession session, VbVoidFn fn) {
  if (auto* s = as(session))
    s->onPlayFinished = fn;
}
void vb_session_on_export_progress(VbSession session, VbProgressFn fn) {
  if (auto* s = as(session))
    s->onExportProgress = fn;
}
void vb_session_on_export_done(VbSession session, VbDoneFn fn) {
  if (auto* s = as(session))
    s->onExportDone = fn;
}

int vb_open(VbSession session, const char* videoUtf8) {
  auto* s = as(session);
  if (!s || !videoUtf8)
    return 0;
  s->videoPath = videoUtf8;
  s->preview.setFacesEnabled(s->faces);
  s->preview.setYoloEnabled(s->yolo);
  s->preview.setRedactionMode(static_cast<vb::RedactionMode>(s->mode));
  return s->preview.openVideo(videoUtf8, vb::defaultFaceModelPath(),
                              vb::defaultYoloModelPath(),
                              vb::defaultLandmarksModelPath(),
                              vb::defaultReidModelPath())
             ? 1
             : 0;
}

void vb_close(VbSession session) {
  if (auto* s = as(session))
    s->preview.closeVideo();
}
void vb_play(VbSession session) {
  if (auto* s = as(session))
    s->preview.play();
}
void vb_pause(VbSession session) {
  if (auto* s = as(session))
    s->preview.pause();
}
void vb_seek(VbSession session, long long frameIndex) {
  if (auto* s = as(session))
    s->preview.seek(frameIndex);
}
void vb_set_mode(VbSession session, int mode) {
  auto* s = as(session);
  if (!s)
    return;
  s->mode = mode;
  s->preview.setRedactionMode(static_cast<vb::RedactionMode>(mode));
  vb::Settings::instance().setRedactionMode(vb::modeName(static_cast<vb::RedactionMode>(mode)));
}
void vb_set_faces(VbSession session, int enabled) {
  if (auto* s = as(session)) {
    s->faces = enabled != 0;
    s->preview.setFacesEnabled(s->faces);
  }
}
void vb_set_yolo(VbSession session, int enabled) {
  if (auto* s = as(session)) {
    s->yolo = enabled != 0;
    s->preview.setYoloEnabled(s->yolo);
  }
}
void vb_set_gpu(VbSession session, int enabled) {
  (void)session;
  vb::Settings::instance().setHardwareAccel(enabled != 0);
}
void vb_set_score(VbSession session, float score) {
  auto* s = as(session);
  if (!s)
    return;
  s->preview.setScoreThreshold(score);
  vb::Settings::instance().setScoreThreshold(score);
}
void vb_add_manual(VbSession session, int x, int y, int w, int h) {
  if (auto* s = as(session))
    s->preview.addManualTrack(cv::Rect(x, y, w, h));
}
void vb_add_static(VbSession session, int x, int y, int w, int h,
                   long long startFrame, long long endFrame) {
  if (auto* s = as(session))
    s->preview.addStaticRegion(cv::Rect(x, y, w, h), startFrame, endFrame);
}
void vb_set_track_enabled(VbSession session, int id, int enabled) {
  if (auto* s = as(session))
    s->preview.setTrackEnabled(id, enabled != 0);
}

void vb_set_face_policy(VbSession session, int policy) {
  if (auto* s = as(session))
    s->preview.setFacePolicy(policy == 1 ? vb::FacePolicy::ReverseKeep
                                         : vb::FacePolicy::FullRedact);
}

void vb_enroll_keep_at(VbSession session, int x, int y) {
  if (auto* s = as(session))
    s->preview.enrollKeepAt(x, y);
}

void vb_confirm_identity(VbSession session, int trackId) {
  if (auto* s = as(session))
    s->preview.confirmIdentity(trackId);
}

void vb_reject_identity(VbSession session, int trackId) {
  if (auto* s = as(session))
    s->preview.rejectIdentity(trackId);
}

void vb_clear_gallery(VbSession session) {
  if (auto* s = as(session))
    s->preview.clearGallery();
}

const char* vb_runtime_status(VbSession session) {
  auto* s = as(session);
  static thread_local std::string status = "GPU (DirectML)";
  if (!s)
    return status.c_str();
  status = s->preview.runtimeStatus();
  return status.c_str();
}

const char* vb_dump_detect_debug(VbSession session) {
  auto* s = as(session);
  static thread_local std::string path;
  path.clear();
  if (!s)
    return path.c_str();
  path = s->preview.dumpDetectDebug();
  return path.c_str();
}

int vb_export(VbSession session, const char* outputUtf8, int watermark) {
  auto* s = as(session);
  if (!s || !outputUtf8 || s->videoPath.empty())
    return 0;
  if (s->exporter.running())
    return 0;
  s->exporter.configure(s->videoPath, outputUtf8, vb::defaultFaceModelPath(),
                        vb::defaultYoloModelPath(), vb::defaultLandmarksModelPath(),
                        vb::defaultReidModelPath(),
                        static_cast<vb::RedactionMode>(s->mode), s->faces, s->yolo,
                        s->preview.facePolicy(), s->preview.gallery(),
                        s->preview.snapshot(), {}, watermark != 0);
  s->exporter.start(
      [s](int pct, const std::string& status) {
        if (s->onExportProgress)
          s->onExportProgress(s->user, pct, status.c_str());
      },
      [s](bool ok, const std::string& pathOrError) {
        if (s->onExportDone)
          s->onExportDone(s->user, ok ? 1 : 0, pathOrError.c_str());
      });
  return 1;
}

void vb_cancel_export(VbSession session) {
  if (auto* s = as(session))
    s->exporter.requestCancel();
}

const char* vb_face_model_path(void) {
  static std::string p = vb::defaultFaceModelPath();
  return p.c_str();
}
const char* vb_yolo_model_path(void) {
  static std::string p = vb::defaultYoloModelPath();
  return p.c_str();
}
const char* vb_hwid(void) {
  static std::string id = vb::currentHwId();
  return id.c_str();
}

const char* vb_hardware_summary(void) {
  static std::string s = vb::HardwareInfo::instance().summary();
  return s.c_str();
}

}  // extern "C"
