#pragma once

namespace vb {

enum class AudioEffect { Mute, Beep };

struct AudioRedactionItem {
  int id = 0;
  double start_time_sec = 0.0;
  double end_time_sec = 0.0;
  AudioEffect effect = AudioEffect::Mute;

  bool valid() const {
    return end_time_sec > start_time_sec && start_time_sec >= 0.0;
  }
};

}  // namespace vb
