#include "vb/RedactionFilter.h"

#include <opencv2/imgproc.hpp>

namespace vb {

void RedactionFilter::apply(cv::Mat& bgr, const std::vector<cv::Rect>& rects) const {
  for (const auto& r : rects)
    applyOne(bgr, r);
}

void RedactionFilter::applyOne(cv::Mat& bgr, cv::Rect rect) const {
  rect &= cv::Rect(0, 0, bgr.cols, bgr.rows);
  if (rect.width < 2 || rect.height < 2)
    return;

  cv::Mat roi = bgr(rect);
  switch (mode_) {
  case RedactionMode::Mosaic: {
    const int sw = std::max(1, rect.width / mosaicFactor_);
    const int sh = std::max(1, rect.height / mosaicFactor_);
    cv::Mat small;
    cv::resize(roi, small, cv::Size(sw, sh), 0, 0, cv::INTER_LINEAR);
    cv::resize(small, roi, rect.size(), 0, 0, cv::INTER_NEAREST);
    break;
  }
  case RedactionMode::Blur:
    cv::GaussianBlur(roi, roi, cv::Size(blurKernel_, blurKernel_), 0);
    break;
  case RedactionMode::Solid:
    roi.setTo(solidColor_);
    break;
  }
}

}  // namespace vb
