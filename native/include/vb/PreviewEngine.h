#pragma once

#include "vb/AudioRedactionItem.h"
#include "vb/FaceDetectorDnn.h"
#include "vb/FaceLandmarks.h"
#include "vb/FaceReId.h"
#include "vb/FramePacket.h"
#include "vb/IdentityGallery.h"
#include "vb/RedactionFilter.h"
#include "vb/TrackManager.h"
#include "vb/TrackObject.h"
#include "vb/VideoDecoder.h"
#include "vb/YoloDetectorDnn.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vb {

class PreviewEngine {
public:
  using FrameFn = std::function<void(const FramePacket&)>;
  using TracksFn = std::function<void(const std::vector<TrackObject>&)>;
  using MarksFn = std::function<void(const std::vector<int64_t>&)>;
  using ErrorFn = std::function<void(const std::string&)>;
  using OpenedFn = std::function<void(double fps, int64_t frames, int w, int h)>;
  using VoidFn = std::function<void()>;

  PreviewEngine();
  ~PreviewEngine();

  void setCallbacks(FrameFn onFrame, TracksFn onTracks, MarksFn onMarks,
                    ErrorFn onError, OpenedFn onOpened, VoidFn onFinished);

  bool openVideo(const std::string& pathUtf8, const std::string& faceModel,
                 const std::string& yoloModel,
                 const std::string& landmarksModel = {},
                 const std::string& reidModel = {});
  void closeVideo();

  void play();
  void pause();
  void seek(int64_t frameIndex);
  void setRedactionMode(RedactionMode mode);
  void setScoreThreshold(float v);
  void setFacesEnabled(bool enabled);
  void setYoloEnabled(bool enabled);
  void setFacePolicy(FacePolicy policy);

  void addManualTrack(const cv::Rect& rect);
  void addStaticRegion(const cv::Rect& rect, int64_t startFrame, int64_t endFrame);
  void setTrackEnabled(int id, bool enabled);
  void enrollKeepAt(int x, int y);
  void enrollKeepTrack(int trackId);
  void confirmIdentity(int trackId);
  void rejectIdentity(int trackId);
  void clearGallery();

  double fps() const { return decoder_.fps(); }
  int64_t frameCount() const { return decoder_.frameCount(); }
  int videoWidth() const { return decoder_.width(); }
  int videoHeight() const { return decoder_.height(); }
  bool isPlaying() const { return playing_.load(); }
  bool hasVideo() const { return hasVideo_.load(); }
  FacePolicy facePolicy() const { return facePolicy_.load(); }
  std::string runtimeStatus() const;
  /// Dump current frame + detection overlays for offline diagnosis.
  /// Returns UTF-8 folder path on success, or empty string on failure.
  std::string dumpDetectDebug();
  IdentityGallery& gallery() { return gallery_; }
  const IdentityGallery& gallery() const { return gallery_; }

  TrackManager& tracks() { return tracks_; }
  std::vector<TrackObject> snapshot() const { return tracks_.snapshot(); }

private:
  void threadMain();
  bool processOneFrame(bool allowDelay);
  void emitDelayedFrame(cv::Mat cleanBgr, int64_t index,
                        const std::vector<cv::Rect>& redact,
                        const std::vector<TrackObject>& boxes);
  void flushDelayQueue(bool emitFrames);
  void emitTracksUpdated(bool force);
  void applyReversePolicy(const cv::Mat& bgr, int64_t frameIndex);
  std::vector<float> extractEmbedding(const cv::Mat& bgr, const cv::Rect& face,
                                      bool* landmarksOk = nullptr) const;
  void refreshPreview();

  struct PendingManual {
    cv::Rect rect;
  };
  struct PendingStatic {
    cv::Rect rect;
    int64_t start = 0;
    int64_t end = -1;
  };
  struct DelayedFrame {
    cv::Mat cleanBgr;
    int64_t index = 0;
    std::vector<cv::Rect> redact;
    std::vector<TrackObject> boxes;
  };

  VideoDecoder decoder_;
  FaceDetectorDnn detector_;
  FaceLandmarks landmarks_;
  FaceReId reid_;
  IdentityGallery gallery_;
  YoloDetectorDnn yolo_;
  RedactionFilter filter_;
  TrackManager tracks_;

  FrameFn onFrame_;
  TracksFn onTracks_;
  MarksFn onMarks_;
  ErrorFn onError_;
  OpenedFn onOpened_;
  VoidFn onFinished_;

  std::mutex cmdMutex_;
  std::vector<PendingManual> pendingManual_;
  std::vector<PendingStatic> pendingStatic_;
  std::vector<int64_t> faceMarks_;

  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> seekRequested_{false};
  std::atomic<int64_t> seekTarget_{0};
  std::atomic<bool> hasVideo_{false};
  std::atomic<bool> facesEnabled_{true};
  std::atomic<bool> yoloEnabled_{true};
  std::atomic<FacePolicy> facePolicy_{FacePolicy::FullRedact};

  cv::Mat lastBgr_;
  int64_t lastIndex_ = 0;
  int64_t lastProcessed_ = -1;
  bool loggingKeep_ = false;
  std::deque<DelayedFrame> delayQueue_;
  std::chrono::steady_clock::time_point lastTracksEmit_{};
  bool tracksEmitTimerStarted_ = false;
};

}  // namespace vb
