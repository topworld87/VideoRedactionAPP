#include "vb/PreviewEngine.h"

#include "vb/CsrtTracker.h"
#include "vb/HardwareInfo.h"
#include "vb/KeepFacePolicy.h"
#include "vb/Settings.h"
#include "vb/fs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace vb {
namespace {

void keepLog(const char* fmt, ...) {
  char buf[768];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  appendTextLog("keep_debug.log", buf);
}

const char* matchTierName(MatchTier tier) {
  switch (tier) {
  case MatchTier::Enrolled:
    return "enrolled";
  case MatchTier::AutoLinked:
    return "auto";
  case MatchTier::Suggest:
    return "suggest";
  default:
    return "none";
  }
}

void paintTag(cv::Mat& bgr, const cv::Rect& rect, const cv::Scalar& color, int thick,
              const char* tag) {
  if (rect.width < 2 || rect.height < 2)
    return;
  const cv::Rect bounds(0, 0, bgr.cols, bgr.rows);
  const cv::Rect box = rect & bounds;
  if (box.width < 2 || box.height < 2)
    return;
  cv::rectangle(bgr, box, color, thick, cv::LINE_AA);
  if (tag == nullptr || tag[0] == '\0')
    return;
  int baseline = 0;
  const double scale = 0.5;
  const cv::Size text = cv::getTextSize(tag, cv::FONT_HERSHEY_SIMPLEX, scale, 1, &baseline);
  int x = box.x;
  int y = box.y - 4;
  if (y < text.height + 4)
    y = std::min(bgr.rows - 2, box.y + text.height + 4);
  cv::Rect chip(x, y - text.height - 2, text.width + 8, text.height + 6);
  chip &= bounds;
  if (chip.width > 2 && chip.height > 2)
    cv::rectangle(bgr, chip, color, cv::FILLED);
  cv::putText(bgr, tag, cv::Point(x + 4, y), cv::FONT_HERSHEY_SIMPLEX, scale,
              cv::Scalar(12, 16, 12), 1, cv::LINE_AA);
}

void paintKeepMarks(cv::Mat& bgr, const std::vector<TrackObject>& boxes) {
  const cv::Scalar gray(170, 176, 184);
  const cv::Scalar keep(132, 220, 61);
  const cv::Scalar mask(56, 168, 232);
  for (const auto& t : boxes) {
    if (t.type != TrackType::AiFace)
      continue;
    const bool kept =
        t.matchTier == MatchTier::Enrolled || t.matchTier == MatchTier::AutoLinked;
    if (!kept)
      paintTag(bgr, t.rect, gray, 1, nullptr);
  }
  for (const auto& t : boxes) {
    if (t.type == TrackType::ManualTrack || t.type == TrackType::StaticRegion)
      continue;
    if (t.type == TrackType::AiObject && t.enabled)
      paintTag(bgr, t.rect, mask, 1, nullptr);
  }
  for (const auto& t : boxes) {
    if (t.type != TrackType::AiFace)
      continue;
    if (t.matchTier == MatchTier::Enrolled || t.matchTier == MatchTier::AutoLinked)
      paintTag(bgr, t.rect, keep, 2, "Keep");
  }
}

void logKeepFaces(int64_t index, size_t detCount, const std::vector<TrackObject>& active) {
  int shown = 0;
  for (const auto& t : active) {
    if (t.type == TrackType::AiFace)
      ++shown;
  }
  keepLog("FACES frame=%lld det=%d shown=%d", static_cast<long long>(index),
          static_cast<int>(detCount), shown);
  for (const auto& t : active) {
    if (t.type != TrackType::AiFace)
      continue;
    const char* reason = "redact";
    if (!t.enabled) {
      if (t.matchTier == MatchTier::Enrolled)
        reason = "enrolled";
      else if (t.matchTier == MatchTier::AutoLinked)
        reason = "reid_auto";
      else if (t.userForced)
        reason = "user_forced";
      else if (t.startFrame == index)
        reason = "born_disabled";
      else
        reason = "carried_disabled";
    }
    keepLog("FACE frame=%lld id=%d en=%d tier=%s ident=%d score=%.3f forced=%d start=%lld "
            "rect=%d,%d,%d,%d reason=%s",
            static_cast<long long>(index), t.id, t.enabled ? 1 : 0, matchTierName(t.matchTier),
            t.identityId, t.reidScore, t.userForced ? 1 : 0,
            static_cast<long long>(t.startFrame), t.rect.x, t.rect.y, t.rect.width,
            t.rect.height, reason);
  }
}

}  // namespace

PreviewEngine::PreviewEngine() = default;

PreviewEngine::~PreviewEngine() { closeVideo(); }

void PreviewEngine::setCallbacks(FrameFn onFrame, TracksFn onTracks, MarksFn onMarks,
                                 ErrorFn onError, OpenedFn onOpened, VoidFn onFinished) {
  onFrame_ = std::move(onFrame);
  onTracks_ = std::move(onTracks);
  onMarks_ = std::move(onMarks);
  onError_ = std::move(onError);
  onOpened_ = std::move(onOpened);
  onFinished_ = std::move(onFinished);
}

void PreviewEngine::emitTracksUpdated(bool force) {
  if (!force && playing_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (!tracksEmitTimerStarted_) {
      lastTracksEmit_ = now;
      tracksEmitTimerStarted_ = true;
    } else if (now - lastTracksEmit_ < std::chrono::milliseconds(200)) {
      return;
    }
    lastTracksEmit_ = now;
  }
  if (onTracks_)
    onTracks_(tracks_.snapshot());
}

bool PreviewEngine::openVideo(const std::string& pathUtf8, const std::string& faceModelPath,
                              const std::string& yoloModelPath,
                              const std::string& landmarksModelPath,
                              const std::string& reidModelPath) {
  closeVideo();
  appendTextLog("keep_debug.log", "OPEN", true);
  if (!decoder_.open(pathUtf8)) {
    if (onError_)
      onError_("Failed to open video.");
    return false;
  }
  const bool tryGpu = Settings::instance().hardwareAccel();
  if (!detector_.load(faceModelPath, tryGpu)) {
    if (onError_)
      onError_("Failed to load face model (YOLO11-face / YuNet ONNX).");
  } else {
    // YOLO-face scores sit lower than YuNet on Hard faces; keep recall high.
    float thr = Settings::instance().scoreThreshold();
    if (detector_.isYoloFace())
      thr = std::min(thr, 0.35f);
    detector_.setScoreThreshold(thr);
    detector_.setMaxTiles(detector_.ep() == OrtEpKind::DirectML ? 10 : 6);
    detector_.setTileOverlap(0.35f);
  }
  if (!landmarksModelPath.empty())
    landmarks_.load(landmarksModelPath, tryGpu);
  if (!reidModelPath.empty())
    reid_.load(reidModelPath, tryGpu);
  if (!yoloModelPath.empty()) {
    if (!yolo_.load(yoloModelPath, tryGpu)) {
      if (onError_)
        onError_("Failed to load YOLO model (optional).");
    } else {
      yolo_.useDefaultRedactionClasses();
    }
  }
  tracks_.reset();
  tracks_.setEofFrame(decoder_.frameCount());
  tracks_.setProximityKeepInherit(facePolicy_.load() != FacePolicy::ReverseKeep);
  faceMarks_.clear();
  hasVideo_ = true;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    lastBgr_.release();
  }
  lastIndex_ = 0;
  lastProcessed_ = -1;
  loggingKeep_ = false;
  tracksEmitTimerStarted_ = false;
  delayQueue_.clear();

  if (!processOneFrame(false)) {
    if (onError_)
      onError_("Video has no frames.");
    return false;
  }
  if (onOpened_)
    onOpened_(decoder_.fps(), decoder_.frameCount(), decoder_.width(),
              decoder_.height());
  emitTracksUpdated(true);
  return true;
}

void PreviewEngine::closeVideo() {
  stop_ = true;
  playing_ = false;
  if (worker_.joinable())
    worker_.join();
  stop_ = false;
  decoder_.close();
  hasVideo_ = false;
  tracks_.reset();
  faceMarks_.clear();
  delayQueue_.clear();
  lastProcessed_ = -1;
  loggingKeep_ = false;
}

void PreviewEngine::play() {
  if (!hasVideo_)
    return;
  playing_ = true;
  tracksEmitTimerStarted_ = false;
  if (!worker_.joinable()) {
    stop_ = false;
    worker_ = std::thread([this] { threadMain(); });
  }
}

void PreviewEngine::pause() { playing_ = false; }

void PreviewEngine::seek(int64_t frameIndex) {
  seekTarget_ = frameIndex;
  seekRequested_ = true;
  if (!worker_.joinable() && hasVideo_) {
    if (decoder_.seekFrame(frameIndex)) {
      processOneFrame(false);
      emitTracksUpdated(true);
    }
    seekRequested_ = false;
  }
}

void PreviewEngine::setRedactionMode(RedactionMode mode) { filter_.setMode(mode); }
void PreviewEngine::setScoreThreshold(float v) { detector_.setScoreThreshold(v); }
void PreviewEngine::setFacesEnabled(bool enabled) { facesEnabled_ = enabled; }
void PreviewEngine::setYoloEnabled(bool enabled) { yoloEnabled_ = enabled; }
void PreviewEngine::setFacePolicy(FacePolicy policy) {
  facePolicy_ = policy;
  tracks_.setProximityKeepInherit(policy != FacePolicy::ReverseKeep);
  refreshPreview();
}

std::string PreviewEngine::runtimeStatus() const {
  if (!detector_.isReady())
    return "Face model not loaded";
  const char* model = detector_.modelLabel();
  const std::string hw = HardwareInfo::instance().summary();
  if (detector_.ep() == OrtEpKind::DirectML)
    return std::string("GPU (DirectML) · ") + model + " · " + hw;
  return std::string("CPU · ") + model + " · " + hw +
         " — processing will be much slower.";
}

std::string PreviewEngine::dumpDetectDebug() {
  cv::Mat bgr;
  int64_t index = 0;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    if (lastBgr_.empty())
      return {};
    bgr = lastBgr_.clone();
    index = lastIndex_;
  }

  namespace fs = std::filesystem;
  const fs::path root = u8path(writableDirUtf8()) / "debug_dump";
  char folder[64];
  std::snprintf(folder, sizeof(folder), "frame_%06lld",
                static_cast<long long>(index));
  const fs::path dir = root / folder;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return {};

  const std::string framePath = pathToUtf8(dir / "frame.png");
  const std::string overlayPath = pathToUtf8(dir / "overlay.png");
  const std::string reportPath = pathToUtf8(dir / "report.txt");
  if (!cv::imwrite(framePath, bgr))
    return {};

  std::vector<YoloDetection> yoloDets;
  if (yolo_.isReady())
    yoloDets = yolo_.detect(bgr);

  const auto zoom =
      YoloDetectorDnn::faceZoomRois(yoloDets, tracks_.zoomFollowBoxes(), bgr.cols,
                                    bgr.rows);
  std::vector<FaceDet> faces;
  std::vector<FaceDet> facesLow;
  if (detector_.isReady()) {
    faces = detector_.detect(bgr, zoom);
    const float saved = detector_.scoreThreshold();
    detector_.setScoreThreshold(0.12f);
    facesLow = detector_.detect(bgr, zoom);
    detector_.setScoreThreshold(saved);
  }

  cv::Mat overlay = bgr.clone();
  int personCount = 0;
  for (const auto& d : yoloDets) {
    if (!YoloDetectorDnn::isPersonClass(d.classId))
      continue;
    ++personCount;
    cv::rectangle(overlay, d.rect, cv::Scalar(40, 200, 40), 2);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "person %.2f", d.score);
    cv::putText(overlay, buf, cv::Point(d.rect.x, std::max(14, d.rect.y - 4)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(40, 200, 40), 1,
                cv::LINE_AA);
  }
  for (const auto& r : zoom) {
    cv::rectangle(overlay, r, cv::Scalar(0, 220, 255), 1);
  }
  for (const auto& f : facesLow) {
    cv::rectangle(overlay, f.rect, cv::Scalar(180, 80, 255), 1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", f.score);
    cv::putText(overlay, buf, cv::Point(f.rect.x, f.rect.y + f.rect.height + 14),
                cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(180, 80, 255), 1,
                cv::LINE_AA);
  }
  for (const auto& f : faces) {
    cv::rectangle(overlay, f.rect, cv::Scalar(0, 0, 255), 2);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", f.score);
    cv::putText(overlay, buf, cv::Point(f.rect.x, std::max(14, f.rect.y - 4)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 1,
                cv::LINE_AA);
  }
  for (const auto& t : tracks_.visibleAt(index)) {
    if (t.type != TrackType::AiFace)
      continue;
    cv::rectangle(overlay, t.rect, cv::Scalar(255, 200, 0), 2);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "T%d%s", t.id, t.enabled ? "" : " off");
    cv::putText(overlay, buf, cv::Point(t.rect.x, t.rect.y + t.rect.height + 14),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(255, 200, 0), 1,
                cv::LINE_AA);
  }

  // Legend strip
  cv::rectangle(overlay, cv::Rect(8, 8, 420, 78), cv::Scalar(0, 0, 0), -1);
  cv::putText(overlay, "green=person  yellow=zoomROI  magenta=lowScore  red=accepted  cyan=track",
              cv::Point(14, 30), cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(230, 230, 230),
              1, cv::LINE_AA);
  char header[128];
  std::snprintf(header, sizeof(header), "frame=%lld  model=%s  thr=%.2f  %dx%d",
                static_cast<long long>(index), detector_.modelLabel(),
                Settings::instance().scoreThreshold(), bgr.cols, bgr.rows);
  cv::putText(overlay, header, cv::Point(14, 54), cv::FONT_HERSHEY_SIMPLEX, 0.42,
              cv::Scalar(230, 230, 230), 1, cv::LINE_AA);
  std::snprintf(header, sizeof(header), "persons=%d zooms=%d faces=%d lowFaces=%d",
                personCount, static_cast<int>(zoom.size()),
                static_cast<int>(faces.size()), static_cast<int>(facesLow.size()));
  cv::putText(overlay, header, cv::Point(14, 74), cv::FONT_HERSHEY_SIMPLEX, 0.42,
              cv::Scalar(230, 230, 230), 1, cv::LINE_AA);

  cv::imwrite(overlayPath, overlay);

  std::ofstream report(u8path(reportPath), std::ios::binary);
  if (report) {
    report << "frameIndex=" << index << "\n";
    report << "size=" << bgr.cols << "x" << bgr.rows << "\n";
    report << "model=" << detector_.modelLabel() << "\n";
    report << "ep=" << (detector_.ep() == OrtEpKind::DirectML ? "DirectML" : "CPU")
           << "\n";
    report << "scoreThreshold=" << Settings::instance().scoreThreshold() << "\n";
    report << "runtime=" << runtimeStatus() << "\n";
    report << "personCount=" << personCount << "\n";
    report << "zoomRoiCount=" << zoom.size() << "\n";
    report << "acceptedFaces=" << faces.size() << "\n";
    report << "lowScoreFaces=" << facesLow.size() << "\n\n";

    report << "[persons]\n";
    for (const auto& d : yoloDets) {
      if (!YoloDetectorDnn::isPersonClass(d.classId))
        continue;
      report << "score=" << d.score << " rect=" << d.rect.x << "," << d.rect.y << ","
             << d.rect.width << "," << d.rect.height
             << " hFrac=" << (static_cast<float>(d.rect.height) / bgr.rows) << "\n";
    }
    report << "\n[zoomRois]\n";
    for (const auto& r : zoom)
      report << r.x << "," << r.y << "," << r.width << "," << r.height << "\n";
    report << "\n[acceptedFaces]\n";
    for (const auto& f : faces)
      report << "score=" << f.score << " rect=" << f.rect.x << "," << f.rect.y << ","
             << f.rect.width << "," << f.rect.height << "\n";
    report << "\n[lowScoreFaces thr=0.12]\n";
    for (const auto& f : facesLow)
      report << "score=" << f.score << " rect=" << f.rect.x << "," << f.rect.y << ","
             << f.rect.width << "," << f.rect.height << "\n";
    report << "\n[tracks]\n";
    for (const auto& t : tracks_.visibleAt(index)) {
      if (t.type != TrackType::AiFace)
        continue;
      report << "id=" << t.id << " enabled=" << t.enabled << " lost=" << t.lost
             << " miss=" << t.missCount << " rect=" << t.rect.x << "," << t.rect.y
             << "," << t.rect.width << "," << t.rect.height << "\n";
    }
  }

  return pathToUtf8(dir);
}

void PreviewEngine::addManualTrack(const cv::Rect& rect) {
  cv::Mat bgr;
  int64_t index = 0;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    if (lastBgr_.empty()) {
      pendingManual_.push_back({rect});
      return;
    }
    bgr = lastBgr_.clone();
    index = lastIndex_;
  }
  tracks_.addManualTrack(bgr, rect, index);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::addStaticRegion(const cv::Rect& rect, int64_t startFrame,
                                    int64_t endFrame) {
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    if (lastBgr_.empty()) {
      pendingStatic_.push_back({rect, startFrame, endFrame});
      return;
    }
    if (startFrame < 0 || startFrame > lastIndex_)
      startFrame = lastIndex_;
  }
  tracks_.addStaticRegion(rect, startFrame, endFrame);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::setTrackEnabled(int id, bool enabled) {
  tracks_.setEnabled(id, enabled, true);
  if (facePolicy_.load() == FacePolicy::ReverseKeep && !enabled)
    enrollKeepTrack(id);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::enrollKeepAt(int x, int y) {
  const int id = tracks_.trackIdAt(x, y, TrackType::AiFace);
  if (id <= 0) {
    keepLog("CLICK_MISS x=%d y=%d", x, y);
    return;
  }
  enrollKeepTrack(id);
}

void PreviewEngine::enrollKeepTrack(int trackId) {
  TrackObject obj;
  if (!tracks_.get(trackId, obj))
    return;
  cv::Mat bgr;
  int64_t frame = 0;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    if (lastBgr_.empty())
      return;
    bgr = lastBgr_.clone();
    frame = lastIndex_;
  }
  bool lmkOk = false;
  auto emb = extractEmbedding(bgr, obj.rect, &lmkOk);
  int identityId = obj.identityId;
  if (identityId < 0) {
    identityId = gallery_.enroll(emb);
  } else {
    gallery_.addSample(identityId, emb);
  }
  keepLog("ENROLL frame=%lld track=%d ident=%d emb=%d lmk=%d reid=%d rect=%d,%d,%d,%d",
          static_cast<long long>(frame), trackId, identityId, static_cast<int>(emb.size()),
          lmkOk ? 1 : 0, reid_.isReady() ? 1 : 0, obj.rect.x, obj.rect.y, obj.rect.width,
          obj.rect.height);
  if (!emb.empty())
    tracks_.setEmbedding(trackId, emb);
  tracks_.setIdentity(trackId, identityId, MatchTier::Enrolled,
                      emb.empty() ? 1.f : 1.f);
  tracks_.setEnabled(trackId, false, true);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::confirmIdentity(int trackId) {
  TrackObject obj;
  if (!tracks_.get(trackId, obj) || obj.identityId < 0)
    return;
  if (!obj.embedding.empty())
    gallery_.addSample(obj.identityId, obj.embedding);
  tracks_.setIdentity(trackId, obj.identityId, MatchTier::Enrolled, obj.reidScore);
  tracks_.setEnabled(trackId, false, true);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::rejectIdentity(int trackId) {
  tracks_.rejectSuggestion(trackId);
  emitTracksUpdated(true);
  refreshPreview();
}

void PreviewEngine::clearGallery() {
  gallery_.clear();
}

std::vector<float> PreviewEngine::extractEmbedding(const cv::Mat& bgr,
                                                   const cv::Rect& face,
                                                   bool* landmarksOk) const {
  if (landmarksOk)
    *landmarksOk = false;
  if (!reid_.isReady())
    return {};
  std::array<cv::Point2f, 5> pts{};
  const std::array<cv::Point2f, 5>* lmk = nullptr;
  if (landmarks_.isReady() &&
      const_cast<FaceLandmarks&>(landmarks_).infer(bgr, face, pts)) {
    lmk = &pts;
    if (landmarksOk)
      *landmarksOk = true;
  }
  return const_cast<FaceReId&>(reid_).embed(bgr, face, lmk);
}

void PreviewEngine::applyReversePolicy(const cv::Mat& bgr, int64_t frameIndex) {
  if (facePolicy_.load() != FacePolicy::ReverseKeep)
    return;
  if (gallery_.empty() || !reid_.isReady()) {
    if (loggingKeep_) {
      keepLog("REID_OFF frame=%lld gallery=%d reid=%d", static_cast<long long>(frameIndex),
              static_cast<int>(gallery_.size()), reid_.isReady() ? 1 : 0);
    }
    return;
  }
  applyKeepFaceFrame(tracks_, gallery_, reid_, landmarks_, bgr, frameIndex,
                     [](const std::string& line) { appendTextLog("keep_debug.log", line); });
}

void PreviewEngine::refreshPreview() {
  cv::Mat bgr;
  int64_t index = 0;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    if (lastBgr_.empty())
      return;
    bgr = lastBgr_.clone();
    index = lastIndex_;
  }
  std::vector<cv::Rect> redactRects;
  std::vector<TrackObject> boxes;
  for (const auto& t : tracks_.snapshot()) {
    if (t.lost || !t.activeAt(index))
      continue;
    boxes.push_back(t);
    if (t.enabled)
      redactRects.push_back(t.rect);
  }
  if (facePolicy_.load() == FacePolicy::ReverseKeep)
    paintKeepMarks(bgr, boxes);
  else
    filter_.apply(bgr, redactRects);
  FramePacket packet;
  packet.bgr = bgr;
  cv::cvtColor(bgr, packet.rgb, cv::COLOR_BGR2RGB);
  if (!packet.rgb.isContinuous())
    packet.rgb = packet.rgb.clone();
  packet.frameIndex = index;
  packet.ptsSec = decoder_.fps() > 1e-3 ? index / decoder_.fps() : 0.0;
  packet.boxes = boxes;
  if (onFrame_)
    onFrame_(packet);
}

void PreviewEngine::threadMain() {
  using clock = std::chrono::steady_clock;
  while (!stop_) {
    if (seekRequested_.exchange(false)) {
      flushDelayQueue(false);
      decoder_.seekFrame(seekTarget_.load());
      processOneFrame(false);
      emitTracksUpdated(true);
    }

    if (!playing_) {
      flushDelayQueue(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    const auto t0 = clock::now();
    if (!processOneFrame(true)) {
      playing_ = false;
      if (onFinished_)
        onFinished_();
      continue;
    }
    emitTracksUpdated(false);

    const double fps = decoder_.fps() > 1e-3 ? decoder_.fps() : 25.0;
    const auto budget = std::chrono::milliseconds(static_cast<int>(1000.0 / fps));
    const auto spent = clock::now() - t0;
    if (spent < budget)
      std::this_thread::sleep_for(budget - spent);
  }
}

bool PreviewEngine::processOneFrame(bool allowDelay) {
  cv::Mat bgr;
  if (!decoder_.read(bgr)) {
    flushDelayQueue(true);
    return false;
  }

  const int64_t index = decoder_.currentFrame() - 1;
  const bool faceJump = lastProcessed_ >= 0 &&
                        (index > lastProcessed_ + 1 || index < lastProcessed_) &&
                        facePolicy_.load() == FacePolicy::ReverseKeep;
  loggingKeep_ = faceJump;
  if (faceJump) {
    keepLog("JUMP from=%lld to=%lld gap=%lld gallery=%d reid=%d auto>=%.2f suggest>=%.2f",
            static_cast<long long>(lastProcessed_), static_cast<long long>(index),
            static_cast<long long>(index - lastProcessed_), static_cast<int>(gallery_.size()),
            reid_.isReady() ? 1 : 0, IdentityGallery::kAutoThresh, IdentityGallery::kSuggestThresh);
    tracks_.breakFaceContinuity();
    tracks_.setKeepDebug(true);
  }
  lastProcessed_ = index;
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    for (const auto& m : pendingManual_)
      tracks_.addManualTrack(bgr, m.rect, index);
    pendingManual_.clear();
    for (const auto& s : pendingStatic_)
      tracks_.addStaticRegion(s.rect, s.start, s.end);
    pendingStatic_.clear();
  }

  std::vector<YoloDetection> yoloDets;
  const bool runYolo =
      yolo_.isReady() && (yoloEnabled_.load() || facesEnabled_.load());
  if (runYolo)
    yoloDets = yolo_.detect(bgr);

  std::vector<FaceDet> faces;
  if (facesEnabled_.load() && detector_.isReady()) {
    const auto zoom = YoloDetectorDnn::faceZoomRois(
        yoloDets, tracks_.zoomFollowBoxes(), bgr.cols, bgr.rows);
    faces = detector_.detect(bgr, zoom);
    YoloDetectorDnn::appendHeadFallbacks(faces, yoloDets, bgr.cols, bgr.rows);
  }

  if (!faces.empty()) {
    if (faceMarks_.empty() || faceMarks_.back() != index)
      faceMarks_.push_back(index);
    if (faceMarks_.size() % 15 == 0 && onMarks_)
      onMarks_(faceMarks_);
  }

  std::vector<std::pair<cv::Rect, std::string>> objects;
  if (yoloEnabled_.load() && yolo_.isReady()) {
    for (const auto& d : yoloDets) {
      if (YoloDetectorDnn::isScreenClass(d.classId))
        objects.emplace_back(d.rect, d.label);
    }
    for (const auto& z : YoloDetectorDnn::toPlateZones(yoloDets))
      objects.emplace_back(z.rect, "plate-zone");
  }

  const auto personHeads =
      YoloDetectorDnn::allPersonHeads(yoloDets, bgr.cols, bgr.rows);
  std::vector<cv::Rect> appeared;
  auto active =
      tracks_.processFrame(bgr, index, faces, objects, &appeared, personHeads);
  if (facePolicy_.load() == FacePolicy::ReverseKeep)
    applyReversePolicy(bgr, index);
  active = tracks_.visibleAt(index);
  std::vector<cv::Rect> redactRects;
  redactRects.reserve(active.size() + appeared.size());
  for (const auto& t : active) {
    if (t.enabled)
      redactRects.push_back(t.rect);
  }
  if (loggingKeep_) {
    logKeepFaces(index, faces.size(), active);
    tracks_.setKeepDebug(false);
    loggingKeep_ = false;
  }

  if (allowDelay && playing_.load()) {
    if (!appeared.empty() && !delayQueue_.empty()) {
      std::vector<cv::Mat*> older;
      std::vector<std::vector<cv::Rect>*> redactSlots;
      older.reserve(delayQueue_.size());
      redactSlots.reserve(delayQueue_.size());
      for (auto& queued : delayQueue_) {
        older.push_back(&queued.cleanBgr);
        redactSlots.push_back(&queued.redact);
      }
      CsrtTracker::backfillAppearances(bgr, appeared, older, redactSlots,
                                       TrackManager::kAppearLookaheadFrames);
    }
    DelayedFrame df;
    df.cleanBgr = bgr.clone();
    df.index = index;
    df.redact = std::move(redactRects);
    df.boxes = active;
    delayQueue_.push_back(std::move(df));
    if (static_cast<int>(delayQueue_.size()) > TrackManager::kAppearLookaheadFrames) {
      DelayedFrame ready = std::move(delayQueue_.front());
      delayQueue_.pop_front();
      emitDelayedFrame(std::move(ready.cleanBgr), ready.index, ready.redact,
                       ready.boxes);
    }
    return true;
  }

  flushDelayQueue(false);
  emitDelayedFrame(bgr.clone(), index, redactRects, active);
  return true;
}

void PreviewEngine::emitDelayedFrame(cv::Mat cleanBgr, int64_t index,
                                     const std::vector<cv::Rect>& redact,
                                     const std::vector<TrackObject>& boxes) {
  {
    std::lock_guard<std::mutex> lock(cmdMutex_);
    lastBgr_ = cleanBgr.clone();
    lastIndex_ = index;
  }
  if (facePolicy_.load() == FacePolicy::ReverseKeep)
    paintKeepMarks(cleanBgr, boxes);
  else
    filter_.apply(cleanBgr, redact);

  FramePacket packet;
  packet.bgr = cleanBgr;
  cv::cvtColor(cleanBgr, packet.rgb, cv::COLOR_BGR2RGB);
  if (!packet.rgb.isContinuous())
    packet.rgb = packet.rgb.clone();
  packet.frameIndex = index;
  packet.ptsSec = decoder_.fps() > 1e-3 ? index / decoder_.fps() : 0.0;
  packet.boxes = boxes;
  if (onFrame_)
    onFrame_(packet);
}

void PreviewEngine::flushDelayQueue(bool emitFrames) {
  while (!delayQueue_.empty()) {
    DelayedFrame df = std::move(delayQueue_.front());
    delayQueue_.pop_front();
    if (emitFrames)
      emitDelayedFrame(std::move(df.cleanBgr), df.index, df.redact, df.boxes);
  }
}

}  // namespace vb
