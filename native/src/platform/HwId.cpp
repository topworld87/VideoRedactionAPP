#include "vb/fs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <cstdio>
#include <string>
#include <vector>

namespace vb {

#ifdef _WIN32

static bool readMachineGuid(std::string& out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                    KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buf[128] = {};
  DWORD bytes = sizeof(buf);
  DWORD type = 0;
  const LONG st =
      RegQueryValueExW(key, L"MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
    return false;
  out = wideToUtf8(buf);
  return !out.empty();
}

static bool uuidAllZero(const unsigned char* u) {
  for (int i = 0; i < 16; ++i) {
    if (u[i] != 0)
      return false;
  }
  return true;
}

static bool uuidAllOnes(const unsigned char* u) {
  for (int i = 0; i < 16; ++i) {
    if (u[i] != 0xff)
      return false;
  }
  return true;
}

/** Same source as OfflineRedactor: `wmic csproduct get uuid` / Win32_ComputerSystemProduct.UUID */
static bool readSmbiosSystemUuid(std::string& out) {
  const DWORD need = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
  if (need == 0)
    return false;

  std::vector<unsigned char> buf(need);
  if (GetSystemFirmwareTable('RSMB', 0, buf.data(), need) != need)
    return false;
  if (buf.size() < 8)
    return false;

  // RawSMBIOSData: 4 byte header + DWORD Length + table bytes
  const DWORD tableLen = *reinterpret_cast<DWORD*>(buf.data() + 4);
  if (tableLen == 0 || 8 + tableLen > buf.size())
    return false;

  const unsigned char* p = buf.data() + 8;
  const unsigned char* end = p + tableLen;

  while (p + 4 <= end) {
    const unsigned char type = p[0];
    const unsigned char length = p[1];
    if (length < 4)
      break;
    if (p + length > end)
      break;

    if (type == 1 && length >= 0x19) {
      const unsigned char* uuid = p + 0x08;
      if (!uuidAllZero(uuid) && !uuidAllOnes(uuid)) {
        // SMBIOS UUID byte order matches what `wmic csproduct get uuid` prints.
        char text[64];
        std::snprintf(
            text, sizeof(text),
            "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            uuid[3], uuid[2], uuid[1], uuid[0], uuid[5], uuid[4], uuid[7], uuid[6], uuid[8],
            uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        out = text;
        return true;
      }
    }

    // Skip formatted area + trailing string-set (double NUL terminated).
    const unsigned char* q = p + length;
    while (q + 1 < end && !(q[0] == 0 && q[1] == 0))
      ++q;
    if (q + 1 >= end)
      break;
    p = q + 2;
  }
  return false;
}

#endif

std::string currentHwId() {
#ifdef _WIN32
  // Match OfflineRedactor/presidio-main platform_support._hwid_windows():
  // durable SMBIOS system UUID (same value as `wmic csproduct get uuid`).
  // Do NOT mix computer name / C: volume serial — those caused different hashes
  // on an unchanged PC.
  std::string id;
  if (readSmbiosSystemUuid(id))
    return id;

  // Fallback when firmware UUID is missing/all-zero (some VMs / odd boards).
  if (readMachineGuid(id))
    return id;

  return {};
#else
  return "unsupported-platform";
#endif
}

}  // namespace vb
