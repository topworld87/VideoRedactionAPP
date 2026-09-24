#include "vb/ByteTrack.h"

#include <algorithm>
#include <cmath>

namespace vb {
namespace {
cv::Rect2f toRect2f(const cv::Rect& r) {
  return cv::Rect2f(static_cast<float>(r.x), static_cast<float>(r.y),
                    static_cast<float>(r.width), static_cast<float>(r.height));
}
cv::Rect toRect(const cv::Rect2f& r) {
  return cv::Rect(static_cast<int>(std::round(r.x)), static_cast<int>(std::round(r.y)),
                  std::max(1, static_cast<int>(std::round(r.width))),
                  std::max(1, static_cast<int>(std::round(r.height))));
}

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace

void ByteTracker::reset() { tracks_.clear(); }

float ByteTracker::iou(const cv::Rect2f& a, const cv::Rect2f& b) {
  const float x1 = std::max(a.x, b.x);
  const float y1 = std::max(a.y, b.y);
  const float x2 = std::min(a.x + a.width, b.x + b.width);
  const float y2 = std::min(a.y + a.height, b.y + b.height);
  const float iw = std::max(0.f, x2 - x1);
  const float ih = std::max(0.f, y2 - y1);
  const float inter = iw * ih;
  const float uni = a.area() + b.area() - inter;
  if (uni <= 1e-6f)
    return 0.f;
  return inter / uni;
}

float ByteTracker::associationScore(const Track& tr, const FaceDet& det, float minIou) {
  const cv::Rect2f drect = toRect2f(det.rect);
  const float v = iou(tr.rect, drect);
  const float tcx = tr.rect.x + tr.rect.width * 0.5f;
  const float tcy = tr.rect.y + tr.rect.height * 0.5f;
  const float dcx = drect.x + drect.width * 0.5f;
  const float dcy = drect.y + drect.height * 0.5f;
  const float dist = std::sqrt((tcx - dcx) * (tcx - dcx) + (tcy - dcy) * (tcy - dcy));
  const float tdiag =
      std::sqrt(tr.rect.width * tr.rect.width + tr.rect.height * tr.rect.height);
  const float ddiag = std::sqrt(drect.width * drect.width + drect.height * drect.height);
  const float gate = 0.70f * std::max(tdiag, ddiag);
  const float wr = drect.width / std::max(4.f, tr.rect.width);
  const float hr = drect.height / std::max(4.f, tr.rect.height);
  const bool scaleOk = wr > 0.28f && wr < 4.5f && hr > 0.28f && hr < 4.5f;

  // Approaching the camera grows the box; IoU against last frame often falls
  // below 0.25 even when it is the same person. Keep the ID via center + scale.
  if (v >= 0.45f)
    return v + 0.20f;
  if (v >= minIou && scaleOk)
    return v + 0.15f;
  if (!scaleOk)
    return 0.f;
  if (dist > gate)
    return 0.f;
  const float prox = 1.f - dist / std::max(gate, 1.f);
  return 0.12f + 0.35f * prox;
}

std::vector<int> ByteTracker::greedyMatch(const std::vector<Track>& tracks,
                                          const std::vector<FaceDet>& dets,
                                          float minIou,
                                          const std::vector<char>& trackMask,
                                          const std::vector<char>& detMask) {
  struct Pair {
    float score;
    int t;
    int d;
  };
  std::vector<Pair> pairs;
  for (int t = 0; t < static_cast<int>(tracks.size()); ++t) {
    if (trackMask[static_cast<size_t>(t)])
      continue;
    for (int d = 0; d < static_cast<int>(dets.size()); ++d) {
      if (detMask[static_cast<size_t>(d)])
        continue;
      const float s = associationScore(tracks[static_cast<size_t>(t)],
                                       dets[static_cast<size_t>(d)], minIou);
      if (s <= 1e-6f)
        continue;
      pairs.push_back({s, t, d});
    }
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const Pair& a, const Pair& b) { return a.score > b.score; });

  std::vector<int> assign(tracks.size(), -1);
  std::vector<char> usedT(tracks.size(), 0);
  std::vector<char> usedD(dets.size(), 0);
  for (const auto& p : pairs) {
    if (usedT[static_cast<size_t>(p.t)] || usedD[static_cast<size_t>(p.d)])
      continue;
    usedT[static_cast<size_t>(p.t)] = 1;
    usedD[static_cast<size_t>(p.d)] = 1;
    assign[static_cast<size_t>(p.t)] = p.d;
  }
  return assign;
}

void ByteTracker::predict() {
  for (auto& t : tracks_) {
    if (t.lost)
      continue;
    const float cx = t.rect.x + t.rect.width * 0.5f + t.vx;
    const float cy = t.rect.y + t.rect.height * 0.5f + t.vy;
    t.rect.width = std::max(4.f, t.rect.width + t.vw);
    t.rect.height = std::max(4.f, t.rect.height + t.vh);
    t.rect.x = cx - t.rect.width * 0.5f;
    t.rect.y = cy - t.rect.height * 0.5f;
  }
}

void ByteTracker::applyDetection(Track& tr, const cv::Rect2f& nr, float score) {
  const float ocx = tr.rect.x + tr.rect.width * 0.5f;
  const float ocy = tr.rect.y + tr.rect.height * 0.5f;
  const float ncx = nr.x + nr.width * 0.5f;
  const float ncy = nr.y + nr.height * 0.5f;
  tr.vx = 0.50f * tr.vx + 0.50f * (ncx - ocx);
  tr.vy = 0.50f * tr.vy + 0.50f * (ncy - ocy);
  tr.vw = 0.55f * tr.vw + 0.45f * (nr.width - tr.rect.width);
  tr.vh = 0.55f * tr.vh + 0.45f * (nr.height - tr.rect.height);
  tr.vw = clampf(tr.vw, -tr.rect.width * 0.35f, tr.rect.width * 0.35f);
  tr.vh = clampf(tr.vh, -tr.rect.height * 0.35f, tr.rect.height * 0.35f);
  tr.rect = nr;
  tr.score = score;
  tr.miss = 0;
  tr.lost = false;
}

std::vector<FaceDet> ByteTracker::update(const std::vector<FaceDet>& dets,
                                         const std::function<int()>& nextId,
                                         std::vector<int>* lostIds) {
  if (lostIds)
    lostIds->clear();
  predict();

  std::vector<FaceDet> high;
  std::vector<FaceDet> low;
  high.reserve(dets.size());
  low.reserve(dets.size());
  for (const auto& d : dets) {
    if (d.score >= highThresh_)
      high.push_back(d);
    else if (d.score >= lowThresh_)
      low.push_back(d);
  }

  std::vector<char> trackUsed(tracks_.size(), 0);
  std::vector<char> highUsed(high.size(), 0);
  const auto highAssign =
      greedyMatch(tracks_, high, matchIou_, trackUsed, highUsed);
  for (size_t t = 0; t < tracks_.size(); ++t) {
    const int d = highAssign[t];
    if (d < 0)
      continue;
    trackUsed[t] = 1;
    highUsed[static_cast<size_t>(d)] = 1;
    applyDetection(tracks_[t], toRect2f(high[static_cast<size_t>(d)].rect),
                   high[static_cast<size_t>(d)].score);
  }

  std::vector<char> lowUsed(low.size(), 0);
  const auto lowAssign = greedyMatch(tracks_, low, lowMatchIou_, trackUsed, lowUsed);
  for (size_t t = 0; t < tracks_.size(); ++t) {
    if (trackUsed[t])
      continue;
    const int d = lowAssign[t];
    if (d < 0)
      continue;
    trackUsed[t] = 1;
    applyDetection(tracks_[t], toRect2f(low[static_cast<size_t>(d)].rect),
                   low[static_cast<size_t>(d)].score);
  }

  // Brief detector blanks: damp motion harder so the mosaic does not skate
  // sideways off a walking face before the next hit.
  for (size_t t = 0; t < tracks_.size(); ++t) {
    if (trackUsed[t])
      continue;
    auto& tr = tracks_[t];
    ++tr.miss;
    tr.vx *= 0.55f;
    tr.vy *= 0.55f;
    tr.vw *= 0.35f;
    tr.vh *= 0.35f;
    if (tr.miss > maxAge_ && !tr.lost) {
      tr.lost = true;
      if (lostIds)
        lostIds->push_back(tr.id);
    }
  }

  for (size_t d = 0; d < high.size(); ++d) {
    if (highUsed[d])
      continue;
    Track tr;
    tr.id = nextId ? nextId() : static_cast<int>(tracks_.size() + 1);
    tr.rect = toRect2f(high[d].rect);
    tr.score = high[d].score;
    tracks_.push_back(tr);
  }

  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                               [](const Track& t) { return t.lost; }),
                tracks_.end());

  std::vector<FaceDet> out;
  out.reserve(tracks_.size());
  for (const auto& t : tracks_) {
    FaceDet f;
    f.rect = toRect(t.rect);
    f.score = t.score;
    out.push_back(f);
  }
  return out;
}

}  // namespace vb
