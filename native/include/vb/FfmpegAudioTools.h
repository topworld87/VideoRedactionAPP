#pragma once

#include "vb/AudioRedactionItem.h"

#include <string>
#include <vector>

namespace vb {

class FfmpegAudioTools {
public:
  static std::string findFfmpeg();
  static bool remuxWithAudioRedaction(const std::string& ffmpegPath,
                                      const std::string& sourceVideo,
                                      const std::string& redactedVideo,
                                      const std::string& outputPath,
                                      const std::vector<AudioRedactionItem>& ranges,
                                      std::string* errorOut = nullptr);
};

std::string defaultFaceModelPath();
std::string defaultYoloModelPath();
std::string defaultLandmarksModelPath();
std::string defaultReidModelPath();

}  // namespace vb
