#include "vb/fs.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

namespace vb {

bool fileExistsUtf8(const std::string& utf8) {
  std::error_code ec;
  return std::filesystem::exists(u8path(utf8), ec);
}

std::wstring utf8ToWide(const std::string& utf8) {
#ifdef _WIN32
  if (utf8.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
  if (n > 1)
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), n);
  return out;
#else
  return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string wideToUtf8(const std::wstring& wide) {
#ifdef _WIN32
  if (wide.empty())
    return {};
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
#else
  return std::string(wide.begin(), wide.end());
#endif
}

void appendTextLog(const std::string& fileName, const std::string& line, bool truncate) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  std::ofstream out(u8path(pathToUtf8(u8path(writableDirUtf8()) / fileName)),
                    truncate ? std::ios::trunc : std::ios::app);
  if (!out)
    return;
  out << line << '\n';
}

std::string exeDirUtf8() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return ".";
  std::filesystem::path p(buf);
  return pathToUtf8(p.parent_path());
#else
  return ".";
#endif
}

std::string appDataDirUtf8() {
#ifdef _WIN32
  wchar_t* appdata = nullptr;
  std::string base;
  size_t len = 0;
  if (_wdupenv_s(&appdata, &len, L"APPDATA") == 0 && appdata) {
    base = wideToUtf8(appdata);
    free(appdata);
  }
  if (base.empty())
    base = ".";
  const auto dir = u8path(base) / "Video Blackout";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return pathToUtf8(dir);
#else
  const char* home = std::getenv("HOME");
  auto dir = u8path(home ? home : ".") / "Library" / "Application Support" /
             "Video Blackout";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return pathToUtf8(dir);
#endif
}

std::string writableDirUtf8() {
  static std::string cached;
  static std::once_flag once;
  std::call_once(once, [] {
    const std::string exeDir = exeDirUtf8();
    std::error_code ec;
    const auto probe = u8path(exeDir) / ".vb_write_probe";
    {
      std::ofstream out(probe, std::ios::trunc | std::ios::binary);
      if (out) {
        out << "ok";
        out.close();
        std::filesystem::remove(probe, ec);
        cached = exeDir;
        return;
      }
    }
    cached = appDataDirUtf8();
  });
  return cached;
}

std::string tempDirUtf8() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, buf);
  std::filesystem::path dir = (n > 0 && n < MAX_PATH)
                                  ? std::filesystem::path(buf) / "VideoBlackout"
                                  : u8path(writableDirUtf8());
#else
  std::filesystem::path dir = std::filesystem::path("/tmp") / "VideoBlackout";
#endif
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return pathToUtf8(dir);
}

}  // namespace vb
