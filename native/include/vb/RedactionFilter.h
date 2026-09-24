#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace vb {

enum class RedactionMode { Mosaic, Blur, Solid };

class RedactionFilter {
public:
  void setMode(RedactionMode mode) { mode_ = mode; }
  RedactionMode mode() const { return mode_; }

  void setMosaicFactor(int factor) { mosaicFactor_ = std::max(4, factor); }
  void setBlurKernel(int k) { blurKernel_ = (k % 2 == 0) ? k + 1 : k; }
  void setSolidColor(cv::Scalar color) { solidColor_ = color; }

  void apply(cv::Mat& bgr, const std::vector<cv::Rect>& rects) const;

private:
  void applyOne(cv::Mat& bgr, cv::Rect rect) const;

  RedactionMode mode_ = RedactionMode::Mosaic;
  int mosaicFactor_ = 15;
  int blurKernel_ = 31;
  cv::Scalar solidColor_{0, 0, 0};
};

inline RedactionMode modeFromName(const std::string& name) {
  if (name == "blur")
    return RedactionMode::Blur;
  if (name == "solid")
    return RedactionMode::Solid;
  return RedactionMode::Mosaic;
}

inline const char* modeName(RedactionMode mode) {
  switch (mode) {
  case RedactionMode::Blur:
    return "blur";
  case RedactionMode::Solid:
    return "solid";
  default:
    return "mosaic";
  }
}

}  // namespace vb
