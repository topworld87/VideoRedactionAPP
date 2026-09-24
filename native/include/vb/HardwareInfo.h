#pragma once

#include <string>
#include <vector>

namespace vb {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel, Other };

struct GpuAdapterInfo {
  int dxgiIndex = -1;
  GpuVendor vendor = GpuVendor::Unknown;
  bool discrete = false;
  bool software = false;
  size_t dedicatedBytes = 0;
  std::string name;
};

/// Runtime GPU / encode preference. Detected once per process via DXGI.
class HardwareInfo {
public:
  static HardwareInfo& instance();

  const std::vector<GpuAdapterInfo>& adapters() const { return adapters_; }
  bool hasGpu() const { return preferredIndex_ >= 0; }
  int preferredDmlDeviceId() const { return preferredIndex_; }
  GpuVendor preferredVendor() const { return preferredVendor_; }
  const std::string& preferredGpuName() const { return preferredName_; }

  /// H.264 encoder candidates ordered for this machine
  /// (NVENC → AMF → QSV → MediaFoundation → libx264).
  const std::vector<std::string>& h264EncoderCandidates() const {
    return encoderCandidates_;
  }

  /// Short label for UI / logs, e.g. "NVIDIA GeForce GTX 1650 · prefer NVENC".
  std::string summary() const;

private:
  HardwareInfo();
  void detect();
  void pickPreferred();
  void buildEncoderCandidates();

  std::vector<GpuAdapterInfo> adapters_;
  int preferredIndex_ = -1;
  GpuVendor preferredVendor_ = GpuVendor::Unknown;
  std::string preferredName_;
  std::vector<std::string> encoderCandidates_;
};

const char* gpuVendorLabel(GpuVendor v);

}  // namespace vb
