#include "vb/CsrtTracker.h"

#include <algorithm>
#include <cmath>

namespace vb {

bool CsrtTracker::init(const cv::Mat& frame, const cv::Rect& bbox) {
  reset();
  if (frame.empty() || bbox.width < 2 || bbox.height < 2)
    return false;
  tracker_ = cv::TrackerMIL::create();
  try {
    tracker_->init(frame, bbox);
    return true;
  } catch (const cv::Exception&) {
    tracker_.release();
    return false;
  }
}

bool CsrtTracker::update(const cv::Mat& frame, cv::Rect& bbox) {
  if (!tracker_ || frame.empty())
    return false;
  cv::Rect bb = bbox;
  try {
    if (!tracker_->update(frame, bb))
      return false;
    bbox = bb;
    return true;
  } catch (const cv::Exception&) {
    return false;
  }
}

void CsrtTracker::reset() { tracker_.release(); }

cv::Rect CsrtTracker::expand(const cv::Rect& r, float frac, int frameW, int frameH) {
  const int dx = std::max(4, static_cast<int>(std::lround(r.width * frac)));
  const int dy = std::max(4, static_cast<int>(std::lround(r.height * frac)));
  cv::Rect g(r.x - dx, r.y - dy, r.width + 2 * dx, r.height + 2 * dy);
  return g & cv::Rect(0, 0, frameW, frameH);
}

std::vector<cv::Rect>
CsrtTracker::trackOlderFrames(const cv::Mat& seedFrame, const cv::Rect& seed,
                              const std::vector<const cv::Mat*>& olderOldestFirst) {
  std::vector<cv::Rect> out(olderOldestFirst.size());
  if (seedFrame.empty() || seed.width < 4 || seed.height < 4)
    return out;

  cv::Rect initBox = seed & cv::Rect(0, 0, seedFrame.cols, seedFrame.rows);
  if (initBox.width < 4 || initBox.height < 4)
    return out;

  CsrtTracker trk;
  if (!trk.init(seedFrame, initBox))
    return out;

  cv::Rect prev = initBox;
  const int maxJump = std::max(96, std::max(initBox.width, initBox.height));
  const int minArea = std::max(64, initBox.area() / 8);

  for (int i = static_cast<int>(olderOldestFirst.size()) - 1; i >= 0; --i) {
    const cv::Mat* fr = olderOldestFirst[static_cast<size_t>(i)];
    if (!fr || fr->empty())
      break;
    cv::Rect bb = prev;
    if (!trk.update(*fr, bb))
      break;
    bb &= cv::Rect(0, 0, fr->cols, fr->rows);
    if (bb.width < 8 || bb.height < 8 || bb.area() < minArea)
      break;
    const int cdx = (bb.x + bb.width / 2) - (prev.x + prev.width / 2);
    const int cdy = (bb.y + bb.height / 2) - (prev.y + prev.height / 2);
    if (cdx * cdx + cdy * cdy > maxJump * maxJump)
      break;
    out[static_cast<size_t>(i)] = bb;
    prev = bb;
  }
  return out;
}

void CsrtTracker::backfillAppearances(const cv::Mat& now,
                                      const std::vector<cv::Rect>& appeared,
                                      const std::vector<cv::Mat*>& older,
                                      const std::vector<std::vector<cv::Rect>*>& redact,
                                      int stampLastN) {
  if (now.empty() || appeared.empty() || older.size() != redact.size())
    return;

  std::vector<const cv::Mat*> views;
  views.reserve(older.size());
  for (const cv::Mat* m : older)
    views.push_back(m);

  const cv::Rect bounds(0, 0, now.cols, now.rows);
  for (const cv::Rect& seed : appeared) {
    const std::vector<cv::Rect> bbs = trackOlderFrames(now, seed, views);
    for (size_t i = 0; i < bbs.size(); ++i) {
      if (bbs[i].width > 0 && redact[i])
        redact[i]->push_back(expand(bbs[i], 0.18f, now.cols, now.rows));
    }
    const int n = std::min(stampLastN, static_cast<int>(redact.size()));
    const cv::Rect stamped = seed & bounds;
    for (int i = static_cast<int>(redact.size()) - n;
         i < static_cast<int>(redact.size()); ++i) {
      if (i >= 0 && redact[i] && stamped.width > 0)
        redact[i]->push_back(stamped);
    }
  }
}

}  // namespace vb
