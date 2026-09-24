#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace vb {

enum class TrackType { AiFace, AiObject, ManualTrack, StaticRegion };

enum class FacePolicy { FullRedact = 0, ReverseKeep = 1 };

enum class MatchTier { None = 0, Enrolled = 1, AutoLinked = 2, Suggest = 3 };

struct FaceDet {
  cv::Rect rect;
  float score = 0.f;
};

struct TrackObject {
  int id = 0;
  TrackType type = TrackType::AiFace;
  cv::Rect rect;
  bool enabled = true;
  int64_t startFrame = 0;
  int64_t endFrame = -1;
  float score = 0.f;
  bool lost = false;
  int missCount = 0;
  int64_t lastSeenFrame = 0;
  std::string label;
  int identityId = -1;
  float reidScore = 0.f;
  MatchTier matchTier = MatchTier::None;
  bool userForced = false;
  bool suggestRejected = false;
  std::vector<float> embedding;

  bool activeAt(int64_t frame) const {
    if (endFrame >= 0 && frame > endFrame)
      return false;
    return frame >= startFrame;
  }
};

}  // namespace vb
