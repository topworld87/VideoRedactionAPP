#include "vb/FfmpegAudioTools.h"

#include "vb/fs.h"
#include "vb/platform/ChildProcess.h"

#include <sstream>

namespace vb {
namespace {

std::string betweenExpr(double startSec, double endSec) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss.precision(3);
  ss << "between(t\\," << startSec << "\\," << endSec << ")";
  return ss.str();
}

std::string orBetweenExpr(const std::vector<AudioRedactionItem>& ranges, bool beepsOnly) {
  std::string out;
  for (const auto& r : ranges) {
    if (!r.valid())
      continue;
    if (beepsOnly && r.effect != AudioEffect::Beep)
      continue;
    if (!out.empty())
      out += '+';
    out += betweenExpr(r.start_time_sec, r.end_time_sec);
  }
  return out;
}

std::string firstExisting(const std::vector<std::string>& cands) {
  for (const auto& c : cands) {
    if (fileExistsUtf8(c))
      return c;
  }
  return {};
}

}  // namespace

std::string FfmpegAudioTools::findFfmpeg() {
  const std::string appDir = exeDirUtf8();
  auto p = firstExisting({
      pathToUtf8(u8path(appDir) / "ffmpeg.exe"),
      pathToUtf8(u8path(appDir) / "tools" / "ffmpeg" / "ffmpeg.exe"),
      pathToUtf8(u8path(appDir) / ".." / "app" / "tools" / "ffmpeg" / "ffmpeg.exe"),
      pathToUtf8(u8path(appDir) / ".." / ".." / "app" / "tools" / "ffmpeg" /
                 "ffmpeg.exe"),
  });
  if (!p.empty())
    return p;
  return findExecutableOnPath("ffmpeg");
}

static std::string firstModel(const std::string& name) {
  const std::string appDir = exeDirUtf8();
  return firstExisting({
      pathToUtf8(u8path(appDir) / "models" / name),
      pathToUtf8(u8path(appDir) / ".." / "models" / name),
      pathToUtf8(u8path(appDir) / ".." / "app" / "models" / name),
      pathToUtf8(u8path(appDir) / ".." / ".." / "app" / "models" / name),
  });
}

std::string defaultFaceModelPath() {
  auto p = firstModel("yolo11n-face.onnx");
  if (!p.empty())
    return p;
  p = firstModel("face_detection_yunet_2023mar.onnx");
  if (!p.empty())
    return p;
  return firstModel("face-detection-adas-0001.onnx");
}

std::string defaultLandmarksModelPath() {
  return firstModel("landmarks-regression-retail-0009.onnx");
}

std::string defaultReidModelPath() {
  return firstModel("face-reidentification-retail-0095.onnx");
}

std::string defaultYoloModelPath() { return firstModel("yolov8n.onnx"); }

bool FfmpegAudioTools::remuxWithAudioRedaction(
    const std::string& ffmpegPath, const std::string& sourceVideo,
    const std::string& redactedVideo, const std::string& outputPath,
    const std::vector<AudioRedactionItem>& ranges, std::string* errorOut) {
  if (!fileExistsUtf8(ffmpegPath)) {
    if (errorOut)
      *errorOut = "ffmpeg.exe not found";
    return false;
  }
  if (!fileExistsUtf8(sourceVideo) || !fileExistsUtf8(redactedVideo)) {
    if (errorOut)
      *errorOut = "Input file missing for audio remux";
    return false;
  }

  std::vector<std::string> args = {"-y", "-i", redactedVideo, "-i", sourceVideo};
  const std::string muteEnable = orBetweenExpr(ranges, false);
  const std::string beepEnable = orBetweenExpr(ranges, true);

  if (muteEnable.empty()) {
    args.insert(args.end(), {"-map", "0:v:0", "-map", "1:a:0?", "-c:v", "copy",
                             "-c:a", "aac", "-shortest", outputPath});
  } else if (beepEnable.empty()) {
    const std::string af = "volume=enable='" + muteEnable + "':volume=0";
    args.insert(args.end(), {"-filter_complex", "[1:a]" + af + "[aout]", "-map",
                             "0:v:0", "-map", "[aout]", "-c:v", "copy", "-c:a",
                             "aac", "-shortest", outputPath});
  } else {
    const std::string beepVolume = "if(" + beepEnable + "\\,0.25\\,0)";
    const std::string fc =
        "[1:a]volume=enable='" + muteEnable +
        "':volume=0[a0];sine=frequency=1000:sample_rate=44100[braw];"
        "[braw]volume=eval=frame:volume='" +
        beepVolume +
        "'[beep];[a0][beep]amix=inputs=2:duration=first:dropout_transition=0[aout]";
    args.insert(args.end(), {"-filter_complex", fc, "-map", "0:v:0", "-map",
                             "[aout]", "-c:v", "copy", "-c:a", "aac", "-shortest",
                             outputPath});
  }

  ChildProcess proc;
  if (!proc.start(ffmpegPath, args, false)) {
    if (errorOut)
      *errorOut = "Failed to start ffmpeg";
    return false;
  }
  const int code = proc.waitFinished(3600000);
  if (code != 0) {
    if (errorOut) {
      const std::string out = proc.readAllOutput();
      *errorOut = out.size() > 500 ? out.substr(out.size() - 500) : out;
      if (errorOut->empty())
        *errorOut = "ffmpeg failed";
    }
    return false;
  }
  return fileExistsUtf8(outputPath);
}

}  // namespace vb
