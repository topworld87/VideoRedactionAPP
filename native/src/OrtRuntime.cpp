#include "vb/OrtRuntime.h"

#include "vb/HardwareInfo.h"
#include "vb/fs.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

extern "C" {
OrtStatus* ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(
    OrtSessionOptions* options, int device_id);
}

namespace vb {

OrtRuntime& OrtRuntime::instance() {
  static OrtRuntime r;
  return r;
}

OrtRuntime::OrtRuntime() : env_(ORT_LOGGING_LEVEL_WARNING, "VideoBlackout") {}

Ort::Env& OrtRuntime::env() { return env_; }

Ort::AllocatorWithDefaultOptions& OrtRuntime::allocator() { return allocator_; }

bool OrtRuntime::envForcesCpu() {
  const char* v = std::getenv("VIDEOBLACKOUT_ORT_EP");
  return v && (std::strcmp(v, "cpu") == 0 || std::strcmp(v, "CPU") == 0);
}

bool OrtRuntime::preferGpu() const {
  if (envForcesCpu())
    return false;
  if (!HardwareInfo::instance().hasGpu())
    return false;
  std::lock_guard<std::mutex> lock(mu_);
  return preferGpu_ && !gpuBlacklisted_;
}

void OrtRuntime::setPreferGpu(bool prefer) {
  std::lock_guard<std::mutex> lock(mu_);
  preferGpu_ = prefer;
}

void OrtRuntime::blacklistGpu() {
  std::lock_guard<std::mutex> lock(mu_);
  gpuBlacklisted_ = true;
}

bool OrtRuntime::gpuBlacklisted() const {
  std::lock_guard<std::mutex> lock(mu_);
  return gpuBlacklisted_;
}

void OrtModelSession::configureSessionOptions(Ort::SessionOptions& opts, bool useGpu) {
  opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  opts.SetIntraOpNumThreads(0);
  if (useGpu) {
#ifdef _WIN32
    opts.DisableMemPattern();
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    // Prefer the scored adapter (usually NVIDIA dGPU). Fall back to 0 only
    // when detection found nothing usable.
    int deviceId = HardwareInfo::instance().preferredDmlDeviceId();
    if (deviceId < 0)
      deviceId = 0;
    Ort::ThrowOnError(
        OrtSessionOptionsAppendExecutionProvider_DML(opts, deviceId));
#else
    (void)opts;
#endif
  }
}

void OrtModelSession::cacheIoNames() {
  inputNameStore_.clear();
  outputNameStore_.clear();
  inputNamePtrs_.clear();
  outputNamePtrs_.clear();
  inputShapes_.clear();
  if (!session_)
    return;

  auto& alloc = OrtRuntime::instance().allocator();
  const size_t nIn = session_->GetInputCount();
  inputNameStore_.reserve(nIn);
  inputShapes_.reserve(nIn);
  for (size_t i = 0; i < nIn; ++i) {
    auto name = session_->GetInputNameAllocated(i, alloc);
    inputNameStore_.emplace_back(name.get());
    try {
      inputShapes_.push_back(
          session_->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
    } catch (...) {
      inputShapes_.push_back({});
    }
  }
  const size_t nOut = session_->GetOutputCount();
  outputNameStore_.reserve(nOut);
  for (size_t i = 0; i < nOut; ++i) {
    auto name = session_->GetOutputNameAllocated(i, alloc);
    outputNameStore_.emplace_back(name.get());
  }
  for (const auto& s : inputNameStore_)
    inputNamePtrs_.push_back(s.c_str());
  for (const auto& s : outputNameStore_)
    outputNamePtrs_.push_back(s.c_str());
}

bool OrtModelSession::createSession(bool useGpu) {
  session_.reset();
  ep_ = useGpu ? OrtEpKind::DirectML : OrtEpKind::Cpu;
  try {
    Ort::SessionOptions opts;
    configureSessionOptions(opts, useGpu);
#ifdef _WIN32
    session_ = std::make_unique<Ort::Session>(OrtRuntime::instance().env(),
                                              utf8ToWide(path_).c_str(), opts);
#else
    session_ = std::make_unique<Ort::Session>(OrtRuntime::instance().env(),
                                              path_.c_str(), opts);
#endif
    cacheIoNames();
    return true;
  } catch (const Ort::Exception&) {
    session_.reset();
    return false;
  } catch (const std::exception&) {
    session_.reset();
    return false;
  }
}

bool OrtModelSession::load(const std::string& onnxPathUtf8, bool tryGpu) {
  path_ = onnxPathUtf8;
  session_.reset();
  triedRunFallback_ = false;
  inputNameStore_.clear();
  outputNameStore_.clear();
  inputNamePtrs_.clear();
  outputNamePtrs_.clear();
  inputShapes_.clear();

  const bool wantGpu = tryGpu && OrtRuntime::instance().preferGpu();
  if (wantGpu) {
    if (createSession(true))
      return true;
    OrtRuntime::instance().blacklistGpu();
  }
  return createSession(false);
}

const std::vector<int64_t>& OrtModelSession::inputShape(size_t index) const {
  static const std::vector<int64_t> kEmpty;
  if (index >= inputShapes_.size())
    return kEmpty;
  return inputShapes_[index];
}

int OrtModelSession::inputHeight() const {
  const auto& s = inputShape(0);
  if (s.size() >= 4)
    return static_cast<int>(s[2]);
  return 0;
}

int OrtModelSession::inputWidth() const {
  const auto& s = inputShape(0);
  if (s.size() >= 4)
    return static_cast<int>(s[3]);
  return 0;
}

const char* OrtModelSession::epLabel() const {
  return ep_ == OrtEpKind::DirectML ? "DirectML" : "CPU";
}

bool OrtModelSession::run(const std::vector<const char*>& inputNames,
                          const std::vector<Ort::Value>& inputs,
                          const std::vector<const char*>& outputNames,
                          std::vector<Ort::Value>& outputs, bool fallbackOnFail) {
  if (!session_)
    return false;

  auto tryRun = [&]() -> bool {
    try {
      Ort::RunOptions runOpts;
      outputs = session_->Run(runOpts, inputNames.data(), inputs.data(),
                              inputs.size(), outputNames.data(), outputNames.size());
      return true;
    } catch (const Ort::Exception&) {
      return false;
    } catch (const std::exception&) {
      return false;
    }
  };

  if (tryRun())
    return true;

  if (!fallbackOnFail)
    return false;

  if (ep_ == OrtEpKind::DirectML && !triedRunFallback_) {
    triedRunFallback_ = true;
    OrtRuntime::instance().blacklistGpu();
    if (!createSession(false))
      return false;
    return tryRun();
  }
  return false;
}

bool OrtModelSession::run(const std::vector<Ort::Value>& inputs,
                          std::vector<Ort::Value>& outputs) {
  if (inputNamePtrs_.empty() || outputNamePtrs_.empty())
    return false;
  return run(inputNamePtrs_, inputs, outputNamePtrs_, outputs);
}

}  // namespace vb
