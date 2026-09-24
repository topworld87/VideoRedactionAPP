#pragma once

#include "vb/OrtRuntime.h"
#include "vb/TrackObject.h"

#include <opencv2/core.hpp>
#include <set>
#include <string>
#include <vector>

namespace vb {

struct YoloDetection {
  cv::Rect rect;
  int classId = -1;
  float score = 0.f;
  std::string label;
};

class YoloDetectorDnn {
public:
  bool load(const std::string& onnxPathUtf8, bool tryGpu = true);
  bool isReady() const { return ready_; }
  OrtEpKind ep() const { return session_.ep(); }

  void setScoreThreshold(float v) { scoreThreshold_ = v; }
  void setNmsThreshold(float v) { nmsThreshold_ = v; }
  void setInputSize(int s) { inputSize_ = s; }

  void setAllowedClasses(const std::set<int>& ids) { allowed_ = ids; }
  void useDefaultRedactionClasses();

  std::vector<YoloDetection> detect(const cv::Mat& bgrFrame);
  /// Letterbox several frames (CPU, parallel) and run one GPU batch.
  /// Falls back to per-frame detect() if DirectML rejects batch > 1.
  std::vector<std::vector<YoloDetection>>
  detectMany(const std::vector<cv::Mat>& frames);

  static std::vector<YoloDetection>
  toPlateZones(const std::vector<YoloDetection>& dets, float bottomRatio = 0.35f);

  static bool isScreenClass(int classId);
  static bool isVehicleClass(int classId);
  static bool isPersonClass(int classId);
  static std::string className(int classId);
  static cv::Rect personHeadRoi(const cv::Rect& person, int frameW, int frameH);
  /// Tight face-sized crop at the top of a person box (for redaction fallback).
  static cv::Rect personFaceRoi(const cv::Rect& person, int frameW, int frameH);
  static cv::Rect zoomAround(const cv::Rect& box, int frameW, int frameH);
  static std::vector<cv::Rect>
  personHeadRois(const std::vector<YoloDetection>& dets, int frameW, int frameH,
                 int maxRois = 10);
  static std::vector<cv::Rect>
  faceZoomRois(const std::vector<YoloDetection>& dets,
               const std::vector<cv::Rect>& followBoxes, int frameW, int frameH,
               int maxRois = 12);
  /// Head crops for every person (no max). Used to snap coasting face tracks
  /// and as synthetic face boxes when the face net blanks for a frame.
  static std::vector<cv::Rect>
  allPersonHeads(const std::vector<YoloDetection>& dets, int frameW, int frameH);
  static void appendHeadFallbacks(std::vector<FaceDet>& faces,
                                  const std::vector<YoloDetection>& dets,
                                  int frameW, int frameH);

private:
  OrtModelSession session_;
  bool ready_ = false;
  float scoreThreshold_ = 0.35f;
  float nmsThreshold_ = 0.45f;
  int inputSize_ = 640;
  std::set<int> allowed_;
  cv::Mat letterbox_;
  cv::Mat inferBlob_;
  cv::Mat batchBlob_;
  bool batchOk_ = true;
};

}  // namespace vb
