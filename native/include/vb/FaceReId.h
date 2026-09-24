#pragma once

#include "vb/OrtRuntime.h"

#include <array>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace vb {

class FaceReId {
public:
  bool load(const std::string& onnxPathUtf8, bool tryGpu = true);
  bool isReady() const { return ready_; }
  OrtEpKind ep() const { return session_.ep(); }

  std::vector<float> embed(const cv::Mat& bgr, const cv::Rect& face,
                           const std::array<cv::Point2f, 5>* landmarks) const;

  static cv::Mat alignFace(const cv::Mat& bgr, const std::array<cv::Point2f, 5>& pts,
                           int outW, int outH);
  static std::vector<float> l2(const std::vector<float>& v);

private:
  OrtModelSession session_;
  bool ready_ = false;
  int inW_ = 128;
  int inH_ = 128;
  mutable cv::Mat blob_;
};

}  // namespace vb
