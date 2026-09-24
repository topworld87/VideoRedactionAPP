#include "vb/KeepFacePolicy.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace vb {
namespace {

std::vector<float> embedFace(FaceReId& reid, FaceLandmarks& landmarks, const cv::Mat& bgr,
                             const cv::Rect& face) {
  if (!reid.isReady() || bgr.empty())
    return {};
  std::array<cv::Point2f, 5> pts{};
  const std::array<cv::Point2f, 5>* lmk = nullptr;
  if (landmarks.isReady() && landmarks.infer(bgr, face, pts))
    lmk = &pts;
  return reid.embed(bgr, face, lmk);
}

void logLine(const std::function<void(const std::string&)>& log, const std::string& line) {
  if (log)
    log(line);
}

}  // namespace

void applyKeepFaceFrame(TrackManager& tracks, IdentityGallery& gallery, FaceReId& reid,
                        FaceLandmarks& landmarks, const cv::Mat& bgr, int64_t frameIndex,
                        const std::function<void(const std::string&)>& log) {
  if (gallery.empty() || !reid.isReady())
    return;

  for (const auto& obj : tracks.snapshot()) {
    if (obj.type != TrackType::AiFace || obj.lost || obj.suggestRejected)
      continue;

    const bool linked =
        obj.matchTier == MatchTier::AutoLinked || obj.matchTier == MatchTier::Enrolled;
    const bool newborn = obj.startFrame == frameIndex;
    const bool periodic = (frameIndex % 8) == 0;
    // A matched track stays clear between checks. A new face is checked at once.
    if (linked && !periodic)
      continue;
    if (!linked && !newborn && !periodic)
      continue;

    const bool wasClear = !obj.enabled;
    auto redact = [&](const char* why, float score) {
      if (!linked && !wasClear && obj.matchTier == MatchTier::None)
        return;
      std::ostringstream line;
      line << "REDACT frame=" << frameIndex << " id=" << obj.id << " why=" << why
           << " score=" << score;
      logLine(log, line.str());
      tracks.releaseKeep(obj.id);
    };

    if (std::min(obj.rect.width, obj.rect.height) < 32) {
      if (!linked)
        redact("small", 0.f);
      continue;
    }

    auto emb = embedFace(reid, landmarks, bgr, obj.rect);
    if (emb.empty()) {
      if (!linked)
        redact("no_embedding", 0.f);
      continue;
    }
    tracks.setEmbedding(obj.id, emb);
    const auto match = gallery.query(emb);

    if (match.tier == MatchTier::AutoLinked) {
      const MatchTier tier = obj.userForced ? MatchTier::Enrolled : MatchTier::AutoLinked;
      if (!wasClear || obj.identityId != match.identityId) {
        std::ostringstream line;
        line << "KEEP frame=" << frameIndex << " id=" << obj.id << " ident=" << match.identityId
             << " score=" << match.score;
        logLine(log, line.str());
      }
      tracks.setIdentity(obj.id, match.identityId, tier, match.score);
      continue;
    }

    // Same track, score still in the uncertain band: keep the previous decision.
    if (linked && match.score >= IdentityGallery::kSuggestThresh)
      continue;

    if (!linked && match.tier == MatchTier::Suggest) {
      tracks.setIdentity(obj.id, match.identityId, MatchTier::Suggest, match.score);
      continue;
    }

    redact(linked ? "lost_match" : "below", match.score);
  }
}

}  // namespace vb
