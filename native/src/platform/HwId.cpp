#include "vb/fs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <intrin.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#include <string>
#include <vector>

namespace vb {

std::string currentHwId() {
#ifdef _WIN32
  DWORD serial = 0;
  GetVolumeInformationW(L"C:\\", nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);

  int cpuInfo[4] = {0};
  __cpuid(cpuInfo, 0);
  std::vector<unsigned char> raw;
  raw.insert(raw.end(), reinterpret_cast<unsigned char*>(&serial),
             reinterpret_cast<unsigned char*>(&serial) + sizeof(serial));
  raw.insert(raw.end(), reinterpret_cast<unsigned char*>(cpuInfo),
             reinterpret_cast<unsigned char*>(cpuInfo) + sizeof(cpuInfo));
  __cpuid(cpuInfo, 1);
  raw.insert(raw.end(), reinterpret_cast<unsigned char*>(cpuInfo),
             reinterpret_cast<unsigned char*>(cpuInfo) + sizeof(cpuInfo));

  wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  GetComputerNameW(host, &n);
  const std::string hostUtf8 = wideToUtf8(host);
  raw.insert(raw.end(), hostUtf8.begin(), hostUtf8.end());

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    return {};
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return {};
  }
  BCryptHashData(hash, raw.data(), static_cast<ULONG>(raw.size()), 0);
  unsigned char digest[32];
  BCryptFinishHash(hash, digest, 32, 0);
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(alg, 0);

  static const char* hex = "0123456789abcdef";
  std::string out;
  out.resize(64);
  for (int i = 0; i < 32; ++i) {
    out[static_cast<size_t>(i * 2)] = hex[digest[i] >> 4];
    out[static_cast<size_t>(i * 2 + 1)] = hex[digest[i] & 0xf];
  }
  return out;
#else
  return "unsupported-platform";
#endif
}

}  // namespace vb
