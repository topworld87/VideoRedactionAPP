#include "vb/HardwareInfo.h"

#include <algorithm>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#endif

namespace vb {
namespace {

GpuVendor vendorFromId(UINT vendorId) {
  switch (vendorId) {
  case 0x10DE:
    return GpuVendor::Nvidia;
  case 0x1002:
  case 0x1022:
    return GpuVendor::Amd;
  case 0x8086:
    return GpuVendor::Intel;
  default:
    return GpuVendor::Other;
  }
}

bool looksVirtualAdapter(const wchar_t* desc, UINT vendorId) {
  if (vendorId == 0x1414)  // Microsoft Basic Render Driver
    return true;
  if (!desc)
    return false;
  // Remote/virtual display drivers must not be used for DirectML / encode.
  const wchar_t* keys[] = {L"Microsoft Basic Render",
                           L"Oray",
                           L"IddDriver",
                           L"Virtual",
                           L"Remote",
                           L"Parsec",
                           L"VMware",
                           L"VirtualBox",
                           L"Hyper-V",
                           L"Citrix",
                           L"TeamViewer"};
  for (const wchar_t* k : keys) {
    if (wcsstr(desc, k) != nullptr)
      return true;
  }
  return false;
}

bool looksDiscrete(GpuVendor vendor, SIZE_T dedicated) {
  if (vendor == GpuVendor::Nvidia)
    return true;
  // Shared-memory iGPUs often report little dedicated VRAM.
  if (dedicated >= (512ull * 1024ull * 1024ull))
    return true;
  return false;
}

int scoreAdapter(const GpuAdapterInfo& a) {
  if (a.software || a.dxgiIndex < 0)
    return -1000000;
  int s = 0;
  if (a.vendor == GpuVendor::Nvidia)
    s += 4000;
  else if (a.vendor == GpuVendor::Amd)
    s += 3000;
  else if (a.vendor == GpuVendor::Intel)
    s += 2000;
  else
    s += 1000;
  if (a.discrete)
    s += 500;
  // Prefer more VRAM as a weak tie-break (MB).
  s += static_cast<int>(std::min<size_t>(a.dedicatedBytes / (1024ull * 1024ull), 8000ull));
  return s;
}

}  // namespace

const char* gpuVendorLabel(GpuVendor v) {
  switch (v) {
  case GpuVendor::Nvidia:
    return "NVIDIA";
  case GpuVendor::Amd:
    return "AMD";
  case GpuVendor::Intel:
    return "Intel";
  case GpuVendor::Other:
    return "GPU";
  default:
    return "CPU";
  }
}

HardwareInfo& HardwareInfo::instance() {
  static HardwareInfo info;
  return info;
}

HardwareInfo::HardwareInfo() { detect(); }

void HardwareInfo::detect() {
  adapters_.clear();
  preferredIndex_ = -1;
  preferredVendor_ = GpuVendor::Unknown;
  preferredName_.clear();
  encoderCandidates_.clear();

#ifdef _WIN32
  IDXGIFactory* factory = nullptr;
  if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                               reinterpret_cast<void**>(&factory))) ||
      !factory) {
    buildEncoderCandidates();
    return;
  }

  for (UINT i = 0;; ++i) {
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;
    if (!adapter)
      continue;

    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
      adapter->Release();
      continue;
    }

    GpuAdapterInfo info;
    info.dxgiIndex = static_cast<int>(i);
    info.vendor = vendorFromId(desc.VendorId);
    info.dedicatedBytes = static_cast<size_t>(desc.DedicatedVideoMemory);
    info.software = looksVirtualAdapter(desc.Description, desc.VendorId);
    info.discrete = !info.software && looksDiscrete(info.vendor, desc.DedicatedVideoMemory);

    char nameUtf8[256] = {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameUtf8,
                                      static_cast<int>(sizeof(nameUtf8) - 1), nullptr,
                                      nullptr);
    if (n > 0)
      info.name = nameUtf8;
    else
      info.name = "GPU";

    adapters_.push_back(std::move(info));
    adapter->Release();
  }
  factory->Release();
#endif

  pickPreferred();
  buildEncoderCandidates();
}

void HardwareInfo::pickPreferred() {
  int best = -1;
  int bestScore = -1000000;
  for (size_t i = 0; i < adapters_.size(); ++i) {
    const int s = scoreAdapter(adapters_[i]);
    if (s > bestScore) {
      bestScore = s;
      best = static_cast<int>(i);
    }
  }
  if (best < 0 || bestScore < 0)
    return;
  const GpuAdapterInfo& a = adapters_[static_cast<size_t>(best)];
  preferredIndex_ = a.dxgiIndex;
  preferredVendor_ = a.vendor;
  preferredName_ = a.name;
}

void HardwareInfo::buildEncoderCandidates() {
  encoderCandidates_.clear();
  auto add = [&](const char* c) {
    if (std::find(encoderCandidates_.begin(), encoderCandidates_.end(), c) ==
        encoderCandidates_.end())
      encoderCandidates_.emplace_back(c);
  };

  switch (preferredVendor_) {
  case GpuVendor::Nvidia:
    add("h264_nvenc");
    break;
  case GpuVendor::Amd:
    add("h264_amf");
    break;
  case GpuVendor::Intel:
    add("h264_qsv");
    break;
  default:
    break;
  }

  // Secondary hardware if the machine also has it (e.g. NVIDIA + Intel iGPU).
  for (const auto& a : adapters_) {
    if (a.software)
      continue;
    if (a.vendor == GpuVendor::Nvidia)
      add("h264_nvenc");
    else if (a.vendor == GpuVendor::Amd)
      add("h264_amf");
    else if (a.vendor == GpuVendor::Intel)
      add("h264_qsv");
  }

  // Soft fallbacks: MF often works when NVENC is listed but fails under load.
  add("h264_mf");
  add("libx264");
}

std::string HardwareInfo::summary() const {
  std::ostringstream os;
  if (preferredIndex_ < 0 || preferredName_.empty()) {
    os << "CPU only · encode libx264/MF";
    return os.str();
  }
  os << preferredName_;
  const char* enc = "MF";
  if (!encoderCandidates_.empty()) {
    if (encoderCandidates_[0] == "h264_nvenc")
      enc = "NVENC";
    else if (encoderCandidates_[0] == "h264_amf")
      enc = "AMF";
    else if (encoderCandidates_[0] == "h264_qsv")
      enc = "QSV";
    else if (encoderCandidates_[0] == "h264_mf")
      enc = "MF";
    else
      enc = "x264";
  }
  os << " · prefer " << enc << " · DML#" << preferredIndex_;
  return os.str();
}

}  // namespace vb
