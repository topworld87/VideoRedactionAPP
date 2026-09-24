#pragma once

#include "vb/TrackObject.h"

#include <functional>
#include <vector>

namespace vb {

class ByteTracker {
public:
  void reset();
  void setMaxAge(int frames) { maxAge_ = frames < 1 ? 1 : frames; }

  std::vector<FaceDet> update(const std::vector<FaceDet>& dets,
                              const std::function<int()>& nextId,
                              std::vector<int>* lostIds = nullptr);

  struct Track {
    int id = 0;
    cv::Rect2f rect;
    float score = 0.f;
    float vx = 0.f;
    float vy = 0.f;
    float vw = 0.f;
    float vh = 0.f;
    int miss = 0;
    bool lost = false;
  };

  const std::vector<Track>& tracks() const { return tracks_; }

private:
  static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
  static float associationScore(const Track& tr, const FaceDet& det, float minIou);
  static std::vector<int> greedyMatch(const std::vector<Track>& tracks,
                                      const std::vector<FaceDet>& dets,
                                      float minIou,
                                      const std::vector<char>& trackMask,
                                      const std::vector<char>& detMask);

  void predict();
  static void applyDetection(Track& tr, const cv::Rect2f& nr, float score);

  std::vector<Track> tracks_;
  int maxAge_ = 90;
  // Detector already gates scores (~0.22 small / ~0.45 large). A 0.50 new-track
  // gate dropped walking and distant faces that never quite reached 0.50.
  float highThresh_ = 0.22f;
  float lowThresh_ = 0.12f;
  float matchIou_ = 0.16f;
  float lowMatchIou_ = 0.08f;
};

}  // namespace vb
