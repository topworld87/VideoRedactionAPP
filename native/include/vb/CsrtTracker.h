#pragma once

#include <opencv2/core.hpp>
#include <opencv2/video/tracking.hpp>
#include <memory>
#include <vector>

namespace vb {

class CsrtTracker {
public:
  bool init(const cv::Mat& frame, const cv::Rect& bbox);
  bool update(const cv::Mat& frame, cv::Rect& bbox);
  void reset();

  static cv::Rect expand(const cv::Rect& r, float frac, int frameW, int frameH);

  static std::vector<cv::Rect>
  trackOlderFrames(const cv::Mat& seedFrame, const cv::Rect& seed,
                   const std::vector<const cv::Mat*>& olderOldestFirst);

  static void backfillAppearances(const cv::Mat& now,
                                  const std::vector<cv::Rect>& appeared,
                                  const std::vector<cv::Mat*>& older,
                                  const std::vector<std::vector<cv::Rect>*>& redact,
                                  int stampLastN);

private:
  cv::Ptr<cv::Tracker> tracker_;
};

}  // namespace vb
