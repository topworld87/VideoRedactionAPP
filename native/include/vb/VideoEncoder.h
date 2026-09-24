#pragma once

#include "vb/platform/ChildProcess.h"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <string>

namespace vb {

class VideoEncoder {
public:
  VideoEncoder() = default;
  ~VideoEncoder() { close(); }

  VideoEncoder(const VideoEncoder&) = delete;
  VideoEncoder& operator=(const VideoEncoder&) = delete;

  bool open(const std::string& pathUtf8, double fps, int width, int height,
            double sourceBitrateKbps = 0.0);
  void close();
  bool isOpened() const;
  bool write(const cv::Mat& bgr);

  const std::string& backendLabel() const { return backendLabel_; }
  const std::string& lastError() const { return lastError_; }
  int bitrateKbps() const { return bitrateKbps_; }

private:
  static int targetBitrateKbps(int width, int height, double fps,
                               double sourceBitrateKbps);
  bool openFfmpegPipe(const std::string& pathUtf8, double fps, int width,
                      int height, int bitrateKbps);
  bool tryStartFfmpeg(const std::string& ffmpegPath, const std::string& codec,
                      const std::string& pathUtf8, double fps, int width, int height,
                      int bitrateKbps);
  bool probeCodec(const std::string& ffmpegPath, const std::string& codec, double fps,
                  int width, int height, int bitrateKbps);
  bool openOpenCv(const std::string& pathUtf8, double fps, int width, int height,
                  int bitrateKbps);
  static std::string pickFfmpegVideoCodec(const std::string& ffmpegPath);

  ChildProcess ffmpeg_;
  cv::VideoWriter writer_;
  cv::Mat writeScratch_;
  bool useFfmpeg_ = false;
  int width_ = 0;
  int height_ = 0;
  int bitrateKbps_ = 0;
  std::string backendLabel_;
  std::string lastError_;
};

}  // namespace vb
