#pragma once

namespace vb {

struct HardwareLoad {
  float cpuPercent = -1.f;  // 0..100, or <0 if unknown
  float gpuPercent = -1.f;  // 0..100, or <0 if unknown
  float peakPercent() const {
    if (cpuPercent < 0.f && gpuPercent < 0.f)
      return -1.f;
    if (cpuPercent < 0.f)
      return gpuPercent;
    if (gpuPercent < 0.f)
      return cpuPercent;
    return cpuPercent > gpuPercent ? cpuPercent : gpuPercent;
  }
};

/// Samples process-machine CPU and (when available) GPU engine utilization.
class HardwareLoadSampler {
public:
  HardwareLoadSampler();
  ~HardwareLoadSampler();

  HardwareLoadSampler(const HardwareLoadSampler&) = delete;
  HardwareLoadSampler& operator=(const HardwareLoadSampler&) = delete;

  /// Call twice ~1s+ apart for the first meaningful GPU reading; afterwards
  /// each call returns the delta since the previous call.
  HardwareLoad sample();

  bool gpuAvailable() const { return gpuOk_; }

private:
  bool sampleCpu(float& outPercent);
  bool sampleGpu(float& outPercent);
  void initGpu();

  unsigned long long prevIdle_ = 0;
  unsigned long long prevKernel_ = 0;
  unsigned long long prevUser_ = 0;
  bool cpuPrimed_ = false;

  void* pdhQuery_ = nullptr;     // PDH_HQUERY
  void* pdhGpuCounter_ = nullptr;
  bool gpuOk_ = false;
  bool gpuPrimed_ = false;
};

}  // namespace vb
