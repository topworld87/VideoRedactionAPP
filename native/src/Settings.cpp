#include "vb/Settings.h"

#include "vb/OrtRuntime.h"
#include "vb/fs.h"

#include <fstream>
#include <sstream>

namespace vb {
namespace {

std::string jsonGetString(const std::string& body, const std::string& key,
                          const std::string& fallback) {
  const std::string pat = "\"" + key + "\"";
  const auto k = body.find(pat);
  if (k == std::string::npos)
    return fallback;
  const auto colon = body.find(':', k + pat.size());
  if (colon == std::string::npos)
    return fallback;
  auto i = body.find_first_not_of(" \t\r\n", colon + 1);
  if (i == std::string::npos)
    return fallback;
  if (body[i] == '"') {
    const auto end = body.find('"', i + 1);
    if (end == std::string::npos)
      return fallback;
    return body.substr(i + 1, end - i - 1);
  }
  auto end = body.find_first_of(",}\r\n", i);
  if (end == std::string::npos)
    end = body.size();
  std::string raw = body.substr(i, end - i);
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
    raw.pop_back();
  return raw.empty() ? fallback : raw;
}

std::string settingsPath() {
  return pathToUtf8(u8path(appDataDirUtf8()) / "settings.json");
}

}  // namespace

Settings& Settings::instance() {
  static Settings s;
  return s;
}

void Settings::load() {
  std::ifstream in(u8path(settingsPath()), std::ios::binary);
  if (!in)
    return;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string body = ss.str();
  language_ = jsonGetString(body, "language", language_);
  redactionMode_ = jsonGetString(body, "redactionMode", redactionMode_);
  try {
    scoreThreshold_ = std::stof(jsonGetString(body, "scoreThreshold", "0.45"));
  } catch (...) {
  }
  const std::string hw = jsonGetString(body, "hardwareAccel", "true");
  hardwareAccel_ = !(hw == "false" || hw == "0");
  OrtRuntime::instance().setPreferGpu(hardwareAccel_);
}

void Settings::save() const {
  std::ofstream out(u8path(settingsPath()), std::ios::binary | std::ios::trunc);
  if (!out)
    return;
  out << "{\n"
      << "  \"language\": \"" << language_ << "\",\n"
      << "  \"redactionMode\": \"" << redactionMode_ << "\",\n"
      << "  \"scoreThreshold\": " << scoreThreshold_ << ",\n"
      << "  \"hardwareAccel\": " << (hardwareAccel_ ? "true" : "false") << "\n"
      << "}\n";
}

void Settings::setLanguage(const std::string& lang) {
  language_ = lang;
  save();
}

void Settings::setRedactionMode(const std::string& mode) {
  redactionMode_ = mode;
  save();
}

void Settings::setScoreThreshold(float v) {
  scoreThreshold_ = v;
  save();
}

void Settings::setHardwareAccel(bool enabled) {
  hardwareAccel_ = enabled;
  OrtRuntime::instance().setPreferGpu(enabled);
  save();
}

}  // namespace vb
