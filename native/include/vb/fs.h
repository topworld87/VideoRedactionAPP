#pragma once

#include <filesystem>
#include <string>

namespace vb {

inline std::filesystem::path u8path(const std::string& utf8) {
  return std::filesystem::u8path(utf8);
}

inline std::string pathToUtf8(const std::filesystem::path& p) {
  return p.u8string();
}

bool fileExistsUtf8(const std::string& utf8);
void appendTextLog(const std::string& fileName, const std::string& line,
                   bool truncate = false);
std::string exeDirUtf8();
std::string appDataDirUtf8();
// Install folder when it is writable, otherwise %APPDATA%\Video Blackout.
// MSIX installs the package read-only, so logs cannot stay next to the exe.
std::string writableDirUtf8();
// Short-lived files (encoder probes). Always outside the install folder.
std::string tempDirUtf8();
std::wstring utf8ToWide(const std::string& utf8);
std::string wideToUtf8(const std::wstring& wide);

}  // namespace vb
