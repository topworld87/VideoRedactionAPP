#pragma once

#include "vb/ByteTrack.h"
#include "vb/CsrtTracker.h"
#include "vb/TrackObject.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vb {

class TrackManager {
public:
  void reset();
  // Scrub is not continuous motion. Drop live face association so a kept
  // face cannot stick to someone else at the new time.
  void breakFaceContinuity();
  void setKeepDebug(bool on) { keepDebug_ = on; }
  // When false, a new face does not copy "keep clear" from a nearby face.
  // Keep-people mode decides that by similarity, not screen distance.
  void setProximityKeepInherit(bool on) { proximityKeepInherit_ = on; }
  // Drop an automatic keep and redact this face again. Ignores user-clicked tracks.
  void releaseKeep(int id);
  void setEofFrame(int64_t eof) { eofFrame_ = eof; }

  std::vector<TrackObject>
  processFrame(const cv::Mat& bgr, int64_t frameIndex,
               const std::vector<FaceDet>& aiFaces,
               const std::vector<std::pair<cv::Rect, std::string>>& aiObjects = {},
               std::vector<cv::Rect>* newlyVisible = nullptr,
               const std::vector<cv::Rect>& personHeads = {});

  static constexpr int kAppearLookaheadFrames = 4;

  int addManualTrack(const cv::Mat& bgr, const cv::Rect& rect, int64_t frameIndex);
  int addStaticRegion(const cv::Rect& rect, int64_t startFrame, int64_t endFrame);

  void setEnabled(int id, bool enabled, bool userForced = true);
  void setIdentity(int id, int identityId, MatchTier tier, float score);
  void setEmbedding(int id, std::vector<float> embedding);
  void rejectSuggestion(int id);
  int trackIdAt(int x, int y, TrackType type = TrackType::AiFace) const;
  bool get(int id, TrackObject& out) const;

  void importAiSnapshot(const std::vector<TrackObject>& snap, bool importFaces = true);
  std::vector<TrackObject> snapshot() const;
  std::vector<TrackObject> visibleAt(int64_t frameIndex) const;
  std::vector<cv::Rect> zoomFollowBoxes() const;

private:
  static float iou(const cv::Rect& a, const cv::Rect& b);
  static float centerDistance(const cv::Rect& a, const cv::Rect& b);
  int nextId();
  void syncFaces(const std::vector<FaceDet>& dets, int64_t frameIndex,
                 const std::vector<cv::Rect>& personHeads = {});
  static bool faceVisibleForRedact(const TrackObject& obj);
  static cv::Rect inflateCoastBox(const cv::Rect& r, float vx, float vy, int miss);
  static std::string withId(const std::string& prefix, int id);

  mutable std::mutex mutex_;
  std::vector<TrackObject> objects_;
  std::unordered_map<int, std::shared_ptr<CsrtTracker>> manualTrackers_;
  std::unordered_map<int, bool> wasVisible_;
  ByteTracker faceTracker_;
  bool keepDebug_ = false;
  bool proximityKeepInherit_ = true;
  int nextId_ = 1;
  int64_t eofFrame_ = -1;
};

}  // namespace vb
