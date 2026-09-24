#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>

namespace vb {

class VideoDecoder {
public:
  bool open(const std::string& pathUtf8);
  void close();
  bool isOpened() const;

  bool read(cv::Mat& bgrOut);
  bool seekFrame(int64_t frameIndex);

  double fps() const { return fps_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int64_t frameCount() const { return frameCount_; }
  int64_t currentFrame() const { return currentFrame_; }
  double bitrateKbps() const { return bitrateKbps_; }

private:
  cv::VideoCapture cap_;
  double fps_ = 25.0;
  int width_ = 0;
  int height_ = 0;
  int64_t frameCount_ = 0;
  int64_t currentFrame_ = 0;
  double bitrateKbps_ = 0.0;
};

}  // namespace vb
