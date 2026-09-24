#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef VB_CORE_EXPORTS
#define VB_API __declspec(dllexport)
#else
#define VB_API __declspec(dllimport)
#endif
#else
#define VB_API
#endif

typedef void* VbSession;

typedef struct VbTrackC {
  int id;
  int type;
  int x;
  int y;
  int w;
  int h;
  int enabled;
  int lost;
  char label[80];
  int identityId;
  int matchTier;
  float reidScore;
} VbTrackC;

typedef void (*VbFrameFn)(void* user, const unsigned char* rgb, int width,
                          int height, int stride, long long frameIndex,
                          double ptsSec);
typedef void (*VbTracksFn)(void* user, const VbTrackC* tracks, int count);
typedef void (*VbMarksFn)(void* user, const long long* frames, int count);
typedef void (*VbErrorFn)(void* user, const char* messageUtf8);
typedef void (*VbOpenedFn)(void* user, double fps, long long frames, int width,
                           int height);
typedef void (*VbVoidFn)(void* user);
typedef void (*VbProgressFn)(void* user, int percent, const char* statusUtf8);
typedef void (*VbDoneFn)(void* user, int ok, const char* pathOrErrorUtf8);

VB_API VbSession vb_session_create(void);
VB_API void vb_session_destroy(VbSession session);

VB_API void vb_session_set_user(VbSession session, void* user);
VB_API void vb_session_on_frame(VbSession session, VbFrameFn fn);
VB_API void vb_session_on_tracks(VbSession session, VbTracksFn fn);
VB_API void vb_session_on_marks(VbSession session, VbMarksFn fn);
VB_API void vb_session_on_error(VbSession session, VbErrorFn fn);
VB_API void vb_session_on_opened(VbSession session, VbOpenedFn fn);
VB_API void vb_session_on_play_finished(VbSession session, VbVoidFn fn);
VB_API void vb_session_on_export_progress(VbSession session, VbProgressFn fn);
VB_API void vb_session_on_export_done(VbSession session, VbDoneFn fn);

VB_API int vb_open(VbSession session, const char* videoUtf8);
VB_API void vb_close(VbSession session);
VB_API void vb_play(VbSession session);
VB_API void vb_pause(VbSession session);
VB_API void vb_seek(VbSession session, long long frameIndex);
VB_API void vb_set_mode(VbSession session, int mode);  // 0 mosaic 1 blur 2 solid
VB_API void vb_set_faces(VbSession session, int enabled);
VB_API void vb_set_yolo(VbSession session, int enabled);
VB_API void vb_set_gpu(VbSession session, int enabled);
VB_API void vb_set_score(VbSession session, float score);
VB_API void vb_add_manual(VbSession session, int x, int y, int w, int h);
VB_API void vb_add_static(VbSession session, int x, int y, int w, int h,
                          long long startFrame, long long endFrame);
VB_API void vb_set_track_enabled(VbSession session, int id, int enabled);
VB_API void vb_set_face_policy(VbSession session, int policy);
VB_API void vb_enroll_keep_at(VbSession session, int x, int y);
VB_API void vb_confirm_identity(VbSession session, int trackId);
VB_API void vb_reject_identity(VbSession session, int trackId);
VB_API void vb_clear_gallery(VbSession session);
VB_API const char* vb_runtime_status(VbSession session);
VB_API const char* vb_dump_detect_debug(VbSession session);
VB_API int vb_export(VbSession session, const char* outputUtf8, int watermark);
VB_API void vb_cancel_export(VbSession session);

VB_API const char* vb_face_model_path(void);
VB_API const char* vb_yolo_model_path(void);
VB_API const char* vb_hwid(void);
VB_API const char* vb_hardware_summary(void);

#ifdef __cplusplus
}
#endif
