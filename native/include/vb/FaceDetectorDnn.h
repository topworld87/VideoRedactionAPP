#pragma once

#include "vb/OrtRuntime.h"
#include "vb/TrackObject.h"

#include <algorithm>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace vb {

class FaceDetectorDnn {
public:
  bool load(const std::string& onnxPathUtf8, bool tryGpu = true);
  bool isReady() const { return ready_; }
  OrtEpKind ep() const { return session_.ep(); }

  void setScoreThreshold(float v) { scoreThreshold_ = v; }
  float scoreThreshold() const { return scoreThreshold_; }
  void setNmsThreshold(float v) { nmsThreshold_ = v; }
  void setMaxTiles(int n) { maxTiles_ = std::max(1, n); }
  void setTileOverlap(float v) { tileOverlap_ = std::clamp(v, 0.15f, 0.50f); }
  bool isYuNet() const { return kind_ == Kind::YuNet; }
  bool isYoloFace() const { return kind_ == Kind::YoloFace; }
  const char* modelLabel() const;

  std::vector<FaceDet> detect(const cv::Mat& bgrFrame,
                              const std::vector<cv::Rect>& zoomRois = {},
                              bool fullFrameTiles = true);
  /// Several frames (each with its own zoom crops) in one GPU launch.
  /// Tracking stays outside. Falls back to detect() if the batch fails.
  std::vector<std::vector<FaceDet>>
  detectGrouped(const std::vector<cv::Mat>& frames,
                const std::vector<std::vector<cv::Rect>>& zoomsPerFrame);

private:
  enum class Kind { YuNet, IntelSsd, YoloFace };

  cv::Mat inferRawYun(const cv::Mat& bgr, bool allowUpscale);
  cv::Mat inferRawIntel(const cv::Mat& bgr, bool allowUpscale);
  cv::Mat inferRawYoloFace(const cv::Mat& bgr, bool allowUpscale);
  // Full frame + zoom crops in one DirectML launch (dynamic batch).
  bool inferYoloFaceBatch(const std::vector<cv::Mat>& crops,
                          const std::vector<int>& shiftX,
                          const std::vector<int>& shiftY,
                          const std::vector<char>& torso,
                          cv::Mat& facesOut,
                          std::vector<cv::Mat>* perCrop = nullptr,
                          bool disableOnFail = true);
  cv::Mat inferRaw(const cv::Mat& bgr, bool allowUpscale);
  cv::Mat decodeIntelDets(const std::vector<Ort::Value>& outputs) const;
  cv::Mat decodeLegacyDets(Ort::Value& value) const;
  cv::Mat mapIntelBoxes(const cv::Mat& faces, float useScale, int padX, int padY) const;
  cv::Mat postProcessYun(const std::vector<cv::Mat>& outputBlobs, int padW, int padH,
                         float scaleX, float scaleY) const;
  cv::Mat nmsFaces(const cv::Mat& faces) const;
  std::vector<FaceDet> finalizeRects(const cv::Mat& faces, int srcW, int srcH) const;
  std::vector<cv::Rect> nativeTiles(int srcW, int srcH) const;
  std::vector<cv::Rect> limitedTiles(const std::vector<cv::Rect>& tiles) const;
  void detectKind();

  OrtModelSession session_;
  Kind kind_ = Kind::YuNet;
  bool ready_ = false;
  float scoreThreshold_ = 0.45f;
  float nmsThreshold_ = 0.42f;
  int topK_ = 80;
  int maxTiles_ = 6;
  float tileOverlap_ = 0.40f;
  int inputW_ = 640;
  int inputH_ = 640;
  static constexpr int kYunInput_ = 640;
  static constexpr int kYoloInput_ = 640;
  const std::vector<int> strides_{8, 16, 32};
  cv::Mat inferPad_;
  cv::Mat inferBlob_;
  cv::Mat letterbox_;
  cv::Mat batchBlob_;
  cv::Mat batchBlobAlt_;
  bool yoloBatchOk_ = true;
};

}  // namespace vb
