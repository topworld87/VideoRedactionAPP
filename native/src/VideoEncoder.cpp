#include "vb/VideoEncoder.h"

#include "vb/FfmpegAudioTools.h"
#include "vb/HardwareInfo.h"
#include "vb/fs.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <thread>
#include <vector>
#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace vb {
namespace {

std::string probeFfmpegEncoders(const std::string& ffmpegPath) {
  ChildProcess probe;
  if (!probe.start(ffmpegPath, {"-hide_banner", "-encoders"}, false))
    return {};
  const int code = probe.waitFinished(8000);
  const std::string out = probe.readAllOutput();
  if (code < 0)
    probe.kill();
  return out;
}

bool encoderListed(const std::string& listing, const std::string& name) {
  return listing.find(name) != std::string::npos;
}

int evenDim(int v) { return std::max(2, v - (v & 1)); }

}  // namespace

int VideoEncoder::targetBitrateKbps(int width, int height, double fps,
                                    double sourceBitrateKbps) {
  if (fps <= 1e-3)
    fps = 25.0;
  const double bpp = 0.14;
  const int estimated =
      std::max(2000, static_cast<int>(std::lround(width * height * fps * bpp / 1000.0)));
  if (sourceBitrateKbps > 100.0)
    return std::max(estimated, static_cast<int>(std::lround(sourceBitrateKbps * 1.05)));
  return estimated;
}

std::string VideoEncoder::pickFfmpegVideoCodec(const std::string& ffmpegPath) {
  const std::string out = probeFfmpegEncoders(ffmpegPath);
  if (out.empty())
    return {};
  for (const auto& name : HardwareInfo::instance().h264EncoderCandidates()) {
    if (encoderListed(out, name))
      return name;
  }
  return {};
}

bool VideoEncoder::tryStartFfmpeg(const std::string& ffmpegPath, const std::string& codec,
                                  const std::string& pathUtf8, double fps, int width,
                                  int height, int bitrateKbps) {
  ffmpeg_.kill();
  ffmpeg_.waitFinished(500);
  ffmpeg_.close();
  lastError_.clear();

  std::ostringstream size;
  size << width << "x" << height;
  std::ostringstream rate;
  rate.setf(std::ios::fixed);
  rate.precision(3);
  rate << fps;
  std::ostringstream br;
  br << bitrateKbps << "k";

  std::vector<std::string> args = {
      "-y",        "-loglevel", "error", "-f",     "rawvideo", "-pix_fmt",
      "bgr24",     "-s",        size.str(), "-r",  rate.str(), "-i",
      "-",         "-an",       "-c:v",     codec};
  if (codec == "libx264") {
    args.insert(args.end(), {"-preset", "veryfast", "-crf", "18"});
  } else if (codec == "h264_nvenc") {
    // Compatible with older mobile NVENC; avoid presets that exit immediately.
    args.insert(args.end(), {"-preset", "p4", "-rc", "vbr", "-b:v", br.str(),
                             "-maxrate", br.str(), "-gpu", "0"});
  } else if (codec == "h264_qsv") {
    args.insert(args.end(), {"-preset", "veryfast", "-b:v", br.str()});
  } else if (codec == "h264_amf") {
    args.insert(args.end(), {"-quality", "speed", "-b:v", br.str()});
  } else {
    args.insert(args.end(), {"-b:v", br.str()});
  }
  args.insert(args.end(), {"-pix_fmt", "yuv420p", pathUtf8});

  if (!ffmpeg_.start(ffmpegPath, args, true)) {
    lastError_ = "failed to start ffmpeg/" + codec;
    return false;
  }
#ifdef _WIN32
  Sleep(150);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
#endif
  if (!ffmpeg_.running()) {
    lastError_ = "ffmpeg/" + codec + " exited on start: " + ffmpeg_.readAllOutput();
    ffmpeg_.kill();
    ffmpeg_.waitFinished(500);
    ffmpeg_.close();
    return false;
  }
  return true;
}

bool VideoEncoder::probeCodec(const std::string& ffmpegPath, const std::string& codec,
                              double fps, int width, int height, int bitrateKbps) {
  const std::string probePath =
      pathToUtf8(u8path(tempDirUtf8()) / ("vb_enc_probe_" + codec + ".mp4"));
  std::error_code ec;
  std::filesystem::remove(u8path(probePath), ec);

  if (!tryStartFfmpeg(ffmpegPath, codec, probePath, fps, width, height, bitrateKbps))
    return false;

  cv::Mat black(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
  const size_t nbytes = black.total() * black.elemSize();
  const bool wrote = ffmpeg_.writeStdin(black.data, nbytes);
  ffmpeg_.closeStdin();
  const int code = ffmpeg_.waitFinished(8000);
  const std::string errOut = ffmpeg_.readAllOutput();
  ffmpeg_.kill();
  ffmpeg_.waitFinished(500);
  ffmpeg_.close();

  const bool okFile = fileExistsUtf8(probePath);
  std::filesystem::remove(u8path(probePath), ec);
  if (!wrote || code != 0 || !okFile) {
    lastError_ = "probe failed ffmpeg/" + codec;
    if (!errOut.empty())
      lastError_ += ": " + errOut;
    return false;
  }
  return true;
}

bool VideoEncoder::openFfmpegPipe(const std::string& pathUtf8, double fps, int width,
                                  int height, int bitrateKbps) {
  const std::string ffmpeg = FfmpegAudioTools::findFfmpeg();
  if (ffmpeg.empty() || !fileExistsUtf8(ffmpeg)) {
    lastError_ = "ffmpeg not found";
    return false;
  }

  const std::string listing = probeFfmpegEncoders(ffmpeg);
  if (listing.empty()) {
    lastError_ = "ffmpeg encoder list empty";
    return false;
  }

  // Prefer hardware from detection, but skip codecs that fail a 1-frame probe
  // (common: NVENC listed but busy / unsupported preset → mid-export write fail).
  for (const auto& codec : HardwareInfo::instance().h264EncoderCandidates()) {
    if (!encoderListed(listing, codec))
      continue;
    if (!probeCodec(ffmpeg, codec, fps, width, height, bitrateKbps))
      continue;
    if (tryStartFfmpeg(ffmpeg, codec, pathUtf8, fps, width, height, bitrateKbps)) {
      backendLabel_ = "ffmpeg/" + codec;
      lastError_.clear();
      return true;
    }
  }
  return false;
}

bool VideoEncoder::openOpenCv(const std::string& pathUtf8, double fps, int width,
                              int height, int bitrateKbps) {
  const cv::Size size(width, height);
  const std::vector<int> params = {
      cv::VIDEOWRITER_PROP_HW_ACCELERATION,
      cv::VIDEO_ACCELERATION_ANY,
      cv::VIDEOWRITER_PROP_QUALITY,
      75,
  };

  auto tryOpen = [&](int api, int fourcc, const char* tag) -> bool {
    writer_.release();
    if (api > 0) {
      if (!writer_.open(pathUtf8, api, fourcc, fps, size, params))
        return false;
    } else {
      if (!writer_.open(pathUtf8, fourcc, fps, size, params))
        return false;
    }
    if (!writer_.isOpened())
      return false;
    writer_.set(cv::VIDEOWRITER_PROP_QUALITY, 75);
    writer_.set(cv::CAP_PROP_BITRATE, static_cast<double>(bitrateKbps));
    backendLabel_ = tag;
    return true;
  };

  if (tryOpen(cv::CAP_MSMF, cv::VideoWriter::fourcc('H', '2', '6', '4'),
              "opencv/MSMF-H264"))
    return true;
  if (tryOpen(cv::CAP_FFMPEG, cv::VideoWriter::fourcc('a', 'v', 'c', '1'),
              "opencv/FFMPEG-avc1"))
    return true;
  if (tryOpen(cv::CAP_FFMPEG, cv::VideoWriter::fourcc('H', '2', '6', '4'),
              "opencv/FFMPEG-H264"))
    return true;
  return tryOpen(0, cv::VideoWriter::fourcc('m', 'p', '4', 'v'), "opencv/mp4v");
}

bool VideoEncoder::open(const std::string& pathUtf8, double fps, int width, int height,
                        double sourceBitrateKbps) {
  close();
  lastError_.clear();
  if (fps <= 1e-3)
    fps = 25.0;
  // yuv420p / most HW encoders require even dimensions.
  width = evenDim(width);
  height = evenDim(height);
  if (width < 2 || height < 2)
    return false;

  width_ = width;
  height_ = height;
  bitrateKbps_ = targetBitrateKbps(width, height, fps, sourceBitrateKbps);
  useFfmpeg_ = false;

  if (openFfmpegPipe(pathUtf8, fps, width, height, bitrateKbps_)) {
    useFfmpeg_ = true;
    return true;
  }
  ffmpeg_.kill();
  ffmpeg_.waitFinished(1000);
  ffmpeg_.close();
  if (openOpenCv(pathUtf8, fps, width, height, bitrateKbps_))
    return true;
  if (lastError_.empty())
    lastError_ = "no working video encoder";
  return false;
}

void VideoEncoder::close() {
  if (useFfmpeg_) {
    ffmpeg_.closeStdin();
    if (ffmpeg_.waitFinished(120000) < 0) {
      ffmpeg_.kill();
      ffmpeg_.waitFinished(2000);
    }
    ffmpeg_.close();
    useFfmpeg_ = false;
  }
  if (writer_.isOpened())
    writer_.release();
}

bool VideoEncoder::isOpened() const {
  if (useFfmpeg_)
    return ffmpeg_.running();
  return writer_.isOpened();
}

bool VideoEncoder::write(const cv::Mat& bgr) {
  if (bgr.empty())
    return false;

  cv::Mat frame = bgr;
  if (bgr.cols != width_ || bgr.rows != height_) {
    cv::resize(bgr, writeScratch_, cv::Size(width_, height_));
    frame = writeScratch_;
  }

  if (useFfmpeg_) {
    if (!ffmpeg_.running()) {
      lastError_ = "encoder exited: " + ffmpeg_.readAllOutput();
      if (lastError_ == "encoder exited: ")
        lastError_ = "encoder process exited during write (" + backendLabel_ + ")";
      return false;
    }
    const cv::Mat* src = &frame;
    if (!frame.isContinuous() || frame.type() != CV_8UC3) {
      if (frame.type() == CV_8UC3)
        writeScratch_ = frame.clone();
      else
        return false;
      src = &writeScratch_;
    }
    const size_t nbytes = src->total() * src->elemSize();
    if (!ffmpeg_.writeStdin(src->data, nbytes)) {
      lastError_ = "pipe write failed (" + backendLabel_ + ")";
      const std::string err = ffmpeg_.readAllOutput();
      if (!err.empty())
        lastError_ += ": " + err;
      return false;
    }
    return true;
  }

  if (!writer_.isOpened())
    return false;
  writer_.write(frame);
  return true;
}

}  // namespace vb
