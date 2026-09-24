#include "vb/FaceDetectorDnn.h"

#include "vb/fs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <utility>
#include <opencv2/core/utility.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace vb {

namespace {

cv::Mat ortValueToMat2D(Ort::Value& value) {
  auto info = value.GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  float* data = value.GetTensorMutableData<float>();
  if (shape.size() == 3) {
    const int n = static_cast<int>(shape[1]);
    const int c = static_cast<int>(shape[2]);
    return cv::Mat(n, c, CV_32FC1, data);
  }
  if (shape.size() == 2) {
    return cv::Mat(static_cast<int>(shape[0]), static_cast<int>(shape[1]),
                   CV_32FC1, data);
  }
  size_t count = 1;
  for (auto d : shape)
    count *= static_cast<size_t>(d);
  return cv::Mat(1, static_cast<int>(count), CV_32FC1, data);
}

cv::Mat shiftFaces(const cv::Mat& faces, float dx, float dy) {
  cv::Mat out = faces.clone();
  for (int i = 0; i < out.rows; ++i) {
    out.at<float>(i, 0) += dx;
    out.at<float>(i, 1) += dy;
  }
  return out;
}

void appendFaces(cv::Mat& dst, const cv::Mat& src) {
  for (int i = 0; i < src.rows; ++i)
    dst.push_back(src.row(i));
}

}  // namespace

bool FaceDetectorDnn::load(const std::string& onnxPath, bool tryDirectMl) {
  ready_ = false;
  if (!fileExistsUtf8(onnxPath))
    return false;
  if (!session_.load(onnxPath, tryDirectMl))
    return false;
  detectKind();
  ready_ = true;
  return true;
}

void FaceDetectorDnn::detectKind() {
  const int h = session_.inputHeight();
  const int w = session_.inputWidth();
  if (h > 0)
    inputH_ = h;
  if (w > 0)
    inputW_ = w;
  if (session_.outputNames().size() >= 12) {
    kind_ = Kind::YuNet;
    inputW_ = kYunInput_;
    inputH_ = kYunInput_;
    return;
  }

  const auto& inNames = session_.inputNames();
  const bool looksYolo =
      (!inNames.empty() && inNames[0] == "images") ||
      (session_.outputNames().size() == 1);
  if (looksYolo) {
    kind_ = Kind::YoloFace;
    inputW_ = kYoloInput_;
    inputH_ = kYoloInput_;
    return;
  }

  kind_ = Kind::IntelSsd;
  if (inputW_ <= 0 || inputH_ <= 0) {
    inputW_ = 672;
    inputH_ = 384;
  }
}

const char* FaceDetectorDnn::modelLabel() const {
  switch (kind_) {
  case Kind::YoloFace:
    return "YOLO11-face";
  case Kind::YuNet:
    return "YuNet";
  case Kind::IntelSsd:
  default:
    return "ADAS";
  }
}

cv::Mat FaceDetectorDnn::inferRaw(const cv::Mat& bgr, bool allowUpscale) {
  if (kind_ == Kind::IntelSsd)
    return inferRawIntel(bgr, allowUpscale);
  if (kind_ == Kind::YoloFace)
    return inferRawYoloFace(bgr, allowUpscale);
  return inferRawYun(bgr, allowUpscale);
}

cv::Mat FaceDetectorDnn::inferRawIntel(const cv::Mat& bgr, bool allowUpscale) {
  cv::Mat empty;
  if (!ready_ || bgr.empty())
    return empty;

  const int srcW = bgr.cols;
  const int srcH = bgr.rows;
  const float scale = std::min(static_cast<float>(inputW_) / static_cast<float>(srcW),
                               static_cast<float>(inputH_) / static_cast<float>(srcH));
  const float useScale = (!allowUpscale && scale > 1.f) ? 1.f : scale;
  const int newW = std::max(1, static_cast<int>(std::round(srcW * useScale)));
  const int newH = std::max(1, static_cast<int>(std::round(srcH * useScale)));
  const int copyW = std::min(newW, inputW_);
  const int copyH = std::min(newH, inputH_);
  const int padX = (inputW_ - copyW) / 2;
  const int padY = (inputH_ - copyH) / 2;

  cv::Mat resized;
  if (copyW != srcW || copyH != srcH)
    cv::resize(bgr, resized, cv::Size(copyW, copyH), 0, 0, cv::INTER_LINEAR);
  else
    resized = bgr;

  inferPad_.create(inputH_, inputW_, CV_8UC3);
  inferPad_.setTo(cv::Scalar(0, 0, 0));
  resized.copyTo(inferPad_(cv::Rect(padX, padY, copyW, copyH)));

  cv::dnn::blobFromImage(inferPad_, inferBlob_, 1.0, cv::Size(), cv::Scalar(), false,
                         false, CV_32F);
  if (!inferBlob_.isContinuous())
    inferBlob_ = inferBlob_.clone();

  const std::array<int64_t, 4> inputShape = {1, 3, inputH_, inputW_};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputVal = Ort::Value::CreateTensor<float>(
      mem, inferBlob_.ptr<float>(), static_cast<size_t>(inferBlob_.total()),
      inputShape.data(), inputShape.size());
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(inputVal));
  std::vector<Ort::Value> outputs;
  if (!session_.run(inputs, outputs) || outputs.empty())
    return empty;

  cv::Mat faces = outputs.size() >= 3 ? decodeIntelDets(outputs)
                                      : decodeLegacyDets(outputs[0]);
  return mapIntelBoxes(faces, useScale, padX, padY);
}

cv::Mat FaceDetectorDnn::decodeLegacyDets(Ort::Value& value) const {
  cv::Mat empty;
  auto info = value.GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  const float* data = value.GetTensorData<float>();
  int rows = 0;
  int stride = 7;
  if (shape.size() == 4) {
    rows = static_cast<int>(shape[2]);
    stride = static_cast<int>(shape[3]);
  } else if (shape.size() == 2) {
    rows = static_cast<int>(shape[0]);
    stride = static_cast<int>(shape[1]);
  } else if (shape.size() == 3) {
    rows = static_cast<int>(shape[1]);
    stride = static_cast<int>(shape[2]);
  }
  if (rows <= 0 || stride < 7)
    return empty;

  cv::Mat faces;
  for (int i = 0; i < rows; ++i) {
    const float* row = data + static_cast<size_t>(i) * static_cast<size_t>(stride);
    if (row[0] < 0.f)
      continue;
    cv::Mat face(1, 5, CV_32FC1);
    face.at<float>(0, 0) = row[3];
    face.at<float>(0, 1) = row[4];
    face.at<float>(0, 2) = row[5] - row[3];
    face.at<float>(0, 3) = row[6] - row[4];
    face.at<float>(0, 4) = row[2];
    faces.push_back(face);
  }
  return faces;
}

cv::Mat FaceDetectorDnn::decodeIntelDets(const std::vector<Ort::Value>& outputs) const {
  cv::Mat empty;
  const float* loc = nullptr;
  const float* conf = nullptr;
  const float* prior = nullptr;
  int locN = 0;
  int confN = 0;
  int priorN = 0;
  for (const auto& out : outputs) {
    const auto shape = out.GetTensorTypeAndShapeInfo().GetShape();
    const int n = static_cast<int>(out.GetTensorTypeAndShapeInfo().GetElementCount());
    const float* data = out.GetTensorData<float>();
    if (shape.size() == 3 && shape[1] == 2) {
      prior = data;
      priorN = n;
    } else if (loc == nullptr || n > locN) {
      if (conf == nullptr && loc != nullptr) {
        conf = loc;
        confN = locN;
      }
      loc = data;
      locN = n;
    } else {
      conf = data;
      confN = n;
    }
  }
  if (!loc || !conf || !prior || locN < 4 || confN < 2 || priorN < 8)
    return empty;
  if (confN > locN) {
    std::swap(conf, loc);
    std::swap(confN, locN);
  }
  const int numPriors = locN / 4;
  if (numPriors <= 0 || confN < numPriors * 2 || priorN < numPriors * 8)
    return empty;
  const int numClasses = confN / numPriors;
  const int faceCls = numClasses > 1 ? 1 : 0;
  const int priorStride = priorN / 2;

  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  boxes.reserve(256);
  scores.reserve(256);
  const float minScore = std::min(0.12f, scoreThreshold_);
  for (int i = 0; i < numPriors; ++i) {
    const float score = conf[i * numClasses + faceCls];
    if (score < minScore)
      continue;
    const float xmin0 = prior[i * 4 + 0];
    const float ymin0 = prior[i * 4 + 1];
    const float xmax0 = prior[i * 4 + 2];
    const float ymax0 = prior[i * 4 + 3];
    const float var0 = prior[priorStride + i * 4 + 0];
    const float var1 = prior[priorStride + i * 4 + 1];
    const float var2 = prior[priorStride + i * 4 + 2];
    const float var3 = prior[priorStride + i * 4 + 3];
    const float pw = xmax0 - xmin0;
    const float ph = ymax0 - ymin0;
    const float pcx = (xmin0 + xmax0) * 0.5f;
    const float pcy = (ymin0 + ymax0) * 0.5f;
    const float dx = loc[i * 4 + 0];
    const float dy = loc[i * 4 + 1];
    const float dw = loc[i * 4 + 2];
    const float dh = loc[i * 4 + 3];
    const float cx = var0 * dx * pw + pcx;
    const float cy = var1 * dy * ph + pcy;
    const float w = std::exp(var2 * dw) * pw;
    const float h = std::exp(var3 * dh) * ph;
    boxes.emplace_back(cx - w * 0.5f, cy - h * 0.5f, w, h);
    scores.push_back(score);
  }

  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, minScore, 0.45f, keep, 1.f, 200);

  cv::Mat faces;
  for (int idx : keep) {
    const auto& b = boxes[static_cast<size_t>(idx)];
    cv::Mat face(1, 5, CV_32FC1);
    face.at<float>(0, 0) = static_cast<float>(b.x);
    face.at<float>(0, 1) = static_cast<float>(b.y);
    face.at<float>(0, 2) = static_cast<float>(b.width);
    face.at<float>(0, 3) = static_cast<float>(b.height);
    face.at<float>(0, 4) = scores[static_cast<size_t>(idx)];
    faces.push_back(face);
  }
  return faces;
}

cv::Mat FaceDetectorDnn::mapIntelBoxes(const cv::Mat& dets, float useScale, int padX,
                                       int padY) const {
  cv::Mat faces;
  if (dets.empty())
    return faces;
  const float invScale = useScale > 1e-6f ? 1.f / useScale : 1.f;
  for (int i = 0; i < dets.rows; ++i) {
    const float xminN = dets.at<float>(i, 0);
    const float yminN = dets.at<float>(i, 1);
    const float wN = dets.at<float>(i, 2);
    const float hN = dets.at<float>(i, 3);
    const float conf = dets.at<float>(i, 4);
    const float xmin = xminN * static_cast<float>(inputW_);
    const float ymin = yminN * static_cast<float>(inputH_);
    const float w = wN * static_cast<float>(inputW_) * invScale;
    const float h = hN * static_cast<float>(inputH_) * invScale;
    float thresh = scoreThreshold_;
    if (std::min(w, h) < 48.f)
      thresh = std::max(0.22f, scoreThreshold_ * 0.62f);
    if (conf < thresh)
      continue;
    cv::Mat face(1, 5, CV_32FC1);
    face.at<float>(0, 0) = (xmin - static_cast<float>(padX)) * invScale;
    face.at<float>(0, 1) = (ymin - static_cast<float>(padY)) * invScale;
    face.at<float>(0, 2) = w;
    face.at<float>(0, 3) = h;
    face.at<float>(0, 4) = conf;
    faces.push_back(face);
  }
  return faces;
}

cv::Mat FaceDetectorDnn::inferRawYoloFace(const cv::Mat& bgr, bool allowUpscale) {
  cv::Mat empty;
  if (!ready_ || bgr.empty())
    return empty;

  const int origW = bgr.cols;
  const int origH = bgr.rows;
  const int inSize = inputW_ > 0 ? inputW_ : kYoloInput_;

  float r = std::min(static_cast<float>(inSize) / static_cast<float>(origW),
                     static_cast<float>(inSize) / static_cast<float>(origH));
  if (!allowUpscale && r > 1.f)
    r = 1.f;
  const int newW = std::max(1, static_cast<int>(std::round(origW * r)));
  const int newH = std::max(1, static_cast<int>(std::round(origH * r)));
  const int padW = (inSize - newW) / 2;
  const int padH = (inSize - newH) / 2;

  cv::Mat resized;
  if (newW != origW || newH != origH)
    cv::resize(bgr, resized, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
  else
    resized = bgr;

  letterbox_.create(inSize, inSize, CV_8UC3);
  letterbox_.setTo(cv::Scalar(114, 114, 114));
  resized.copyTo(letterbox_(cv::Rect(padW, padH, newW, newH)));

  // Ultralytics export: RGB, /255
  cv::dnn::blobFromImage(letterbox_, inferBlob_, 1.0 / 255.0, cv::Size(),
                         cv::Scalar(), true, false, CV_32F);
  if (!inferBlob_.isContinuous())
    inferBlob_ = inferBlob_.clone();

  const std::array<int64_t, 4> inputShape = {1, 3, inSize, inSize};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputVal = Ort::Value::CreateTensor<float>(
      mem, inferBlob_.ptr<float>(), static_cast<size_t>(inferBlob_.total()),
      inputShape.data(), inputShape.size());

  const auto& inNames = session_.inputNames();
  const auto& outNames = session_.outputNames();
  const char* inName = !inNames.empty() ? inNames[0].c_str() : "images";
  const char* outName = !outNames.empty() ? outNames[0].c_str() : "output0";

  std::vector<const char*> inNamePtrs = {inName};
  std::vector<const char*> outNamePtrs = {outName};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(inputVal));
  std::vector<Ort::Value> outputs;
  if (!session_.run(inNamePtrs, inputs, outNamePtrs, outputs) || outputs.empty())
    return empty;

  auto info = outputs[0].GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  float* data = outputs[0].GetTensorMutableData<float>();

  // Expect [1, 5, N] or [1, N, 5] — cx,cy,w,h,score for single-class face.
  cv::Mat output;
  cv::Mat transposed;
  if (shape.size() == 3 && shape[1] == 5) {
    output = cv::Mat(5, static_cast<int>(shape[2]), CV_32FC1, data);
  } else if (shape.size() == 3 && shape[2] == 5) {
    cv::Mat tmp(static_cast<int>(shape[1]), 5, CV_32FC1, data);
    transposed = tmp.t();
    output = transposed;
  } else if (shape.size() == 2 && shape[0] == 5) {
    output = cv::Mat(5, static_cast<int>(shape[1]), CV_32FC1, data);
  } else {
    return empty;
  }

  cv::Mat faces;
  const float minScore = std::min(0.22f, scoreThreshold_);
  for (int i = 0; i < output.cols; ++i) {
    const float score = output.at<float>(4, i);
    if (score < minScore)
      continue;
    const float cx = output.at<float>(0, i);
    const float cy = output.at<float>(1, i);
    const float w = output.at<float>(2, i);
    const float h = output.at<float>(3, i);

    float x1 = (cx - w * 0.5f - static_cast<float>(padW)) / r;
    float y1 = (cy - h * 0.5f - static_cast<float>(padH)) / r;
    float bw = w / r;
    float bh = h / r;

    float thresh = scoreThreshold_;
    if (std::min(bw, bh) < 48.f)
      thresh = std::max(0.22f, scoreThreshold_ * 0.55f);
    if (score < thresh)
      continue;

    cv::Mat face(1, 5, CV_32FC1);
    face.at<float>(0, 0) = x1;
    face.at<float>(0, 1) = y1;
    face.at<float>(0, 2) = bw;
    face.at<float>(0, 3) = bh;
    face.at<float>(0, 4) = score;
    faces.push_back(face);
  }
  return faces;
}

bool FaceDetectorDnn::inferYoloFaceBatch(const std::vector<cv::Mat>& crops,
                                         const std::vector<int>& shiftX,
                                         const std::vector<int>& shiftY,
                                         const std::vector<char>& torso,
                                         cv::Mat& facesOut,
                                         std::vector<cv::Mat>* perCrop,
                                         bool disableOnFail) {
  facesOut.release();
  if (!yoloBatchOk_ || crops.empty() || crops.size() != shiftX.size())
    return false;

  const int inSize = inputW_ > 0 ? inputW_ : kYoloInput_;
  const int batch = static_cast<int>(crops.size());
  const size_t plane = static_cast<size_t>(3) * inSize * inSize;
  batchBlob_.create(1, static_cast<int>(plane * static_cast<size_t>(batch)), CV_32F);
  float* blob = batchBlob_.ptr<float>();

  struct Meta {
    float r = 1.f;
    int padW = 0;
    int padH = 0;
  };
  std::vector<Meta> meta(static_cast<size_t>(batch));

  if (perCrop)
    perCrop->assign(static_cast<size_t>(batch), cv::Mat());

  cv::parallel_for_(cv::Range(0, batch), [&](const cv::Range& range) {
    for (int b = range.start; b < range.end; ++b) {
      const cv::Mat& src = crops[static_cast<size_t>(b)];
      Meta& m = meta[static_cast<size_t>(b)];
      float* dst = blob + plane * static_cast<size_t>(b);
      const float fill = 114.f / 255.f;
      std::fill(dst, dst + plane, fill);
      if (src.empty())
        continue;
      m.r = std::min(static_cast<float>(inSize) / static_cast<float>(src.cols),
                     static_cast<float>(inSize) / static_cast<float>(src.rows));
      const int newW = std::max(1, static_cast<int>(std::round(src.cols * m.r)));
      const int newH = std::max(1, static_cast<int>(std::round(src.rows * m.r)));
      m.padW = (inSize - newW) / 2;
      m.padH = (inSize - newH) / 2;
      cv::Mat resized;
      if (newW != src.cols || newH != src.rows)
        cv::resize(src, resized, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
      else
        resized = src;
      const int area = inSize * inSize;
      for (int y = 0; y < resized.rows; ++y) {
        const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
        const int oy = y + m.padH;
        for (int x = 0; x < resized.cols; ++x) {
          const int ox = x + m.padW;
          const int pix = oy * inSize + ox;
          dst[pix] = row[x][2] / 255.f;
          dst[area + pix] = row[x][1] / 255.f;
          dst[2 * area + pix] = row[x][0] / 255.f;
        }
      }
    }
  });

  const std::array<int64_t, 4> inputShape = {batch, 3, inSize, inSize};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputVal = Ort::Value::CreateTensor<float>(
      mem, blob, plane * static_cast<size_t>(batch), inputShape.data(),
      inputShape.size());
  const auto& inNames = session_.inputNames();
  const auto& outNames = session_.outputNames();
  const char* inName = !inNames.empty() ? inNames[0].c_str() : "images";
  const char* outName = !outNames.empty() ? outNames[0].c_str() : "output0";
  std::vector<const char*> inNamePtrs = {inName};
  std::vector<const char*> outNamePtrs = {outName};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(inputVal));
  std::vector<Ort::Value> outputs;
  if (!session_.run(inNamePtrs, inputs, outNamePtrs, outputs, /*fallbackOnFail=*/false) ||
      outputs.empty()) {
    if (disableOnFail)
      yoloBatchOk_ = false;
    return false;
  }

  auto info = outputs[0].GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  const float* data = outputs[0].GetTensorData<float>();
  if (shape.size() != 3 || shape[0] != batch)
    return false;

  const bool chFirst = shape[1] == 5;
  const bool chLast = shape[2] == 5;
  if (!chFirst && !chLast)
    return false;
  const int anchors = static_cast<int>(chFirst ? shape[2] : shape[1]);
  const float minScore = std::min(0.22f, scoreThreshold_);

  for (int b = 0; b < batch; ++b) {
    const Meta& m = meta[static_cast<size_t>(b)];
    const float inv = m.r > 1e-6f ? 1.f / m.r : 1.f;
    const bool dropTorso = torso[static_cast<size_t>(b)] != 0;
    const cv::Mat& src = crops[static_cast<size_t>(b)];
    const float roiArea = static_cast<float>(std::max(1, src.cols * src.rows));
    for (int i = 0; i < anchors; ++i) {
      float cx, cy, w, h, score;
      if (chFirst) {
        const float* base = data + static_cast<size_t>(b) * 5 * static_cast<size_t>(anchors);
        cx = base[i];
        cy = base[static_cast<size_t>(anchors) + i];
        w = base[2 * static_cast<size_t>(anchors) + i];
        h = base[3 * static_cast<size_t>(anchors) + i];
        score = base[4 * static_cast<size_t>(anchors) + i];
      } else {
        const float* row =
            data + (static_cast<size_t>(b) * static_cast<size_t>(anchors) +
                    static_cast<size_t>(i)) *
                       5;
        cx = row[0];
        cy = row[1];
        w = row[2];
        h = row[3];
        score = row[4];
      }
      if (score < minScore)
        continue;
      const float bw = w * inv;
      const float bh = h * inv;
      if (dropTorso && bw * bh > roiArea * 0.50f)
        continue;
      float thresh = scoreThreshold_;
      if (std::min(bw, bh) < 48.f)
        thresh = std::max(0.22f, scoreThreshold_ * 0.55f);
      if (score < thresh)
        continue;
      cv::Mat face(1, 5, CV_32FC1);
      face.at<float>(0, 0) =
          (cx - w * 0.5f - static_cast<float>(m.padW)) * inv +
          static_cast<float>(shiftX[static_cast<size_t>(b)]);
      face.at<float>(0, 1) =
          (cy - h * 0.5f - static_cast<float>(m.padH)) * inv +
          static_cast<float>(shiftY[static_cast<size_t>(b)]);
      face.at<float>(0, 2) = bw;
      face.at<float>(0, 3) = bh;
      face.at<float>(0, 4) = score;
      if (perCrop)
        (*perCrop)[static_cast<size_t>(b)].push_back(face);
      else
        facesOut.push_back(face);
    }
  }
  return true;
}

std::vector<std::vector<FaceDet>> FaceDetectorDnn::detectGrouped(
    const std::vector<cv::Mat>& frames,
    const std::vector<std::vector<cv::Rect>>& zoomsPerFrame) {
  std::vector<std::vector<FaceDet>> out(frames.size());
  if (!ready_ || frames.empty())
    return out;

  auto each = [&]() {
    for (size_t i = 0; i < frames.size(); ++i) {
      const std::vector<cv::Rect> empty;
      const auto& zooms =
          i < zoomsPerFrame.size() ? zoomsPerFrame[i] : empty;
      out[i] = detect(frames[i], zooms);
    }
  };
  if (kind_ != Kind::YoloFace || !yoloBatchOk_ || frames.size() < 2) {
    each();
    return out;
  }

  struct CropItem {
    cv::Mat mat;
    int shiftX = 0;
    int shiftY = 0;
    char torso = 0;
    int owner = 0;
  };
  std::vector<CropItem> fulls;
  std::vector<CropItem> zooms;
  fulls.reserve(frames.size());
  zooms.reserve(frames.size() * 6);

  for (size_t fi = 0; fi < frames.size(); ++fi) {
    const cv::Mat& bgr = frames[fi];
    if (bgr.empty())
      continue;
    const cv::Rect bounds(0, 0, bgr.cols, bgr.rows);
    fulls.push_back({bgr, 0, 0, 0, static_cast<int>(fi)});
    if (fi >= zoomsPerFrame.size())
      continue;
    for (const cv::Rect& zr : zoomsPerFrame[fi]) {
      cv::Rect roi = zr & bounds;
      if (roi.width < 12 || roi.height < 12)
        continue;
      if (roi.width > inputW_) {
        roi.x += (roi.width - inputW_) / 2;
        roi.width = inputW_;
      }
      if (roi.height > inputH_)
        roi.height = inputH_;
      roi &= bounds;
      if (roi.width < 12 || roi.height < 12)
        continue;
      const bool torsoCrop =
          roi.height > static_cast<int>(roi.width * 1.28f) && roi.height >= 80;
      zooms.push_back({bgr(roi), roi.x, roi.y, torsoCrop ? 1 : 0,
                       static_cast<int>(fi)});
    }
  }

  auto runGroup = [&](const std::vector<CropItem>& items,
                      std::vector<cv::Mat>& perCropOut) -> bool {
    if (items.empty())
      return true;
    std::vector<cv::Mat> crops;
    std::vector<int> sx, sy;
    std::vector<char> torso;
    crops.reserve(items.size());
    for (const auto& it : items) {
      crops.push_back(it.mat);
      sx.push_back(it.shiftX);
      sy.push_back(it.shiftY);
      torso.push_back(it.torso);
    }
    constexpr int kSlice = 48;
    const int n = static_cast<int>(crops.size());
    perCropOut.assign(static_cast<size_t>(n), cv::Mat());

    auto packSlice = [&](int s, int cnt, cv::Mat& blob, std::vector<float>& r,
                         std::vector<int>& padW, std::vector<int>& padH) {
      const int inSize = inputW_ > 0 ? inputW_ : kYoloInput_;
      const size_t plane =
          static_cast<size_t>(3) * inSize * inSize;
      blob.create(1, static_cast<int>(plane * static_cast<size_t>(cnt)), CV_32F);
      float* dstBase = blob.ptr<float>();
      r.assign(static_cast<size_t>(cnt), 1.f);
      padW.assign(static_cast<size_t>(cnt), 0);
      padH.assign(static_cast<size_t>(cnt), 0);
      cv::parallel_for_(cv::Range(0, cnt), [&](const cv::Range& range) {
        for (int i = range.start; i < range.end; ++i) {
          const cv::Mat& src = crops[static_cast<size_t>(s + i)];
          float* dst = dstBase + plane * static_cast<size_t>(i);
          const float fill = 114.f / 255.f;
          std::fill(dst, dst + plane, fill);
          if (src.empty())
            continue;
          r[static_cast<size_t>(i)] =
              std::min(static_cast<float>(inSize) / static_cast<float>(src.cols),
                       static_cast<float>(inSize) / static_cast<float>(src.rows));
          const int newW =
              std::max(1, static_cast<int>(std::round(src.cols * r[static_cast<size_t>(i)])));
          const int newH =
              std::max(1, static_cast<int>(std::round(src.rows * r[static_cast<size_t>(i)])));
          padW[static_cast<size_t>(i)] = (inSize - newW) / 2;
          padH[static_cast<size_t>(i)] = (inSize - newH) / 2;
          cv::Mat resized;
          if (newW != src.cols || newH != src.rows)
            cv::resize(src, resized, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
          else
            resized = src;
          const int area = inSize * inSize;
          for (int y = 0; y < resized.rows; ++y) {
            const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
            const int oy = y + padH[static_cast<size_t>(i)];
            for (int x = 0; x < resized.cols; ++x) {
              const int ox = x + padW[static_cast<size_t>(i)];
              const int pix = oy * inSize + ox;
              dst[pix] = row[x][2] / 255.f;
              dst[area + pix] = row[x][1] / 255.f;
              dst[2 * area + pix] = row[x][0] / 255.f;
            }
          }
        }
      });
    };

    auto runPacked = [&](int s, int cnt, cv::Mat& blob, const std::vector<float>& r,
                         const std::vector<int>& padW, const std::vector<int>& padH)
        -> bool {
      const int inSize = inputW_ > 0 ? inputW_ : kYoloInput_;
      const size_t plane =
          static_cast<size_t>(3) * inSize * inSize;
      const std::array<int64_t, 4> inputShape = {cnt, 3, inSize, inSize};
      Ort::MemoryInfo mem =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value inputVal = Ort::Value::CreateTensor<float>(
          mem, blob.ptr<float>(), plane * static_cast<size_t>(cnt),
          inputShape.data(), inputShape.size());
      const auto& inNames = session_.inputNames();
      const auto& outNames = session_.outputNames();
      const char* inName = !inNames.empty() ? inNames[0].c_str() : "images";
      const char* outName = !outNames.empty() ? outNames[0].c_str() : "output0";
      std::vector<const char*> inNamePtrs = {inName};
      std::vector<const char*> outNamePtrs = {outName};
      std::vector<Ort::Value> inputs;
      inputs.push_back(std::move(inputVal));
      std::vector<Ort::Value> outputs;
      if (!session_.run(inNamePtrs, inputs, outNamePtrs, outputs,
                        /*fallbackOnFail=*/false) ||
          outputs.empty())
        return false;
      auto info = outputs[0].GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      const float* data = outputs[0].GetTensorData<float>();
      if (shape.size() != 3 || shape[0] != cnt || !data)
        return false;
      const bool chFirst = shape[1] == 5;
      const bool chLast = shape[2] == 5;
      if (!chFirst && !chLast)
        return false;
      const int anchors = static_cast<int>(chFirst ? shape[2] : shape[1]);
      const float minScore = std::min(0.22f, scoreThreshold_);
      for (int b = 0; b < cnt; ++b) {
        const float inv = r[static_cast<size_t>(b)] > 1e-6f
                              ? 1.f / r[static_cast<size_t>(b)]
                              : 1.f;
        const bool dropTorso = torso[static_cast<size_t>(s + b)] != 0;
        const cv::Mat& src = crops[static_cast<size_t>(s + b)];
        const float roiArea =
            static_cast<float>(std::max(1, src.cols * src.rows));
        for (int i = 0; i < anchors; ++i) {
          float cx, cy, w, h, score;
          if (chFirst) {
            const float* base =
                data + static_cast<size_t>(b) * 5 * static_cast<size_t>(anchors);
            cx = base[i];
            cy = base[static_cast<size_t>(anchors) + i];
            w = base[2 * static_cast<size_t>(anchors) + i];
            h = base[3 * static_cast<size_t>(anchors) + i];
            score = base[4 * static_cast<size_t>(anchors) + i];
          } else {
            const float* row =
                data + (static_cast<size_t>(b) * static_cast<size_t>(anchors) +
                        static_cast<size_t>(i)) *
                           5;
            cx = row[0];
            cy = row[1];
            w = row[2];
            h = row[3];
            score = row[4];
          }
          if (score < minScore)
            continue;
          const float bw = w * inv;
          const float bh = h * inv;
          if (dropTorso && bw * bh > roiArea * 0.50f)
            continue;
          float thresh = scoreThreshold_;
          if (std::min(bw, bh) < 48.f)
            thresh = std::max(0.22f, scoreThreshold_ * 0.55f);
          if (score < thresh)
            continue;
          cv::Mat face(1, 5, CV_32FC1);
          face.at<float>(0, 0) =
              (cx - w * 0.5f - static_cast<float>(padW[static_cast<size_t>(b)])) *
                  inv +
              static_cast<float>(sx[static_cast<size_t>(s + b)]);
          face.at<float>(0, 1) =
              (cy - h * 0.5f - static_cast<float>(padH[static_cast<size_t>(b)])) *
                  inv +
              static_cast<float>(sy[static_cast<size_t>(s + b)]);
          face.at<float>(0, 2) = bw;
          face.at<float>(0, 3) = bh;
          face.at<float>(0, 4) = score;
          perCropOut[static_cast<size_t>(s + b)].push_back(face);
        }
      }
      return true;
    };

    cv::Mat* blobCur = &batchBlob_;
    cv::Mat* blobNext = &batchBlobAlt_;
    std::vector<float> rCur, rNext;
    std::vector<int> padWCur, padHCur, padWNext, padHNext;
    const int c0 = std::min(kSlice, n);
    packSlice(0, c0, *blobCur, rCur, padWCur, padHCur);

    for (int s = 0; s < n; s += kSlice) {
      const int cnt = std::min(kSlice, n - s);
      const int sNext = s + kSlice;
      const int cNext = sNext < n ? std::min(kSlice, n - sNext) : 0;
      std::future<void> prep;
      if (cNext > 0) {
        prep = std::async(std::launch::async, [&] {
          packSlice(sNext, cNext, *blobNext, rNext, padWNext, padHNext);
        });
      }
      if (!runPacked(s, cnt, *blobCur, rCur, padWCur, padHCur)) {
        if (prep.valid())
          prep.wait();
        return false;
      }
      if (prep.valid())
        prep.wait();
      std::swap(blobCur, blobNext);
      rCur.swap(rNext);
      padWCur.swap(padWNext);
      padHCur.swap(padHNext);
    }
    return true;
  };

  std::vector<cv::Mat> perFull;
  std::vector<cv::Mat> perZoom;
  if (!runGroup(fulls, perFull) || !runGroup(zooms, perZoom)) {
    each();
    return out;
  }

  std::vector<cv::Mat> grouped(frames.size());
  for (size_t i = 0; i < fulls.size(); ++i) {
    if (perFull[i].empty())
      continue;
    appendFaces(grouped[static_cast<size_t>(fulls[i].owner)], perFull[i]);
  }
  for (size_t i = 0; i < zooms.size(); ++i) {
    if (perZoom[i].empty())
      continue;
    appendFaces(grouped[static_cast<size_t>(zooms[i].owner)], perZoom[i]);
  }
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].empty())
      continue;
    out[i] = finalizeRects(nmsFaces(grouped[i]), frames[i].cols, frames[i].rows);
  }
  return out;
}

cv::Mat FaceDetectorDnn::inferRawYun(const cv::Mat& bgr, bool allowUpscale) {
  cv::Mat empty;
  if (!ready_ || bgr.empty())
    return empty;

  const int srcW = bgr.cols;
  const int srcH = bgr.rows;
  const int inW = inputW_ > 0 ? inputW_ : kYunInput_;
  const int inH = inputH_ > 0 ? inputH_ : kYunInput_;
  float scale = std::min(static_cast<float>(inW) / static_cast<float>(srcW),
                         static_cast<float>(inH) / static_cast<float>(srcH));
  if (!allowUpscale && scale > 1.f)
    scale = 1.f;
  const int newW =
      std::max(1, std::min(inW, static_cast<int>(std::round(srcW * scale))));
  const int newH =
      std::max(1, std::min(inH, static_cast<int>(std::round(srcH * scale))));
  const float scaleX = static_cast<float>(srcW) / static_cast<float>(newW);
  const float scaleY = static_cast<float>(srcH) / static_cast<float>(newH);

  cv::Mat content;
  if (newW != srcW || newH != srcH)
    cv::resize(bgr, content, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
  else
    content = bgr;

  inferPad_.create(inH, inW, CV_8UC3);
  inferPad_.setTo(cv::Scalar(0, 0, 0));
  content.copyTo(inferPad_(cv::Rect(0, 0, content.cols, content.rows)));

  cv::dnn::blobFromImage(inferPad_, inferBlob_, 1.0, cv::Size(), cv::Scalar(),
                         false, false, CV_32F);
  if (!inferBlob_.isContinuous())
    inferBlob_ = inferBlob_.clone();

  const std::array<int64_t, 4> inputShape = {1, 3, inH, inW};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputVal = Ort::Value::CreateTensor<float>(
      mem, inferBlob_.ptr<float>(), static_cast<size_t>(inferBlob_.total()),
      inputShape.data(), inputShape.size());

  static const char* kInputName = "input";
  static const char* kOutputNames[] = {
      "cls_8",  "cls_16",  "cls_32",  "obj_8",  "obj_16",  "obj_32",
      "bbox_8", "bbox_16", "bbox_32", "kps_8",  "kps_16",  "kps_32"};

  std::vector<Ort::Value> outputs;
  std::vector<const char*> inNames = {kInputName};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(inputVal));
  std::vector<const char*> outNames(std::begin(kOutputNames),
                                    std::end(kOutputNames));

  if (!session_.run(inNames, inputs, outNames, outputs) || outputs.size() < 12)
    return empty;

  std::vector<cv::Mat> outputBlobs;
  outputBlobs.reserve(12);
  for (size_t i = 0; i < 12; ++i)
    outputBlobs.push_back(ortValueToMat2D(outputs[i]));

  cv::Mat faces = postProcessYun(outputBlobs, inW, inH, scaleX, scaleY);
  const cv::Rect frameBounds(0, 0, srcW, srcH);
  cv::Mat clipped;
  for (int i = 0; i < faces.rows; ++i) {
    float x = faces.at<float>(i, 0);
    float y = faces.at<float>(i, 1);
    float w = faces.at<float>(i, 2);
    float h = faces.at<float>(i, 3);
    cv::Rect r(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w),
               static_cast<int>(h));
    r &= frameBounds;
    if (r.width <= 2 || r.height <= 2)
      continue;
    cv::Mat row = faces.row(i).clone();
    row.at<float>(0, 0) = static_cast<float>(r.x);
    row.at<float>(0, 1) = static_cast<float>(r.y);
    row.at<float>(0, 2) = static_cast<float>(r.width);
    row.at<float>(0, 3) = static_cast<float>(r.height);
    clipped.push_back(row);
  }
  return clipped;
}

std::vector<cv::Rect> FaceDetectorDnn::nativeTiles(int srcW, int srcH) const {
  std::vector<cv::Rect> tiles;
  if (srcW <= 0 || srcH <= 0)
    return tiles;

  const int tw = std::min(inputW_, srcW);
  const int th = std::min(inputH_, srcH);
  const float overlap = tileOverlap_;
  const int stepX = std::max(1, static_cast<int>(std::round(tw * (1.f - overlap))));
  const int stepY = std::max(1, static_cast<int>(std::round(th * (1.f - overlap))));

  auto axis = [](int size, int tile, int step) {
    std::vector<int> pos;
    if (size <= tile) {
      pos.push_back(0);
      return pos;
    }
    for (int p = 0; p + tile < size; p += step)
      pos.push_back(p);
    const int last = size - tile;
    if (pos.empty() || pos.back() != last)
      pos.push_back(last);
    return pos;
  };

  const std::vector<int> xs = axis(srcW, tw, stepX);
  const std::vector<int> ys = axis(srcH, th, stepY);
  for (int y : ys) {
    for (int x : xs)
      tiles.emplace_back(x, y, tw, th);
  }
  return tiles;
}

std::vector<cv::Rect>
FaceDetectorDnn::limitedTiles(const std::vector<cv::Rect>& tiles) const {
  if (static_cast<int>(tiles.size()) <= maxTiles_)
    return tiles;
  std::vector<cv::Rect> out;
  out.reserve(static_cast<size_t>(maxTiles_));
  const int n = static_cast<int>(tiles.size());
  for (int i = 0; i < maxTiles_; ++i) {
    const int idx = (i * (n - 1)) / std::max(1, maxTiles_ - 1);
    out.push_back(tiles[static_cast<size_t>(idx)]);
  }
  return out;
}

std::vector<FaceDet>
FaceDetectorDnn::detect(const cv::Mat& bgrFrame,
                        const std::vector<cv::Rect>& zoomRois,
                        bool fullFrameTiles) {
  std::vector<FaceDet> out;
  if (!ready_ || bgrFrame.empty())
    return out;

  const int srcW = bgrFrame.cols;
  const int srcH = bgrFrame.rows;
  const cv::Rect frameBounds(0, 0, srcW, srcH);

  if (kind_ == Kind::YoloFace && yoloBatchOk_) {
    std::vector<cv::Mat> crops;
    std::vector<int> shiftX;
    std::vector<int> shiftY;
    std::vector<char> torso;
    crops.push_back(bgrFrame);
    shiftX.push_back(0);
    shiftY.push_back(0);
    torso.push_back(0);
    for (const cv::Rect& zr : zoomRois) {
      cv::Rect roi = zr & frameBounds;
      if (roi.width < 12 || roi.height < 12)
        continue;
      if (roi.width > inputW_) {
        roi.x += (roi.width - inputW_) / 2;
        roi.width = inputW_;
      }
      if (roi.height > inputH_)
        roi.height = inputH_;
      roi &= frameBounds;
      if (roi.width < 12 || roi.height < 12)
        continue;
      const bool torsoCrop =
          roi.height > static_cast<int>(roi.width * 1.28f) && roi.height >= 80;
      crops.push_back(bgrFrame(roi));
      shiftX.push_back(roi.x);
      shiftY.push_back(roi.y);
      torso.push_back(torsoCrop ? 1 : 0);
    }
    cv::Mat batched;
    if (inferYoloFaceBatch(crops, shiftX, shiftY, torso, batched))
      return finalizeRects(nmsFaces(batched), srcW, srcH);
  }

  cv::Mat all;
  // YOLO-face already letterboxes the full frame; skip dense tiling on large
  // frames (slow) and rely on person-head zoom ROIs for distant faces.
  const bool useTiles =
      fullFrameTiles && kind_ != Kind::YoloFace;
  if (srcW <= inputW_ && srcH <= inputH_) {
    all = inferRaw(bgrFrame, /*allowUpscale=*/true);
  } else {
    appendFaces(all, inferRaw(bgrFrame, /*allowUpscale=*/false));
    if (useTiles) {
      for (const cv::Rect& tile : limitedTiles(nativeTiles(srcW, srcH))) {
        const cv::Rect roi = tile & frameBounds;
        if (roi.width < 16 || roi.height < 16)
          continue;
        cv::Mat raw = inferRaw(bgrFrame(roi), /*allowUpscale=*/false);
        if (raw.empty())
          continue;
        appendFaces(all, shiftFaces(raw, static_cast<float>(roi.x),
                                    static_cast<float>(roi.y)));
      }
    }
  }

  for (const cv::Rect& zr : zoomRois) {
    cv::Rect roi = zr & frameBounds;
    if (roi.width < 12 || roi.height < 12)
      continue;
    if (roi.width > inputW_) {
      roi.x += (roi.width - inputW_) / 2;
      roi.width = inputW_;
    }
    if (roi.height > inputH_)
      roi.height = inputH_;
    roi &= frameBounds;
    if (roi.width < 12 || roi.height < 12)
      continue;
    cv::Mat raw = inferRaw(bgrFrame(roi), /*allowUpscale=*/true);
    if (raw.empty())
      continue;
    // A torso-sized crop that "detects" itself as one face is the overhead
    // whole-person box. Keep inner faces; drop crop-filling blobs.
    const bool torsoCrop =
        roi.height > static_cast<int>(roi.width * 1.28f) && roi.height >= 80;
    if (torsoCrop) {
      const float roiArea = static_cast<float>(std::max(1, roi.area()));
      cv::Mat kept;
      for (int i = 0; i < raw.rows; ++i) {
        const float w = raw.at<float>(i, 2);
        const float h = raw.at<float>(i, 3);
        if (w * h > roiArea * 0.50f)
          continue;
        kept.push_back(raw.row(i));
      }
      raw = kept;
      if (raw.empty())
        continue;
    }
    appendFaces(all, shiftFaces(raw, static_cast<float>(roi.x),
                                static_cast<float>(roi.y)));
  }

  all = nmsFaces(all);
  return finalizeRects(all, srcW, srcH);
}

cv::Mat FaceDetectorDnn::nmsFaces(const cv::Mat& faces) const {
  if (faces.rows <= 1)
    return faces;

  std::vector<cv::Rect> faceBoxes;
  std::vector<float> faceScores;
  faceBoxes.reserve(faces.rows);
  faceScores.reserve(faces.rows);
  for (int rIdx = 0; rIdx < faces.rows; ++rIdx) {
    faceBoxes.emplace_back(static_cast<int>(faces.at<float>(rIdx, 0)),
                           static_cast<int>(faces.at<float>(rIdx, 1)),
                           static_cast<int>(faces.at<float>(rIdx, 2)),
                           static_cast<int>(faces.at<float>(rIdx, 3)));
    faceScores.push_back(faces.at<float>(rIdx, 4));
  }

  std::vector<int> keepIdx;
  cv::dnn::NMSBoxes(faceBoxes, faceScores, 0.22f, nmsThreshold_, keepIdx, 1.f,
                    topK_);

  cv::Mat nms;
  for (int idx : keepIdx)
    nms.push_back(faces.row(idx));

  // Drop a large "face" that contains a smaller one — usually body-as-face.
  if (nms.rows <= 1)
    return nms;
  std::vector<char> drop(static_cast<size_t>(nms.rows), 0);
  for (int i = 0; i < nms.rows; ++i) {
    const float ix = nms.at<float>(i, 0);
    const float iy = nms.at<float>(i, 1);
    const float iw = nms.at<float>(i, 2);
    const float ih = nms.at<float>(i, 3);
    const float ia = std::max(1.f, iw * ih);
    for (int j = 0; j < nms.rows; ++j) {
      if (i == j)
        continue;
      const float jw = nms.at<float>(j, 2);
      const float jh = nms.at<float>(j, 3);
      const float ja = std::max(1.f, jw * jh);
      if (ia < ja * 2.2f)
        continue;
      const float jcx = nms.at<float>(j, 0) + jw * 0.5f;
      const float jcy = nms.at<float>(j, 1) + jh * 0.5f;
      if (jcx >= ix && jcx <= ix + iw && jcy >= iy && jcy <= iy + ih) {
        drop[static_cast<size_t>(i)] = 1;
        break;
      }
    }
  }
  cv::Mat filtered;
  for (int i = 0; i < nms.rows; ++i) {
    if (!drop[static_cast<size_t>(i)])
      filtered.push_back(nms.row(i));
  }
  return filtered.empty() ? nms : filtered;
}

std::vector<FaceDet> FaceDetectorDnn::finalizeRects(const cv::Mat& faces, int srcW,
                                                    int srcH) const {
  std::vector<FaceDet> out;
  out.reserve(faces.rows);
  const cv::Rect frameBounds(0, 0, srcW, srcH);
  for (int i = 0; i < faces.rows; ++i) {
    int x = static_cast<int>(faces.at<float>(i, 0));
    int y = static_cast<int>(faces.at<float>(i, 1));
    int w = static_cast<int>(faces.at<float>(i, 2));
    int h = static_cast<int>(faces.at<float>(i, 3));
    cv::Rect r(x, y, w, h);
    r &= frameBounds;
    if (r.width <= 2 || r.height <= 2)
      continue;
    const float ar =
        static_cast<float>(r.height) / static_cast<float>(std::max(1, r.width));
    if (ar > 1.85f || ar < 0.42f)
      continue;
    if (r.height > srcH * 42 / 100 && ar > 1.35f)
      continue;
    const float pad = (r.width < 64 || r.height < 64) ? 0.32f : 0.16f;
    const int dx = std::max(3, static_cast<int>(std::round(r.width * pad)));
    const int dy = std::max(4, static_cast<int>(std::round(r.height * pad)));
    const int extraY = std::max(2, dy / 3);
    r.x -= dx;
    r.y -= dy + extraY / 2;
    r.width += dx * 2;
    r.height += dy * 2 + extraY;
    r &= frameBounds;
    if (r.width > 2 && r.height > 2) {
      FaceDet d;
      d.rect = r;
      d.score = faces.at<float>(i, 4);
      out.push_back(d);
    }
  }
  return out;
}

cv::Mat FaceDetectorDnn::postProcessYun(const std::vector<cv::Mat>& outputBlobs,
                                        int padW, int padH, float scaleX,
                                        float scaleY) const {
  cv::Mat faces;
  if (outputBlobs.size() < 12)
    return faces;

  for (size_t i = 0; i < strides_.size(); ++i) {
    const int stride = strides_[i];
    const int cols = padW / stride;
    const int rows = padH / stride;

    const cv::Mat& cls = outputBlobs[i];
    const cv::Mat& obj = outputBlobs[i + strides_.size()];
    const cv::Mat& bbox = outputBlobs[i + strides_.size() * 2];

    const float* cls_v = reinterpret_cast<const float*>(cls.data);
    const float* obj_v = reinterpret_cast<const float*>(obj.data);
    const float* bbox_v = reinterpret_cast<const float*>(bbox.data);
    const int bboxStep = bbox.cols >= 4 ? bbox.cols : 4;

    cv::Mat face(1, 5, CV_32FC1);

    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < cols; ++c) {
        const size_t idx = static_cast<size_t>(r * cols + c);
        float cls_score = std::clamp(cls_v[idx], 0.f, 1.f);
        float obj_score = std::clamp(obj_v[idx], 0.f, 1.f);
        const float score = std::sqrt(cls_score * obj_score);
        if (score < 0.22f)
          continue;

        const float cx = (c + bbox_v[idx * bboxStep + 0]) * stride;
        const float cy = (r + bbox_v[idx * bboxStep + 1]) * stride;
        const float w = std::exp(bbox_v[idx * bboxStep + 2]) * stride;
        const float h = std::exp(bbox_v[idx * bboxStep + 3]) * stride;
        float thresh = scoreThreshold_;
        if (std::min(w, h) < 48.f)
          thresh = std::max(0.22f, scoreThreshold_ * 0.62f);
        if (score < thresh)
          continue;

        face.at<float>(0, 0) = (cx - w / 2.f) * scaleX;
        face.at<float>(0, 1) = (cy - h / 2.f) * scaleY;
        face.at<float>(0, 2) = w * scaleX;
        face.at<float>(0, 3) = h * scaleY;
        face.at<float>(0, 4) = score;
        faces.push_back(face);
      }
    }
  }

  return nmsFaces(faces);
}

}  // namespace vb
