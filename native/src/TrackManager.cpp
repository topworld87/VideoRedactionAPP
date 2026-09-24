#include "vb/TrackManager.h"

#include "vb/fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>

namespace vb {
namespace {
constexpr float kMatchIou = 0.22f;
constexpr float kSoftIou = 0.08f;
constexpr float kSoftCenterPx = 120.f;
constexpr int kCoastFrames = 90;
// Keep ID for matching much longer than we keep painting a coasting box.
// After a few missed detections the predicted box drifts off the face.
constexpr int kRedactCoastFrames = 2;

cv::Rect padForLookahead(const cv::Rect& r) {
  const int dx = std::max(6, r.width / 5);
  const int dy = std::max(6, r.height / 5);
  return cv::Rect(r.x - dx, r.y - dy, r.width + 2 * dx, r.height + 2 * dy);
}

cv::Rect toRect(const cv::Rect2f& r) {
  return cv::Rect(static_cast<int>(std::round(r.x)), static_cast<int>(std::round(r.y)),
                  std::max(1, static_cast<int>(std::round(r.width))),
                  std::max(1, static_cast<int>(std::round(r.height))));
}
}  // namespace

bool TrackManager::faceVisibleForRedact(const TrackObject& obj) {
  if (obj.type != TrackType::AiFace)
    return !obj.lost;
  if (obj.lost)
    return false;
  return obj.missCount <= kRedactCoastFrames;
}

cv::Rect TrackManager::inflateCoastBox(const cv::Rect& r, float vx, float vy, int miss) {
  if (miss <= 0)
    return r;
  // Keep nearly face-sized: only nudge along motion, tiny pad.
  const int shiftX = static_cast<int>(std::lround(vx * static_cast<float>(miss)));
  const int shiftY = static_cast<int>(std::lround(vy * static_cast<float>(miss)));
  const int pad = std::max(3, std::min(r.width, r.height) / 8);
  return cv::Rect(r.x + shiftX - pad, r.y + shiftY - pad, r.width + 2 * pad,
                  r.height + 2 * pad);
}

std::string TrackManager::withId(const std::string& prefix, int id) {
  return prefix + " #" + std::to_string(id);
}

void TrackManager::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  objects_.clear();
  manualTrackers_.clear();
  wasVisible_.clear();
  faceTracker_.reset();
  faceTracker_.setMaxAge(kCoastFrames);
  nextId_ = 1;
  keepDebug_ = false;
}

void TrackManager::breakFaceContinuity() {
  std::lock_guard<std::mutex> lock(mutex_);
  faceTracker_.reset();
  faceTracker_.setMaxAge(kCoastFrames);
  for (auto& obj : objects_) {
    if (obj.type != TrackType::AiFace || obj.lost)
      continue;
    obj.lost = true;
    obj.missCount = kCoastFrames + 1;
  }
}

int TrackManager::nextId() { return nextId_++; }

float TrackManager::iou(const cv::Rect& a, const cv::Rect& b) {
  const int x1 = std::max(a.x, b.x);
  const int y1 = std::max(a.y, b.y);
  const int x2 = std::min(a.x + a.width, b.x + b.width);
  const int y2 = std::min(a.y + a.height, b.y + b.height);
  const int iw = std::max(0, x2 - x1);
  const int ih = std::max(0, y2 - y1);
  const int inter = iw * ih;
  const int uni = a.area() + b.area() - inter;
  if (uni <= 0)
    return 0.f;
  return static_cast<float>(inter) / static_cast<float>(uni);
}

float TrackManager::centerDistance(const cv::Rect& a, const cv::Rect& b) {
  const float ax = a.x + a.width * 0.5f;
  const float ay = a.y + a.height * 0.5f;
  const float bx = b.x + b.width * 0.5f;
  const float by = b.y + b.height * 0.5f;
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

void TrackManager::syncFaces(const std::vector<FaceDet>& dets, int64_t frameIndex,
                             const std::vector<cv::Rect>& personHeads) {
  std::vector<int> lostIds;
  faceTracker_.update(dets, [this]() { return nextId(); }, &lostIds);

  std::unordered_map<int, TrackObject*> byId;
  for (auto& obj : objects_) {
    if (obj.type == TrackType::AiFace)
      byId[obj.id] = &obj;
  }

  for (int id : lostIds) {
    auto it = byId.find(id);
    if (it == byId.end())
      continue;
    it->second->lost = true;
    it->second->missCount = kCoastFrames + 1;
  }

  for (const auto& tr : faceTracker_.tracks()) {
    auto it = byId.find(tr.id);
    if (it != byId.end()) {
      auto& obj = *it->second;
      cv::Rect box = toRect(tr.rect);
      // Brief detector miss: snap to nearest person head so the mosaic does not
      // coast sideways off a walking face.
      if (tr.miss > 0 && !personHeads.empty()) {
        float bestDist = 1e9f;
        int best = -1;
        for (int i = 0; i < static_cast<int>(personHeads.size()); ++i) {
          const float d = centerDistance(box, personHeads[static_cast<size_t>(i)]);
          const float gate =
              0.85f * std::max(box.width, personHeads[static_cast<size_t>(i)].width) + 28.f;
          if (d < bestDist && d <= gate) {
            bestDist = d;
            best = i;
          }
        }
        if (best >= 0) {
          const cv::Rect& head = personHeads[static_cast<size_t>(best)];
          // Keep previous face size when possible — only re-center on the head.
          if (box.width > 8 && box.height > 8 &&
              box.area() < head.area() * 2) {
            const int cx = head.x + head.width / 2;
            const int cy = head.y + head.height / 2;
            box = cv::Rect(cx - box.width / 2, cy - box.height / 2, box.width,
                           box.height);
          } else {
            box = head;
          }
        } else {
          box = inflateCoastBox(box, tr.vx, tr.vy, tr.miss);
        }
      }
      if (keepDebug_ && !obj.enabled) {
        const float jump = centerDistance(obj.rect, box);
        char buf[640];
        std::snprintf(
            buf, sizeof(buf),
            "MATCH_KEEP frame=%lld id=%d tier=%d ident=%d forced=%d lost=%d jump=%.1f old=%d,%d,%d,%d new=%d,%d,%d,%d",
            static_cast<long long>(frameIndex), obj.id, static_cast<int>(obj.matchTier),
            obj.identityId, obj.userForced ? 1 : 0, obj.lost ? 1 : 0, jump, obj.rect.x,
            obj.rect.y, obj.rect.width, obj.rect.height, box.x, box.y, box.width,
            box.height);
        appendTextLog("keep_debug.log", buf);
      }
      obj.rect = box;
      obj.score = tr.score;
      obj.lost = tr.lost;
      obj.missCount = tr.miss;
      obj.lastSeenFrame = frameIndex;
      continue;
    }

    bool inheritEnabled = true;
    int inheritFrom = -1;
    float inheritDist = 0.f;
    cv::Rect inheritRect;
    int inheritTier = 0;
    const cv::Rect newborn = toRect(tr.rect);
    if (proximityKeepInherit_) {
      for (const auto& o : objects_) {
        // A lost face belongs to another time. Do not copy "keep clear"
        // onto whoever is now near that old box.
        if (o.type != TrackType::AiFace || o.enabled || o.lost)
          continue;
        const float d = centerDistance(o.rect, newborn);
        if (d <= kSoftCenterPx) {
          inheritEnabled = false;
          inheritFrom = o.id;
          inheritDist = d;
          inheritRect = o.rect;
          inheritTier = static_cast<int>(o.matchTier);
          break;
        }
      }
      if (!inheritEnabled && keepDebug_) {
        char buf[640];
        std::snprintf(
            buf, sizeof(buf),
            "INHERIT frame=%lld newId=%d fromId=%d dist=%.1f fromTier=%d fromRect=%d,%d,%d,%d newRect=%d,%d,%d,%d",
            static_cast<long long>(frameIndex), tr.id, inheritFrom, inheritDist, inheritTier,
            inheritRect.x, inheritRect.y, inheritRect.width, inheritRect.height, newborn.x,
            newborn.y, newborn.width, newborn.height);
        appendTextLog("keep_debug.log", buf);
      }
    }

    TrackObject t;
    t.id = tr.id;
    t.type = TrackType::AiFace;
    t.rect = toRect(tr.rect);
    t.score = tr.score;
    t.enabled = inheritEnabled;
    t.startFrame = frameIndex;
    t.lastSeenFrame = frameIndex;
    t.missCount = tr.miss;
    t.label = withId("Face", t.id);
    objects_.push_back(t);
    if (t.id >= nextId_)
      nextId_ = t.id + 1;
  }
}

std::vector<TrackObject> TrackManager::processFrame(
    const cv::Mat& bgr, int64_t frameIndex, const std::vector<FaceDet>& aiFaces,
    const std::vector<std::pair<cv::Rect, std::string>>& aiObjects,
    std::vector<cv::Rect>* newlyVisible, const std::vector<cv::Rect>& personHeads) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto& obj : objects_) {
    if (obj.type != TrackType::ManualTrack || !obj.enabled || obj.lost)
      continue;
    auto it = manualTrackers_.find(obj.id);
    if (it == manualTrackers_.end() || !it->second)
      continue;
    cv::Rect bb = obj.rect;
    if (it->second->update(bgr, bb)) {
      obj.rect = bb;
      obj.lost = false;
      obj.missCount = 0;
      obj.lastSeenFrame = frameIndex;
    } else {
      ++obj.missCount;
      if (obj.missCount > 30)
        obj.lost = true;
    }
  }

  syncFaces(aiFaces, frameIndex, personHeads);

  auto associateType = [&](TrackType type, const std::vector<cv::Rect>& rects,
                           const std::vector<std::string>& labels,
                           const std::string& prefix) {
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(objects_.size()); ++i) {
      if (objects_[static_cast<size_t>(i)].type != type)
        continue;
      indices.push_back(i);
    }

    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
      const auto& oa = objects_[static_cast<size_t>(a)];
      const auto& ob = objects_[static_cast<size_t>(b)];
      const int sa = (!oa.lost ? 0 : (!oa.enabled ? 1 : 2));
      const int sb = (!ob.lost ? 0 : (!ob.enabled ? 1 : 2));
      if (sa != sb)
        return sa < sb;
      return oa.id < ob.id;
    });

    std::vector<bool> used(rects.size(), false);

    auto tryMatch = [&](TrackObject& obj, float minIou, float maxCenter,
                        bool requireCenter) -> int {
      float bestScore = -1.f;
      int bestJ = -1;
      for (size_t j = 0; j < rects.size(); ++j) {
        if (used[j])
          continue;
        const float v = iou(obj.rect, rects[j]);
        const float d = centerDistance(obj.rect, rects[j]);
        if (v < minIou)
          continue;
        if (requireCenter && d > maxCenter)
          continue;
        const float score = v * 1000.f - d;
        if (score > bestScore) {
          bestScore = score;
          bestJ = static_cast<int>(j);
        }
      }
      return bestJ;
    };

    for (int oi = 0; oi < static_cast<int>(indices.size()); ++oi) {
      auto& obj = objects_[static_cast<size_t>(indices[static_cast<size_t>(oi)])];
      int bestJ = tryMatch(obj, kMatchIou, kSoftCenterPx, false);
      if (bestJ < 0 && (!obj.enabled || obj.missCount <= kCoastFrames))
        bestJ = tryMatch(obj, kSoftIou, kSoftCenterPx, true);

      if (bestJ >= 0) {
        used[static_cast<size_t>(bestJ)] = true;
        obj.rect = rects[static_cast<size_t>(bestJ)];
        obj.lost = false;
        obj.missCount = 0;
        obj.lastSeenFrame = frameIndex;
        if (bestJ < static_cast<int>(labels.size()) &&
            !labels[static_cast<size_t>(bestJ)].empty())
          obj.label = withId(labels[static_cast<size_t>(bestJ)], obj.id);
      } else {
        ++obj.missCount;
        obj.lost = obj.missCount > kCoastFrames;
      }
    }

    for (size_t j = 0; j < rects.size(); ++j) {
      if (used[j])
        continue;

      bool inheritEnabled = true;
      for (const auto& o : objects_) {
        if (o.type != type || o.enabled)
          continue;
        if (centerDistance(o.rect, rects[j]) <= kSoftCenterPx) {
          inheritEnabled = false;
          break;
        }
      }

      TrackObject t;
      t.id = nextId();
      t.type = type;
      t.rect = rects[j];
      t.enabled = inheritEnabled;
      t.startFrame = frameIndex;
      t.lastSeenFrame = frameIndex;
      t.missCount = 0;
      t.lost = false;
      const std::string base =
          (j < labels.size() && !labels[j].empty()) ? labels[j] : prefix;
      t.label = withId(base, t.id);
      objects_.push_back(t);
    }
  };

  std::vector<cv::Rect> objRects;
  std::vector<std::string> objLabels;
  objRects.reserve(aiObjects.size());
  objLabels.reserve(aiObjects.size());
  for (const auto& p : aiObjects) {
    objRects.push_back(p.first);
    objLabels.push_back(p.second);
  }
  associateType(TrackType::AiObject, objRects, objLabels, "Object");

  if (newlyVisible) {
    newlyVisible->clear();
    for (const auto& obj : objects_) {
      if (obj.type != TrackType::AiFace && obj.type != TrackType::AiObject)
        continue;
      const bool vis = obj.enabled && faceVisibleForRedact(obj);
      auto it = wasVisible_.find(obj.id);
      const bool was = it != wasVisible_.end() && it->second;
      if (vis && !was)
        newlyVisible->push_back(padForLookahead(obj.rect));
    }
  }
  wasVisible_.clear();
  for (const auto& obj : objects_) {
    if (obj.type != TrackType::AiFace && obj.type != TrackType::AiObject)
      continue;
    wasVisible_[obj.id] = obj.enabled && faceVisibleForRedact(obj);
  }

  std::vector<TrackObject> active;
  for (const auto& obj : objects_) {
    if (!faceVisibleForRedact(obj) || !obj.activeAt(frameIndex))
      continue;
    active.push_back(obj);
  }
  return active;
}

int TrackManager::addManualTrack(const cv::Mat& bgr, const cv::Rect& rect,
                                 int64_t frameIndex) {
  std::lock_guard<std::mutex> lock(mutex_);
  TrackObject t;
  t.id = nextId();
  t.type = TrackType::ManualTrack;
  t.rect = rect;
  t.enabled = true;
  t.startFrame = frameIndex;
  t.lastSeenFrame = frameIndex;
  t.label = withId("Manual", t.id);

  auto tracker = std::make_shared<CsrtTracker>();
  if (tracker->init(bgr, rect))
    manualTrackers_[t.id] = tracker;
  objects_.push_back(t);
  return t.id;
}

int TrackManager::addStaticRegion(const cv::Rect& rect, int64_t startFrame,
                                  int64_t endFrame) {
  std::lock_guard<std::mutex> lock(mutex_);
  TrackObject t;
  t.id = nextId();
  t.type = TrackType::StaticRegion;
  t.rect = rect;
  t.enabled = true;
  t.startFrame = startFrame;
  t.endFrame = endFrame;
  t.lastSeenFrame = startFrame;
  t.label = withId("Static", t.id);
  objects_.push_back(t);
  return t.id;
}

void TrackManager::setEnabled(int id, bool enabled, bool userForced) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& obj : objects_) {
    if (obj.id != id)
      continue;
    obj.enabled = enabled;
    if (userForced)
      obj.userForced = true;
    break;
  }
}

void TrackManager::setIdentity(int id, int identityId, MatchTier tier, float score) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& obj : objects_) {
    if (obj.id != id)
      continue;
    obj.identityId = identityId;
    obj.matchTier = tier;
    obj.reidScore = score;
    if (tier == MatchTier::Enrolled || tier == MatchTier::AutoLinked) {
      obj.enabled = false;
      const char* tag = tier == MatchTier::Enrolled ? "Keep" : "Auto";
      obj.label = withId(std::string(tag), identityId > 0 ? identityId : id);
    } else if (tier == MatchTier::Suggest) {
      obj.enabled = true;
      obj.label = withId("Maybe keep", identityId);
    }
    break;
  }
}

void TrackManager::setEmbedding(int id, std::vector<float> embedding) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& obj : objects_) {
    if (obj.id != id)
      continue;
    obj.embedding = std::move(embedding);
    break;
  }
}

bool TrackManager::get(int id, TrackObject& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& obj : objects_) {
    if (obj.id != id)
      continue;
    out = obj;
    return true;
  }
  return false;
}

void TrackManager::releaseKeep(int id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& obj : objects_) {
    if (obj.id != id)
      continue;
    obj.userForced = false;
    obj.matchTier = MatchTier::None;
    obj.identityId = -1;
    obj.reidScore = 0.f;
    obj.enabled = true;
    obj.label = withId("Face", obj.id);
    break;
  }
}

void TrackManager::rejectSuggestion(int id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& obj : objects_) {
    if (obj.id != id)
      continue;
    obj.suggestRejected = true;
    obj.matchTier = MatchTier::None;
    obj.identityId = -1;
    obj.enabled = true;
    obj.label = withId("Face", obj.id);
    break;
  }
}

int TrackManager::trackIdAt(int x, int y, TrackType type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  int best = -1;
  int bestArea = 0;
  for (const auto& obj : objects_) {
    if (obj.type != type || obj.lost)
      continue;
    if (!obj.rect.contains(cv::Point(x, y)))
      continue;
    const int area = obj.rect.area();
    if (best < 0 || area < bestArea) {
      best = obj.id;
      bestArea = area;
    }
  }
  return best;
}

void TrackManager::importAiSnapshot(const std::vector<TrackObject>& snap, bool importFaces) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& t : snap) {
    if (t.type == TrackType::AiFace && !importFaces)
      continue;
    if (t.type != TrackType::AiFace && t.type != TrackType::AiObject)
      continue;
    TrackObject o = t;
    o.lost = true;
    o.missCount = 0;
    objects_.push_back(o);
    if (o.id >= nextId_)
      nextId_ = o.id + 1;
  }
}

std::vector<TrackObject> TrackManager::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return objects_;
}

std::vector<TrackObject> TrackManager::visibleAt(int64_t frameIndex) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TrackObject> out;
  for (const auto& obj : objects_) {
    if (!faceVisibleForRedact(obj) || !obj.activeAt(frameIndex))
      continue;
    out.push_back(obj);
  }
  return out;
}

std::vector<cv::Rect> TrackManager::zoomFollowBoxes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<cv::Rect> out;
  for (const auto& obj : objects_) {
    if (obj.type != TrackType::AiFace)
      continue;
    if (obj.lost && obj.missCount > 45)
      continue;
    const bool small = obj.rect.width < 96 || obj.rect.height < 96;
    if (!small && obj.missCount <= 0)
      continue;
    if (obj.rect.width > 4 && obj.rect.height > 4)
      out.push_back(obj.rect);
  }
  return out;
}

}  // namespace vb
