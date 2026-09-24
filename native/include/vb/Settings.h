#pragma once

#include <string>

namespace vb {

class Settings {
public:
  static Settings& instance();

  void load();
  void save() const;

  const std::string& language() const { return language_; }
  void setLanguage(const std::string& lang);

  const std::string& redactionMode() const { return redactionMode_; }
  void setRedactionMode(const std::string& mode);

  float scoreThreshold() const { return scoreThreshold_; }
  void setScoreThreshold(float v);

  bool hardwareAccel() const { return hardwareAccel_; }
  void setHardwareAccel(bool enabled);

private:
  Settings() = default;
  std::string language_;
  std::string redactionMode_ = "mosaic";
  float scoreThreshold_ = 0.45f;
  bool hardwareAccel_ = true;
};

}  // namespace vb
