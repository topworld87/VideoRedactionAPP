#pragma once

#include "vb/AudioRedactionItem.h"
#include "vb/IdentityGallery.h"
#include "vb/RedactionFilter.h"
#include "vb/TrackObject.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace vb {

class ExportEngine {
public:
  using ProgressFn = std::function<void(int percent, const std::string& status)>;
  using DoneFn = std::function<void(bool ok, const std::string& pathOrError)>;

  ExportEngine() = default;
  ~ExportEngine() { requestCancel(); join(); }

  void configure(const std::string& inputPath, const std::string& outputPath,
                 const std::string& faceModelPath, const std::string& yoloModelPath,
                 const std::string& landmarksModelPath, const std::string& reidModelPath,
                 RedactionMode mode, bool facesEnabled, bool yoloEnabled,
                 FacePolicy facePolicy, const IdentityGallery& gallery,
                 const std::vector<TrackObject>& tracksSnapshot,
                 const std::vector<AudioRedactionItem>& audioRanges,
                 bool watermark);

  void start(ProgressFn onProgress, DoneFn onDone);
  void requestCancel() { cancel_ = true; }
  void join();
  bool running() const { return running_.load(); }

private:
  void run();

  std::string inputPath_;
  std::string outputPath_;
  std::string faceModelPath_;
  std::string yoloModelPath_;
  std::string landmarksModelPath_;
  std::string reidModelPath_;
  RedactionMode mode_ = RedactionMode::Mosaic;
  bool facesEnabled_ = true;
  bool yoloEnabled_ = false;
  bool watermark_ = false;
  FacePolicy facePolicy_ = FacePolicy::FullRedact;
  IdentityGallery gallery_;
  std::vector<TrackObject> snap_;
  std::vector<AudioRedactionItem> audioRanges_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::thread worker_;
  ProgressFn onProgress_;
  DoneFn onDone_;
};

}  // namespace vb
