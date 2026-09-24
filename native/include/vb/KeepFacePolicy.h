#pragma once

#include "vb/FaceLandmarks.h"
#include "vb/FaceReId.h"
#include "vb/IdentityGallery.h"
#include "vb/TrackManager.h"

#include <functional>
#include <string>

namespace vb {

// Same keep/redact decision for preview and export.
// A face stays clear only when this frame matches a clicked person
// (similarity >= IdentityGallery::kAutoThresh). Otherwise it is redacted.
void applyKeepFaceFrame(TrackManager& tracks, IdentityGallery& gallery, FaceReId& reid,
                        FaceLandmarks& landmarks, const cv::Mat& bgr, int64_t frameIndex,
                        const std::function<void(const std::string&)>& log = {});

}  // namespace vb
