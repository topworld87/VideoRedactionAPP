#pragma once

#include "vb/TrackObject.h"

#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <vector>

namespace vb {

struct FramePacket {
  cv::Mat bgr;
  cv::Mat rgb;
  int64_t frameIndex = 0;
  double ptsSec = 0.0;
  std::vector<TrackObject> boxes;
};

using FramePacketPtr = std::shared_ptr<FramePacket>;

}  // namespace vb
