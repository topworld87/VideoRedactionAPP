#pragma once

#include "vb/OrtRuntime.h"

#include <array>
#include <opencv2/core.hpp>
#include <string>

namespace vb {

class FaceLandmarks {
public:
  bool load(const std::string& onnxPathUtf8, bool tryGpu = true);
  bool isReady() const { return ready_; }
  OrtEpKind ep() const { return session_.ep(); }

  bool infer(const cv::Mat& bgr, const cv::Rect& face,
             std::array<cv::Point2f, 5>& points) const;

private:
  OrtModelSession session_;
  bool ready_ = false;
  int inW_ = 48;
  int inH_ = 48;
  mutable cv::Mat blob_;
};

}  // namespace vb
