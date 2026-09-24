#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

namespace vb {

enum class OrtEpKind { Cpu, DirectML };

class OrtRuntime {
public:
  static OrtRuntime& instance();

  Ort::Env& env();
  Ort::AllocatorWithDefaultOptions& allocator();

  bool preferGpu() const;
  void setPreferGpu(bool prefer);

  bool gpuBlacklisted() const;
  void blacklistGpu();

  static bool envForcesCpu();

private:
  OrtRuntime();

  Ort::Env env_;
  Ort::AllocatorWithDefaultOptions allocator_;
  mutable std::mutex mu_;
  bool preferGpu_ = true;
  bool gpuBlacklisted_ = false;
};

class OrtModelSession {
public:
  bool load(const std::string& onnxPathUtf8, bool tryGpu);
  bool isReady() const { return static_cast<bool>(session_); }
  OrtEpKind ep() const { return ep_; }
  const char* epLabel() const;

  bool run(const std::vector<const char*>& inputNames,
           const std::vector<Ort::Value>& inputs,
           const std::vector<const char*>& outputNames,
           std::vector<Ort::Value>& outputs, bool fallbackOnFail = true);
  bool run(const std::vector<Ort::Value>& inputs, std::vector<Ort::Value>& outputs);

  Ort::Session* session() { return session_.get(); }
  const std::vector<std::string>& inputNames() const { return inputNameStore_; }
  const std::vector<std::string>& outputNames() const { return outputNameStore_; }
  const std::vector<int64_t>& inputShape(size_t index = 0) const;
  int inputHeight() const;
  int inputWidth() const;

private:
  bool createSession(bool useGpu);
  void cacheIoNames();
  static void configureSessionOptions(Ort::SessionOptions& opts, bool useGpu);

  std::string path_;
  std::unique_ptr<Ort::Session> session_;
  OrtEpKind ep_ = OrtEpKind::Cpu;
  bool triedRunFallback_ = false;
  std::vector<std::string> inputNameStore_;
  std::vector<std::string> outputNameStore_;
  std::vector<const char*> inputNamePtrs_;
  std::vector<const char*> outputNamePtrs_;
  std::vector<std::vector<int64_t>> inputShapes_;
};

}  // namespace vb
