#include "vb/HardwareLoad.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#endif

namespace vb {
namespace {

#ifdef _WIN32
unsigned long long fileTimeToU64(const FILETIME& ft) {
  return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
         ft.dwLowDateTime;
}
#endif

}  // namespace

HardwareLoadSampler::HardwareLoadSampler() { initGpu(); }

HardwareLoadSampler::~HardwareLoadSampler() {
#ifdef _WIN32
  if (pdhQuery_) {
    PdhCloseQuery(static_cast<PDH_HQUERY>(pdhQuery_));
    pdhQuery_ = nullptr;
    pdhGpuCounter_ = nullptr;
  }
#endif
}

void HardwareLoadSampler::initGpu() {
#ifdef _WIN32
  PDH_HQUERY query = nullptr;
  if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
    return;
  PDH_HCOUNTER counter = nullptr;
  // Aggregates all GPU engines (3D/Compute/Copy/...). We take the max instance.
  const PDH_STATUS st = PdhAddEnglishCounterW(
      query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
  if (st != ERROR_SUCCESS) {
    PdhCloseQuery(query);
    return;
  }
  PdhCollectQueryData(query);
  pdhQuery_ = query;
  pdhGpuCounter_ = counter;
  gpuOk_ = true;
  gpuPrimed_ = false;
#endif
}

bool HardwareLoadSampler::sampleCpu(float& outPercent) {
#ifdef _WIN32
  FILETIME idle{}, kernel{}, user{};
  if (!GetSystemTimes(&idle, &kernel, &user))
    return false;
  const unsigned long long i = fileTimeToU64(idle);
  const unsigned long long k = fileTimeToU64(kernel);
  const unsigned long long u = fileTimeToU64(user);
  if (!cpuPrimed_) {
    prevIdle_ = i;
    prevKernel_ = k;
    prevUser_ = u;
    cpuPrimed_ = true;
    outPercent = 0.f;
    return true;
  }
  const unsigned long long dIdle = i - prevIdle_;
  const unsigned long long dKernel = k - prevKernel_;
  const unsigned long long dUser = u - prevUser_;
  prevIdle_ = i;
  prevKernel_ = k;
  prevUser_ = u;
  // Kernel includes idle time on Windows.
  const unsigned long long total = dKernel + dUser;
  if (total == 0) {
    outPercent = 0.f;
    return true;
  }
  const unsigned long long busy = total - std::min(total, dIdle);
  outPercent = 100.f * static_cast<float>(busy) / static_cast<float>(total);
  outPercent = std::clamp(outPercent, 0.f, 100.f);
  return true;
#else
  (void)outPercent;
  return false;
#endif
}

bool HardwareLoadSampler::sampleGpu(float& outPercent) {
#ifdef _WIN32
  if (!gpuOk_ || !pdhQuery_ || !pdhGpuCounter_)
    return false;
  if (PdhCollectQueryData(static_cast<PDH_HQUERY>(pdhQuery_)) != ERROR_SUCCESS)
    return false;
  if (!gpuPrimed_) {
    gpuPrimed_ = true;
    outPercent = 0.f;
    return true;
  }

  DWORD bufSize = 0;
  DWORD itemCount = 0;
  PDH_STATUS st = PdhGetFormattedCounterArrayW(
      static_cast<PDH_HCOUNTER>(pdhGpuCounter_), PDH_FMT_DOUBLE, &bufSize,
      &itemCount, nullptr);
  if (st != PDH_MORE_DATA && st != ERROR_SUCCESS)
    return false;
  std::vector<unsigned char> buf(bufSize);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
  itemCount = 0;
  st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdhGpuCounter_),
                                    PDH_FMT_DOUBLE, &bufSize, &itemCount, items);
  if (st != ERROR_SUCCESS || itemCount == 0)
    return false;

  double peak = 0.0;
  for (DWORD i = 0; i < itemCount; ++i) {
    if (items[i].FmtValue.CStatus != ERROR_SUCCESS)
      continue;
    peak = std::max(peak, items[i].FmtValue.doubleValue);
  }
  outPercent = std::clamp(static_cast<float>(peak), 0.f, 100.f);
  return true;
#else
  (void)outPercent;
  return false;
#endif
}

HardwareLoad HardwareLoadSampler::sample() {
  HardwareLoad load;
  float cpu = -1.f;
  if (sampleCpu(cpu))
    load.cpuPercent = cpu;
  float gpu = -1.f;
  if (sampleGpu(gpu))
    load.gpuPercent = gpu;
  return load;
}

}  // namespace vb
