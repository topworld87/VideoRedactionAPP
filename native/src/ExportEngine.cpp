#include "vb/ExportEngine.h"

#include "vb/CsrtTracker.h"
#include "vb/FaceDetectorDnn.h"
#include "vb/FaceLandmarks.h"
#include "vb/FaceReId.h"
#include "vb/FfmpegAudioTools.h"
#include "vb/HardwareInfo.h"
#include "vb/HardwareLoad.h"
#include "vb/IdentityGallery.h"
#include "vb/KeepFacePolicy.h"
#include "vb/OrtRuntime.h"
#include "vb/RedactionFilter.h"
#include "vb/Settings.h"
#include "vb/TrackManager.h"
#include "vb/VideoDecoder.h"
#include "vb/VideoEncoder.h"
#include "vb/YoloDetectorDnn.h"
#include "vb/fs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <array>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <unordered_map>

namespace vb {
namespace {

void drawExportWatermark(cv::Mat& bgr) {
  const std::string text = "Created with videoredaction.io";
  const int thickness = std::max(2, bgr.cols / 400);
  const double scale = std::max(1.2, bgr.cols / 420.0);
  int baseline = 0;
  const cv::Size sz =
      cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
  const cv::Point org((bgr.cols - sz.width) / 2, (bgr.rows + sz.height) / 2);
  const int pad = std::max(12, bgr.cols / 80);
  cv::Rect band(org.x - pad, org.y - sz.height - pad, sz.width + pad * 2,
                sz.height + baseline + pad * 2);
  band &= cv::Rect(0, 0, bgr.cols, bgr.rows);
  if (band.area() > 0) {
    cv::Mat roi = bgr(band);
    cv::Mat overlay = roi.clone();
    overlay.setTo(cv::Scalar(20, 20, 20));
    cv::addWeighted(overlay, 0.45, roi, 0.55, 0, roi);
  }
  cv::putText(bgr, text, org + cv::Point(2, 2), cv::FONT_HERSHEY_SIMPLEX, scale,
              cv::Scalar(0, 0, 0), thickness + 2, cv::LINE_AA);
  cv::putText(bgr, text, org, cv::FONT_HERSHEY_SIMPLEX, scale,
              cv::Scalar(255, 255, 255), thickness, cv::LINE_AA);
}

int exportHoldFrames(double fps) {
  const int lo = TrackManager::kAppearLookaheadFrames;
  const int byTime = static_cast<int>(std::lround((fps > 1.0 ? fps : 25.0) * 0.4));
  return std::max(lo, std::min(byTime, 12));
}

void exportLog(const std::string& msg) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  std::ofstream out(u8path(pathToUtf8(u8path(writableDirUtf8()) / "export.log")),
                    std::ios::app);
  if (!out)
    return;
  out << msg << '\n';
}

template <typename T>
class BoundedQueue {
public:
  explicit BoundedQueue(size_t capacity) : capacity_(std::max<size_t>(1, capacity)) {}

  bool push(T&& item) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    cv_.wait(lock, [&] { return closed_ || q_.size() < capacity_; });
    waitPushMs_ += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    if (closed_)
      return false;
    q_.push_back(std::move(item));
    cv_.notify_all();
    return true;
  }

  bool pop(T& out) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    cv_.wait(lock, [&] { return closed_ || !q_.empty(); });
    waitPopMs_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    if (q_.empty())
      return false;
    out = std::move(q_.front());
    q_.pop_front();
    cv_.notify_all();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
    cv_.notify_all();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
  }
  size_t capacity() const { return capacity_; }

  void takeWaitStats(double& pushMs, double& popMs) {
    std::lock_guard<std::mutex> lock(mu_);
    pushMs = waitPushMs_;
    popMs = waitPopMs_;
    waitPushMs_ = 0;
    waitPopMs_ = 0;
  }

private:
  size_t capacity_;
  std::deque<T> q_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool closed_ = false;
  double waitPushMs_ = 0;
  double waitPopMs_ = 0;
};

}  // namespace

void ExportEngine::configure(const std::string& inputPath, const std::string& outputPath,
                             const std::string& faceModelPath,
                             const std::string& yoloModelPath,
                             const std::string& landmarksModelPath,
                             const std::string& reidModelPath, RedactionMode mode,
                             bool facesEnabled, bool yoloEnabled, FacePolicy facePolicy,
                             const IdentityGallery& gallery,
                             const std::vector<TrackObject>& tracksSnapshot,
                             const std::vector<AudioRedactionItem>& audioRanges,
                             bool watermark) {
  inputPath_ = inputPath;
  outputPath_ = outputPath;
  faceModelPath_ = faceModelPath;
  yoloModelPath_ = yoloModelPath;
  landmarksModelPath_ = landmarksModelPath;
  reidModelPath_ = reidModelPath;
  mode_ = mode;
  facesEnabled_ = facesEnabled;
  yoloEnabled_ = yoloEnabled;
  facePolicy_ = facePolicy;
  gallery_ = gallery;
  watermark_ = watermark;
  snap_ = tracksSnapshot;
  audioRanges_ = audioRanges;
  cancel_ = false;
}

void ExportEngine::join() {
  if (worker_.joinable())
    worker_.join();
}

void ExportEngine::start(ProgressFn onProgress, DoneFn onDone) {
  join();
  onProgress_ = std::move(onProgress);
  onDone_ = std::move(onDone);
  running_ = true;
  worker_ = std::thread([this] { run(); });
}

void ExportEngine::run() {
  auto progress = [&](int pct, const std::string& text) {
    if (onProgress_)
      onProgress_(pct, text);
  };
  auto fail = [&](const std::string& err) {
    running_ = false;
    if (onDone_)
      onDone_(false, err);
  };
  auto ok = [&](const std::string& path) {
    running_ = false;
    if (onDone_)
      onDone_(true, path);
  };

  try {
    exportLog("==== export start ====");
    exportLog(std::string("hardware=") + HardwareInfo::instance().summary());
    progress(1, "Opening video...");

    VideoDecoder decoder;
    VideoEncoder encoder;
    FaceDetectorDnn detector;
    FaceLandmarks landmarks;
    FaceReId reid;
    IdentityGallery gallery = gallery_;
    YoloDetectorDnn yolo;
    RedactionFilter filter;
    filter.setMode(mode_);

    if (!decoder.open(inputPath_)) {
      fail("Failed to open source video for export.");
      return;
    }
    if (cancel_) {
      fail("Export cancelled.");
      return;
    }

    const bool tryGpu = Settings::instance().hardwareAccel();
    if (facesEnabled_) {
      progress(2, "Loading face model...");
      if (detector.load(faceModelPath_, tryGpu)) {
        float thr = Settings::instance().scoreThreshold();
        if (detector.isYoloFace())
          thr = std::min(thr, 0.35f);
        detector.setScoreThreshold(thr);
        detector.setTileOverlap(0.35f);
        detector.setMaxTiles(detector.ep() == OrtEpKind::DirectML ? 10 : 6);
      }
      if (facePolicy_ == FacePolicy::ReverseKeep) {
        if (!landmarksModelPath_.empty())
          landmarks.load(landmarksModelPath_, tryGpu);
        if (!reidModelPath_.empty())
          reid.load(reidModelPath_, tryGpu);
      }
    }
    if (cancel_) {
      fail("Export cancelled.");
      return;
    }
    if (yoloEnabled_ || (facesEnabled_ && !yoloModelPath_.empty())) {
      progress(3, "Loading object model...");
      if (yolo.load(yoloModelPath_, tryGpu))
        yolo.useDefaultRedactionClasses();
    }
    if (cancel_) {
      fail("Export cancelled.");
      return;
    }

    progress(4, "Creating output file...");
    const std::string videoOnly = outputPath_ + ".video_only.mp4";
    if (!encoder.open(videoOnly, decoder.fps(), decoder.width(), decoder.height(),
                      decoder.bitrateKbps())) {
      fail(encoder.lastError().empty()
               ? "Failed to create output video. Try another path."
               : ("Failed to create output video: " + encoder.lastError()));
      return;
    }

    struct ManualSeed {
      int id;
      cv::Rect rect;
      int64_t start;
      bool enabled;
    };
    std::vector<ManualSeed> manuals;
    std::vector<TrackObject> statics;
    for (const auto& o : snap_) {
      if (o.type == TrackType::ManualTrack)
        manuals.push_back({o.id, o.rect, o.startFrame, o.enabled});
      else if (o.type == TrackType::StaticRegion)
        statics.push_back(o);
    }

    TrackManager live;
    live.setEofFrame(decoder.frameCount());
    if (facePolicy_ == FacePolicy::ReverseKeep)
      live.setProximityKeepInherit(false);
    // Keep-face export re-decides every frame from the clicked identities.
    // Preview face tracks are not a second keep-list.
    live.importAiSnapshot(snap_, facePolicy_ != FacePolicy::ReverseKeep);

    std::unordered_map<int, std::shared_ptr<CsrtTracker>> manualTrackers;
    std::unordered_map<int, cv::Rect> manualRects;
    for (const auto& m : manuals)
      manualRects[m.id] = m.rect;

    const int64_t total = std::max<int64_t>(1, decoder.frameCount());
    std::atomic<int64_t> written{0};
    const int holdFrames = exportHoldFrames(decoder.fps());
    // Use CPU cores for letterbox / mosaic while DirectML owns the GPU.
    cv::setNumThreads(static_cast<int>(std::max(2u, std::thread::hardware_concurrency())));
    exportLog(std::string("encoder=") + encoder.backendLabel());
    exportLog(std::string("face_ep=") +
              (!detector.isReady()
                   ? "n/a"
                   : (detector.ep() == OrtEpKind::DirectML ? "DirectML" : "CPU")));
    exportLog(std::string("yolo_ep=") +
              (!yolo.isReady() ? "n/a"
                               : (yolo.ep() == OrtEpKind::DirectML ? "DirectML" : "CPU")));
    exportLog("pipeline=prefetch+async_encode+adaptive+yolo_face_overlap");

    struct PendingFrame {
      cv::Mat bgr;
      std::vector<cv::Rect> redact;
    };
    std::deque<PendingFrame> pending;

    struct DecodedFrame {
      cv::Mat bgr;
      int64_t index = 0;
    };

    struct InferBatch {
      std::vector<cv::Mat> mats;
      std::vector<int64_t> indices;
      std::vector<std::vector<YoloDetection>> yolo;
      std::vector<std::vector<cv::Rect>> zooms;
      size_t faceCrops = 0;
    };

    // Gears = GPU/CPU batch size. Face-bound exports prefer holding a mid/high
    // frame batch; zoom count is adapted separately (see zoomCap below).
    static constexpr int kGears[] = {2, 3, 4, 6, 8, 10, 12};
    static constexpr int kGearCount = static_cast<int>(sizeof(kGears) / sizeof(kGears[0]));
    int gear = 3;  // batch 6
    int zoomCapGear = 2;  // maps to {2,3,4,6,8}
    static constexpr int kZoomCaps[] = {2, 3, 4, 6, 8};
    static constexpr int kZoomGearCount =
        static_cast<int>(sizeof(kZoomCaps) / sizeof(kZoomCaps[0]));
    const int kMaxBatch = kGears[kGearCount - 1];
    HardwareLoadSampler loadSampler;
    loadSampler.sample();  // prime CPU/GPU counters
    auto lastAdjust = std::chrono::steady_clock::now();
    auto lastDiag = lastAdjust;
    auto exportT0 = lastAdjust;
    constexpr auto kAdjustEvery = std::chrono::seconds(10);
    constexpr auto kDiagEvery = std::chrono::seconds(5);
    constexpr float kUpBelow = 70.f;
    constexpr float kDownAbove = 82.f;

    // Timing accumulators for bottleneck diagnosis (reset each diag window).
    double msDecodeWait = 0, msYolo = 0, msFace = 0, msTrack = 0, msEncodePush = 0;
    int64_t diagFrames = 0;
    int diagBatches = 0;
    int starveDecode = 0;  // popped fewer frames than requested batch
    size_t maxDecodeQ = 0, maxEncodeQ = 0;
    size_t faceCropsTotal = 0;
    int faceSlicesTotal = 0;
    // Last diag window: face owns wall time. Used so adaptive never climbs zoom
    // while face-bound (that path cut fps ~17→7 in logs).
    bool faceBoundHint = facesEnabled_;

    BoundedQueue<DecodedFrame> decodeQ(static_cast<size_t>(kMaxBatch * 2));
    BoundedQueue<PendingFrame> encodeQ(64);
    std::atomic<bool> encodeFailed{false};

    std::thread decodeThread([&] {
      cv::Mat local;
      while (!cancel_ && decoder.read(local) && !local.empty()) {
        DecodedFrame df;
        df.bgr = local.clone();
        df.index = decoder.currentFrame() - 1;
        if (!decodeQ.push(std::move(df)))
          break;
      }
      decodeQ.close();
    });

    std::thread encodeThread([&] {
      PendingFrame job;
      while (encodeQ.pop(job)) {
        if (cancel_)
          break;
        filter.apply(job.bgr, job.redact);
        if (watermark_)
          drawExportWatermark(job.bgr);
        if (!encoder.write(job.bgr)) {
          encodeFailed = true;
          break;
        }
        written.fetch_add(1);
      }
    });

    auto enqueueWrite = [&](PendingFrame&& pf) -> bool {
      if (encodeFailed.load())
        return false;
      const auto t0 = std::chrono::steady_clock::now();
      const bool okPush = encodeQ.push(std::move(pf));
      msEncodePush += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      maxEncodeQ = std::max(maxEncodeQ, encodeQ.size());
      return okPush;
    };

    auto shutdownPipeline = [&]() {
      cancel_ = true;
      decodeQ.close();
      encodeQ.close();
      if (decodeThread.joinable())
        decodeThread.join();
      if (encodeThread.joinable())
        encodeThread.join();
    };

    auto processOne = [&](cv::Mat& frameBgr, int64_t frameIndex,
                          const std::vector<YoloDetection>& yoloDets,
                          const std::vector<FaceDet>& faces) -> bool {
      if (encodeFailed.load())
        return false;
      const int64_t processed = frameIndex + 1;
      const int pct =
          4 + static_cast<int>(std::min<int64_t>(86, processed * 86 / total));
      if (processed <= 15 || processed % 10 == 0)
        progress(pct, "Processing frame " + std::to_string(processed) + " / " +
                          std::to_string(total));
      else
        progress(pct, {});

      std::vector<std::pair<cv::Rect, std::string>> objects;
      if (yoloEnabled_ && yolo.isReady()) {
        for (const auto& d : yoloDets) {
          if (YoloDetectorDnn::isScreenClass(d.classId))
            objects.emplace_back(d.rect, d.label);
        }
        for (const auto& z : YoloDetectorDnn::toPlateZones(yoloDets))
          objects.emplace_back(z.rect, "plate-zone");
      }

      const auto personHeads =
          YoloDetectorDnn::allPersonHeads(yoloDets, frameBgr.cols, frameBgr.rows);
      std::vector<cv::Rect> appeared;
      auto activeAi = live.processFrame(frameBgr, frameIndex, faces, objects,
                                        &appeared, personHeads);
      if (facePolicy_ == FacePolicy::ReverseKeep && !gallery.empty() && reid.isReady()) {
        applyKeepFaceFrame(live, gallery, reid, landmarks, frameBgr, frameIndex, exportLog);
        activeAi = live.visibleAt(frameIndex);
      }

      for (const auto& m : manuals) {
        if (frameIndex < m.start)
          continue;
        if (!manualTrackers.count(m.id)) {
          auto trk = std::make_shared<CsrtTracker>();
          const cv::Rect seed = manualRects.count(m.id) ? manualRects[m.id] : m.rect;
          if (trk->init(frameBgr, seed))
            manualTrackers[m.id] = trk;
        }
        auto it = manualTrackers.find(m.id);
        if (it == manualTrackers.end() || !it->second)
          continue;
        cv::Rect bb = manualRects.count(m.id) ? manualRects[m.id] : m.rect;
        if (it->second->update(frameBgr, bb))
          manualRects[m.id] = bb;
      }

      std::vector<cv::Rect> redact;
      for (const auto& t : activeAi) {
        if (t.enabled)
          redact.push_back(t.rect);
      }
      for (const auto& m : manuals) {
        if (!m.enabled || frameIndex < m.start)
          continue;
        if (manualRects.count(m.id))
          redact.push_back(manualRects[m.id]);
      }
      for (const auto& s : statics) {
        if (!s.enabled || !s.activeAt(frameIndex))
          continue;
        redact.push_back(s.rect);
      }

      if (!appeared.empty() && !pending.empty()) {
        const int n = std::min(TrackManager::kAppearLookaheadFrames,
                               static_cast<int>(pending.size()));
        for (int i = static_cast<int>(pending.size()) - n;
             i < static_cast<int>(pending.size()); ++i) {
          pending[static_cast<size_t>(i)].redact.insert(
              pending[static_cast<size_t>(i)].redact.end(), appeared.begin(),
              appeared.end());
        }
      }

      pending.push_back({std::move(frameBgr), std::move(redact)});
      if (static_cast<int>(pending.size()) > holdFrames) {
        if (!enqueueWrite(std::move(pending.front()))) {
          shutdownPipeline();
          encoder.close();
          std::error_code ec;
          std::filesystem::remove(u8path(videoOnly), ec);
          fail(encoder.lastError().empty()
                   ? "Write failed during export."
                   : ("Write failed: " + encoder.lastError()));
          return false;
        }
        pending.pop_front();
      }
      return true;
    };

    auto pullBatch = [&](InferBatch& b) -> bool {
      const int batchN = kGears[gear];
      b = InferBatch{};
      b.mats.reserve(static_cast<size_t>(batchN));
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < batchN; ++i) {
        DecodedFrame df;
        if (!decodeQ.pop(df))
          break;
        b.mats.push_back(std::move(df.bgr));
        b.indices.push_back(df.index);
        maxDecodeQ = std::max(maxDecodeQ, decodeQ.size());
      }
      msDecodeWait += std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      if (b.mats.empty())
        return false;
      if (static_cast<int>(b.mats.size()) < batchN)
        ++starveDecode;
      return true;
    };

    auto runYoloOn = [&](InferBatch& b) {
      b.yolo.assign(b.mats.size(), {});
      const bool runYolo = yolo.isReady() && (yoloEnabled_ || facesEnabled_);
      if (!runYolo)
        return;
      const auto t0 = std::chrono::steady_clock::now();
      b.yolo = yolo.detectMany(b.mats);
      msYolo += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    };

    auto buildZooms = [&](InferBatch& b) {
      b.zooms.assign(b.mats.size(), {});
      b.faceCrops = b.mats.size();
      if (!facesEnabled_ || !detector.isReady())
        return;
      const auto follow = live.zoomFollowBoxes();
      const int zoomCap = kZoomCaps[zoomCapGear];
      for (size_t i = 0; i < b.mats.size(); ++i) {
        b.zooms[i] = YoloDetectorDnn::faceZoomRois(
            b.yolo[i], follow, b.mats[i].cols, b.mats[i].rows, zoomCap);
        b.faceCrops += b.zooms[i].size();
      }
    };

    auto runAdaptiveAndDiag = [&]() {
      const auto now = std::chrono::steady_clock::now();

      if (now - lastAdjust >= kAdjustEvery) {
        const HardwareLoad load = loadSampler.sample();
        const float peak = load.peakPercent();
        const int prevGear = gear;
        const int prevZoom = zoomCapGear;
        const bool decodeFull = decodeQ.size() + 2 >= decodeQ.capacity();
        // When face_infer owns the GPU and the decode queue stays full, the GPU
        // is usefully busy — shrinking the *frame* batch just adds more zoom
        // crops and hurts fps (seen: 8.9→6.9 after gear 6→4). Instead trim zooms.
        // Never climb zoom while face-bound / decode ahead (seen: zoom 4→6 → fps
        // 17→7). Peak dips from sampling noise must not add crops.
        if (peak >= 0.f) {
          if (peak > kDownAbove) {
            if (decodeFull && zoomCapGear > 0)
              --zoomCapGear;
            else if (!decodeFull && gear > 0)
              --gear;
            else if (zoomCapGear > 0)
              --zoomCapGear;
          } else if (peak < kUpBelow) {
            // Prefer larger frame batches first. Only touch zoom after gear is
            // maxed — and then only *down* while face-bound / decode ahead.
            if (gear + 1 < kGearCount) {
              ++gear;
            } else if (faceBoundHint && decodeFull) {
              if (zoomCapGear > 0)
                --zoomCapGear;
            } else if (!faceBoundHint && !decodeFull &&
                       zoomCapGear + 1 < kZoomGearCount) {
              ++zoomCapGear;
            }
          }
        }
        lastAdjust = now;
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(1);
        oss << "adaptive cpu=";
        if (load.cpuPercent >= 0.f)
          oss << load.cpuPercent;
        else
          oss << "n/a";
        oss << " gpu=";
        if (load.gpuPercent >= 0.f)
          oss << load.gpuPercent;
        else
          oss << "n/a";
        oss << " peak=" << peak << " gear " << prevGear << "->" << gear
            << " batch=" << kGears[gear] << " zoom " << prevZoom << "->"
            << zoomCapGear << " zoomCap=" << kZoomCaps[zoomCapGear]
            << (decodeFull ? " decodeQ=full" : " decodeQ=ok")
            << (faceBoundHint ? " faceBound=1" : " faceBound=0");
        exportLog(oss.str());
      }

      if (now - lastDiag >= kDiagEvery) {
        const HardwareLoad load = loadSampler.sample();
        double dPush = 0, dPop = 0, ePush = 0, ePop = 0;
        decodeQ.takeWaitStats(dPush, dPop);
        encodeQ.takeWaitStats(ePush, ePop);
        const double windowMs =
            std::chrono::duration<double, std::milli>(now - lastDiag).count();
        const double fps =
            windowMs > 1.0 ? (1000.0 * static_cast<double>(diagFrames) / windowMs)
                           : 0.0;
        const double sumStage =
            msDecodeWait + msYolo + msFace + msTrack + msEncodePush;
        auto pctOf = [&](double ms) -> double {
          return sumStage > 1.0 ? (100.0 * ms / sumStage) : 0.0;
        };

        std::string bottleneck = "unknown";
        double best = -1;
        auto consider = [&](const char* name, double ms) {
          if (ms > best) {
            best = ms;
            bottleneck = name;
          }
        };
        consider("decode_wait", msDecodeWait);
        consider("yolo_infer", msYolo);
        consider("face_infer", msFace);
        consider("track_cpu", msTrack);
        consider("encode_queue", msEncodePush);
        faceBoundHint =
            bottleneck == "face_infer" ||
            (sumStage > 1.0 && msFace > 0.55 * sumStage);

        std::ostringstream hint;
        if (load.peakPercent() >= 0.f && load.peakPercent() < 50.f) {
          if (bottleneck == "decode_wait")
            hint << " | HINT: GPU idle waiting for decoded frames (decode slow/starve)";
          else if (bottleneck == "encode_queue")
            hint << " | HINT: GPU path blocked — encode/mosaic queue full (encoder slow)";
          else if (bottleneck == "track_cpu")
            hint << " | HINT: tracking/reid serial CPU dominates between GPU launches";
          else if (bottleneck == "yolo_infer" || bottleneck == "face_infer")
            hint << " | HINT: inference is the limiter but util still low — check TaskMgr Compute_0 not 3D";
          else
            hint << " | HINT: peak util low; pipeline gaps between GPU batches";
        } else if (load.peakPercent() >= 80.f) {
          hint << " | HINT: near target load; gear should hold/drop";
        }
        if (starveDecode > 0)
          hint << " | starve_batches=" << starveDecode;
        if (dPush > 200.0)
          hint << " | decode_thread_blocked_on_full_queue";
        if (ePush > 200.0)
          hint << " | main_blocked_on_full_encode_queue";
        if (faceCropsTotal > 0 && diagBatches > 0) {
          const double cropsPerBatch =
              static_cast<double>(faceCropsTotal) / static_cast<double>(diagBatches);
          if (cropsPerBatch > 40.0)
            hint << " | HINT: too many face crops/batch (" << cropsPerBatch
                 << ") — zoom ROIs dominate GPU time";
        }

        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss.precision(1);
        oss << "diag "
            << "t=" << std::chrono::duration<double>(now - exportT0).count() << "s "
            << "fps=" << fps << " frames=" << diagFrames << " batches=" << diagBatches
            << " gear=" << gear << " batch=" << kGears[gear] << " cpu=";
        if (load.cpuPercent >= 0.f)
          oss << load.cpuPercent;
        else
          oss << "n/a";
        oss << " gpu=";
        if (load.gpuPercent >= 0.f)
          oss << load.gpuPercent;
        else
          oss << "n/a";
        oss << " peak=";
        if (load.peakPercent() >= 0.f)
          oss << load.peakPercent();
        else
          oss << "n/a";
        oss << " | ms decode_wait=" << msDecodeWait << "(" << pctOf(msDecodeWait)
            << "%) yolo=" << msYolo << "(" << pctOf(msYolo) << "%) face=" << msFace
            << "(" << pctOf(msFace) << "%) track=" << msTrack << "(" << pctOf(msTrack)
            << "%) enc_push=" << msEncodePush << "(" << pctOf(msEncodePush) << "%)"
            << " | face_crops=" << faceCropsTotal << " face_slices~" << faceSlicesTotal
            << " | q decode=" << decodeQ.size() << "/" << decodeQ.capacity()
            << " encode=" << encodeQ.size() << "/" << encodeQ.capacity()
            << " maxD=" << maxDecodeQ << " maxE=" << maxEncodeQ
            << " | qWaitMs d_push=" << dPush << " d_pop=" << dPop << " e_push=" << ePush
            << " e_pop=" << ePop << " | bottleneck=" << bottleneck << hint.str();
        exportLog(oss.str());

        msDecodeWait = msYolo = msFace = msTrack = msEncodePush = 0;
        diagFrames = 0;
        diagBatches = 0;
        starveDecode = 0;
        maxDecodeQ = maxEncodeQ = 0;
        faceCropsTotal = 0;
        faceSlicesTotal = 0;
        lastDiag = now;
      }
    };

    // Dual-buffer: Face(cur) ∥ pull+YOLO(next). Separate ORT sessions; same
    // crops / track order as the serial path (zooms for next after track(cur)).
    InferBatch cur;
    if (pullBatch(cur)) {
      runYoloOn(cur);
      buildZooms(cur);

      while (!cancel_ && !encodeFailed.load()) {
        runAdaptiveAndDiag();

        std::future<std::vector<std::vector<FaceDet>>> faceFut;
        const bool doFace = facesEnabled_ && detector.isReady();
        if (doFace) {
          faceCropsTotal += cur.faceCrops;
          faceSlicesTotal +=
              static_cast<int>((cur.faceCrops + 47) / 48);
          faceFut = std::async(std::launch::async, [&] {
            return detector.detectGrouped(cur.mats, cur.zooms);
          });
        }

        InferBatch next;
        const bool haveNext = pullBatch(next);
        if (haveNext)
          runYoloOn(next);

        std::vector<std::vector<FaceDet>> faceBatch(cur.mats.size());
        if (doFace) {
          const auto t0 = std::chrono::steady_clock::now();
          faceBatch = faceFut.get();
          msFace += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
          for (size_t i = 0; i < cur.mats.size(); ++i) {
            YoloDetectorDnn::appendHeadFallbacks(
                faceBatch[i], cur.yolo[i], cur.mats[i].cols, cur.mats[i].rows);
          }
        }

        ++diagBatches;
        diagFrames += static_cast<int64_t>(cur.mats.size());
        {
          const auto t0 = std::chrono::steady_clock::now();
          const double encBefore = msEncodePush;
          for (size_t i = 0; i < cur.mats.size(); ++i) {
            if (cancel_ || encodeFailed.load())
              break;
            if (!processOne(cur.mats[i], cur.indices[i], cur.yolo[i], faceBatch[i]))
              return;
          }
          const double wall = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
          const double encPart = msEncodePush - encBefore;
          msTrack += std::max(0.0, wall - encPart);
        }

        if (!haveNext)
          break;
        buildZooms(next);
        cur = std::move(next);
      }
    }

    if (decodeThread.joinable())
      decodeThread.join();
    decodeQ.close();

    progress(90, "Writing remaining frames...");
    while (!pending.empty()) {
      if (cancel_ || encodeFailed.load() || !enqueueWrite(std::move(pending.front()))) {
        shutdownPipeline();
        encoder.close();
        std::error_code ec;
        std::filesystem::remove(u8path(videoOnly), ec);
        fail(cancel_ ? "Export cancelled."
                     : (encoder.lastError().empty()
                            ? "Write failed during export."
                            : ("Write failed: " + encoder.lastError())));
        return;
      }
      pending.pop_front();
    }
    encodeQ.close();
    if (encodeThread.joinable())
      encodeThread.join();

    if (encodeFailed.load()) {
      encoder.close();
      std::error_code ec;
      std::filesystem::remove(u8path(videoOnly), ec);
      fail(encoder.lastError().empty()
               ? "Write failed during export."
               : ("Write failed: " + encoder.lastError()));
      return;
    }

    encoder.close();
    if (cancel_) {
      std::error_code ec;
      std::filesystem::remove(u8path(videoOnly), ec);
      fail("Export cancelled.");
      return;
    }

    progress(92, "Muxing audio...");
    const std::string ffmpeg = FfmpegAudioTools::findFfmpeg();
    std::string audioErr;
    bool audioOk = false;
    if (!ffmpeg.empty()) {
      audioOk = FfmpegAudioTools::remuxWithAudioRedaction(
          ffmpeg, inputPath_, videoOnly, outputPath_, audioRanges_, &audioErr);
    } else {
      audioErr = "ffmpeg missing";
    }

    if (audioOk) {
      std::error_code ec;
      std::filesystem::remove(u8path(videoOnly), ec);
      progress(100, "Done");
      ok(outputPath_);
      return;
    }

    std::error_code ec;
    if (fileExistsUtf8(outputPath_))
      std::filesystem::remove(u8path(outputPath_), ec);
    std::filesystem::rename(u8path(videoOnly), u8path(outputPath_), ec);
    if (ec) {
      std::filesystem::copy_file(u8path(videoOnly), u8path(outputPath_),
                                 std::filesystem::copy_options::overwrite_existing, ec);
      std::filesystem::remove(u8path(videoOnly), ec);
    }
    progress(100, "Done");
    ok(outputPath_);
  } catch (const std::exception& e) {
    fail(e.what());
  } catch (...) {
    fail("Unexpected error during export.");
  }
}

}  // namespace vb
