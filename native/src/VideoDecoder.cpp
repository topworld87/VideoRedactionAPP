#include "vb/VideoDecoder.h"

#include "vb/fs.h"

namespace vb {

bool VideoDecoder::open(const std::string& pathUtf8) {
  close();
  if (!cap_.open(pathUtf8, cv::CAP_FFMPEG))
    return false;

  cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);
  fps_ = cap_.get(cv::CAP_PROP_FPS);
  if (fps_ <= 1e-3)
    fps_ = 25.0;
  width_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH));
  height_ = static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT));
  frameCount_ = static_cast<int64_t>(cap_.get(cv::CAP_PROP_FRAME_COUNT));
  bitrateKbps_ = cap_.get(cv::CAP_PROP_BITRATE);
  if (bitrateKbps_ < 0.0)
    bitrateKbps_ = 0.0;
  currentFrame_ = 0;
  return cap_.isOpened() && width_ > 0 && height_ > 0;
}

void VideoDecoder::close() {
  if (cap_.isOpened())
    cap_.release();
  currentFrame_ = 0;
}

bool VideoDecoder::isOpened() const { return cap_.isOpened(); }

bool VideoDecoder::read(cv::Mat& bgrOut) {
  if (!cap_.isOpened())
    return false;
  if (!cap_.read(bgrOut) || bgrOut.empty())
    return false;
  ++currentFrame_;
  return true;
}

bool VideoDecoder::seekFrame(int64_t frameIndex) {
  if (!cap_.isOpened())
    return false;
  if (frameIndex < 0)
    frameIndex = 0;
  if (frameCount_ > 0 && frameIndex >= frameCount_)
    frameIndex = frameCount_ - 1;
  const bool ok =
      cap_.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(frameIndex));
  if (ok)
    currentFrame_ = frameIndex;
  return ok;
}

}  // namespace vb
