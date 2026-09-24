#include "vb/YoloDetectorDnn.h"

#include "vb/fs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/core/utility.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace vb {

namespace {
const char* kCocoNames[80] = {
    "person",        "bicycle",      "car",           "motorcycle",    "airplane",
    "bus",           "train",        "truck",         "boat",          "traffic light",
    "fire hydrant",  "stop sign",    "parking meter", "bench",         "bird",
    "cat",           "dog",          "horse",         "sheep",         "cow",
    "elephant",      "bear",         "zebra",         "giraffe",       "backpack",
    "umbrella",      "handbag",      "tie",           "suitcase",      "frisbee",
    "skis",          "snowboard",    "sports ball",   "kite",          "baseball bat",
    "baseball glove","skateboard",   "surfboard",     "tennis racket", "bottle",
    "wine glass",    "cup",          "fork",          "knife",         "spoon",
    "bowl",          "banana",       "apple",         "sandwich",      "orange",
    "broccoli",      "carrot",       "hot dog",       "pizza",         "donut",
    "cake",          "chair",        "couch",         "potted plant",  "bed",
    "dining table",  "toilet",       "tv",            "laptop",        "mouse",
    "remote",        "keyboard",     "cell phone",    "microwave",     "oven",
    "toaster",       "sink",         "refrigerator",  "book",          "clock",
    "vase",          "scissors",     "teddy bear",    "hair drier",    "toothbrush"};
} // namespace

bool YoloDetectorDnn::isScreenClass(int classId) {
  return classId == 62 || classId == 63 || classId == 67 || classId == 66;
}

bool YoloDetectorDnn::isVehicleClass(int classId) {
  return classId == 2 || classId == 3 || classId == 5 || classId == 7;
}

bool YoloDetectorDnn::isPersonClass(int classId) { return classId == 0; }

cv::Rect YoloDetectorDnn::personHeadRoi(const cv::Rect& person, int frameW,
                                        int frameH) {
  const float aspect =
      static_cast<float>(person.height) /
      static_cast<float>(std::max(1, person.width));
  // Search window for the face net — intentionally a bit larger than a face.
  float headFrac = 0.26f;
  float widthFrac = 1.0f;
  if (aspect < 1.15f) {
    headFrac = 0.40f;
    widthFrac = 0.62f;
  } else if (aspect < 1.55f) {
    headFrac = 0.30f;
    widthFrac = 0.82f;
  }
  const int headH = std::max(16, static_cast<int>(person.height * headFrac));
  const int headW = std::max(16, static_cast<int>(person.width * widthFrac));
  const int cx = person.x + person.width / 2;
  const int padX = std::max(8, headW / 5);
  const int padY = std::max(8, headH / 5);
  cv::Rect r(cx - headW / 2 - padX, person.y - padY, headW + padX * 2,
             headH + padY * 2);
  r &= cv::Rect(0, 0, frameW, frameH);
  return r;
}

cv::Rect YoloDetectorDnn::personFaceRoi(const cv::Rect& person, int frameW,
                                        int frameH) {
  const float aspect =
      static_cast<float>(person.height) /
      static_cast<float>(std::max(1, person.width));
  // Standing person: face ≈ 1/7–1/8 of body height, ~0.55 of body width.
  float faceHFrac = 0.16f;
  float faceWFrac = 0.55f;
  if (aspect < 1.15f) {
    faceHFrac = 0.28f;
    faceWFrac = 0.48f;
  } else if (aspect < 1.55f) {
    faceHFrac = 0.20f;
    faceWFrac = 0.52f;
  }
  const int faceH = std::max(14, static_cast<int>(person.height * faceHFrac));
  const int faceW = std::max(14, static_cast<int>(person.width * faceWFrac));
  const int cx = person.x + person.width / 2;
  // Sit the box near the top of the person, slightly down from hairline.
  const int top = person.y + std::max(0, person.height / 40);
  const int pad = std::max(2, faceW / 10);
  cv::Rect r(cx - faceW / 2 - pad, top - pad / 2, faceW + pad * 2, faceH + pad);
  r &= cv::Rect(0, 0, frameW, frameH);
  return r;
}

cv::Rect YoloDetectorDnn::zoomAround(const cv::Rect& box, int frameW, int frameH) {
  const int dx = std::max(20, box.width * 3 / 4);
  const int dy = std::max(20, box.height * 3 / 4);
  cv::Rect r(box.x - dx, box.y - dy, box.width + 2 * dx, box.height + 2 * dy);
  r &= cv::Rect(0, 0, frameW, frameH);
  return r;
}

std::vector<cv::Rect>
YoloDetectorDnn::personHeadRois(const std::vector<YoloDetection>& dets,
                                int frameW, int frameH, int maxRois) {
  struct Cand {
    cv::Rect roi;
    int personH = 0;
  };
  std::vector<Cand> cands;
  const int closeH = std::max(200, frameH * 50 / 100);
  for (const auto& d : dets) {
    if (!isPersonClass(d.classId))
      continue;
    const float aspect = static_cast<float>(d.rect.height) /
                         static_cast<float>(std::max(1, d.rect.width));
    // Standing close-ups are already large in the full-frame pass. Keep zoom
    // for squat/overhead people — those faces are easy to miss otherwise.
    if (aspect > 1.4f && d.rect.height >= closeH)
      continue;
    cv::Rect roi = personHeadRoi(d.rect, frameW, frameH);
    if (roi.width < 12 || roi.height < 12)
      continue;
    cands.push_back({roi, d.rect.height});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    return a.personH < b.personH; // smallest (farthest) first
  });
  std::vector<cv::Rect> out;
  for (const auto& c : cands) {
    if (static_cast<int>(out.size()) >= maxRois)
      break;
    out.push_back(c.roi);
  }
  return out;
}

std::vector<cv::Rect>
YoloDetectorDnn::faceZoomRois(const std::vector<YoloDetection>& dets,
                              const std::vector<cv::Rect>& followBoxes, int frameW,
                              int frameH, int maxRois) {
  const int followBudget =
      std::min(4, std::max(0, maxRois / 3));
  const int personCap = std::max(1, maxRois - followBudget);
  std::vector<cv::Rect> out = personHeadRois(dets, frameW, frameH, personCap);
  for (const cv::Rect& box : followBoxes) {
    if (static_cast<int>(out.size()) >= maxRois)
      break;
    cv::Rect roi = zoomAround(box, frameW, frameH);
    if (roi.width < 12 || roi.height < 12)
      continue;
    out.push_back(roi);
  }
  return out;
}

std::vector<cv::Rect>
YoloDetectorDnn::allPersonHeads(const std::vector<YoloDetection>& dets, int frameW,
                                int frameH) {
  std::vector<cv::Rect> out;
  for (const auto& d : dets) {
    if (!isPersonClass(d.classId))
      continue;
    // Tight face proxy for coast-snap / redaction — not the large zoom window.
    cv::Rect roi = personFaceRoi(d.rect, frameW, frameH);
    if (roi.width >= 12 && roi.height >= 12)
      out.push_back(roi);
  }
  return out;
}

void YoloDetectorDnn::appendHeadFallbacks(std::vector<FaceDet>& faces,
                                          const std::vector<YoloDetection>& dets,
                                          int frameW, int frameH) {
  auto overlapsFace = [&](const cv::Rect& head) {
    for (const auto& f : faces) {
      const int x1 = std::max(head.x, f.rect.x);
      const int y1 = std::max(head.y, f.rect.y);
      const int x2 = std::min(head.x + head.width, f.rect.x + f.rect.width);
      const int y2 = std::min(head.y + head.height, f.rect.y + f.rect.height);
      const int iw = std::max(0, x2 - x1);
      const int ih = std::max(0, y2 - y1);
      if (iw * ih <= 0)
        continue;
      const float inter = static_cast<float>(iw * ih);
      const float uni = static_cast<float>(head.area() + f.rect.area()) - inter;
      if (uni > 1.f && inter / uni >= 0.12f)
        return true;
      const float hcx = head.x + head.width * 0.5f;
      const float hcy = head.y + head.height * 0.5f;
      if (hcx >= f.rect.x && hcx <= f.rect.x + f.rect.width && hcy >= f.rect.y &&
          hcy <= f.rect.y + f.rect.height)
        return true;
    }
    return false;
  };

  for (const auto& d : dets) {
    if (!isPersonClass(d.classId))
      continue;
    cv::Rect face = personFaceRoi(d.rect, frameW, frameH);
    if (face.width < 12 || face.height < 12)
      continue;
    if (overlapsFace(face))
      continue;
    FaceDet fake;
    fake.rect = face;
    // Below highThresh so these only re-associate existing tracks during a
    // brief face miss — they should not spawn huge new body tracks.
    fake.score = 0.18f;
    faces.push_back(fake);
  }
}

std::string YoloDetectorDnn::className(int classId) {
  if (classId >= 0 && classId < 80)
    return kCocoNames[classId];
  return "object";
}

void YoloDetectorDnn::useDefaultRedactionClasses() {
  allowed_ = {2, 3, 5, 7, 62, 63, 66, 67};
}

bool YoloDetectorDnn::load(const std::string& onnxPath, bool tryDirectMl) {
  ready_ = false;
  if (!fileExistsUtf8(onnxPath))
    return false;
  if (!session_.load(onnxPath, tryDirectMl))
    return false;
  if (allowed_.empty())
    useDefaultRedactionClasses();
  ready_ = true;
  return true;
}

std::vector<YoloDetection> YoloDetectorDnn::detect(const cv::Mat& bgrFrame) {
  std::vector<YoloDetection> out;
  if (!ready_ || bgrFrame.empty())
    return out;

  const int origW = bgrFrame.cols;
  const int origH = bgrFrame.rows;

  const float r = std::min(static_cast<float>(inputSize_) / origW,
                           static_cast<float>(inputSize_) / origH);
  const int newW = static_cast<int>(std::round(origW * r));
  const int newH = static_cast<int>(std::round(origH * r));
  const int padW = (inputSize_ - newW) / 2;
  const int padH = (inputSize_ - newH) / 2;

  cv::Mat resized;
  cv::resize(bgrFrame, resized, cv::Size(newW, newH));
  letterbox_.create(inputSize_, inputSize_, CV_8UC3);
  letterbox_.setTo(cv::Scalar(114, 114, 114));
  resized.copyTo(letterbox_(cv::Rect(padW, padH, newW, newH)));

  cv::dnn::blobFromImage(letterbox_, inferBlob_, 1.0 / 255.0, cv::Size(),
                         cv::Scalar(), true, false, CV_32F);
  if (!inferBlob_.isContinuous())
    inferBlob_ = inferBlob_.clone();

  const std::array<int64_t, 4> inputShape = {1, 3, inputSize_, inputSize_};
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputVal = Ort::Value::CreateTensor<float>(
      mem, inferBlob_.ptr<float>(), static_cast<size_t>(inferBlob_.total()),
      inputShape.data(), inputShape.size());

  const auto& inStore = session_.inputNames();
  const auto& outStore = session_.outputNames();
  const char* inName = !inStore.empty() ? inStore[0].c_str() : "images";
  const char* outName = !outStore.empty() ? outStore[0].c_str() : "output0";
  std::vector<const char*> inNames = {inName};
  std::vector<const char*> outNames = {outName};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(inputVal));
  std::vector<Ort::Value> outputs;
  if (!session_.run(inNames, inputs, outNames, outputs) || outputs.empty())
    return out;

  auto info = outputs[0].GetTensorTypeAndShapeInfo();
  const auto shape = info.GetShape();
  float* data = outputs[0].GetTensorMutableData<float>();

  // Expect [1, 84, N] or [1, N, 84]
  cv::Mat output;
  cv::Mat transposed;
  if (shape.size() == 3 && shape[1] == 84) {
    output = cv::Mat(84, static_cast<int>(shape[2]), CV_32FC1, data);
  } else if (shape.size() == 3 && shape[2] == 84) {
    cv::Mat tmp(static_cast<int>(shape[1]), 84, CV_32FC1, data);
    transposed = tmp.t();
    output = transposed;
  } else {
    return out;
  }

  if (output.rows < 84)
    return out;

  std::vector<cv::Rect> boxes;
  std::vector<float> scores;
  std::vector<int> classIds;

  for (int i = 0; i < output.cols; ++i) {
    float bestScore = 0.f;
    int bestCls = -1;
    for (int c = 0; c < 80; ++c) {
      const float s = output.at<float>(4 + c, i);
      if (s > bestScore) {
        bestScore = s;
        bestCls = c;
      }
    }
    if (bestScore < scoreThreshold_ && bestCls != 0)
      continue;
    if (bestCls == 0 && bestScore < 0.15f)
      continue;
    const bool keepPerson = (bestCls == 0);
    if (!keepPerson && !allowed_.empty() && allowed_.find(bestCls) == allowed_.end())
      continue;

    const float cx = output.at<float>(0, i);
    const float cy = output.at<float>(1, i);
    const float w = output.at<float>(2, i);
    const float h = output.at<float>(3, i);

    float x1 = (cx - w / 2.f - padW) / r;
    float y1 = (cy - h / 2.f - padH) / r;
    float x2 = (cx + w / 2.f - padW) / r;
    float y2 = (cy + h / 2.f - padH) / r;

    cv::Rect rect(static_cast<int>(x1), static_cast<int>(y1),
                  static_cast<int>(x2 - x1), static_cast<int>(y2 - y1));
    rect &= cv::Rect(0, 0, origW, origH);
    if (rect.width < 4 || rect.height < 4)
      continue;

    boxes.push_back(rect);
    scores.push_back(bestScore);
    classIds.push_back(bestCls);
  }

  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, 0.18f, nmsThreshold_, keep);

  out.reserve(keep.size());
  for (int idx : keep) {
    YoloDetection d;
    d.rect = boxes[idx];
    d.classId = classIds[idx];
    d.score = scores[idx];
    d.label = className(d.classId);
    out.push_back(d);
  }
  return out;
}

namespace {

std::vector<YoloDetection>
decodeYoloView(const float* base, bool chFirst, int anchors, float r, int padW,
               int padH, int origW, int origH, float scoreThreshold,
               float nmsThreshold, const std::set<int>& allowed) {
  std::vector<YoloDetection> out;
  if (!base || anchors <= 0 || origW < 2 || origH < 2 || r < 1e-6f)
    return out;

  auto at = [&](int c, int i) -> float {
    if (chFirst)
      return base[static_cast<size_t>(c) * static_cast<size_t>(anchors) +
                  static_cast<size_t>(i)];
    return base[static_cast<size_t>(i) * 84 + static_cast<size_t>(c)];
  };

  std::vector<cv::Rect> boxes;
  std::vector<float> scores;
  std::vector<int> classIds;
  for (int i = 0; i < anchors; ++i) {
    float bestScore = 0.f;
    int bestCls = -1;
    for (int c = 0; c < 80; ++c) {
      const float s = at(4 + c, i);
      if (s > bestScore) {
        bestScore = s;
        bestCls = c;
      }
    }
    if (bestScore < scoreThreshold && bestCls != 0)
      continue;
    if (bestCls == 0 && bestScore < 0.15f)
      continue;
    const bool keepPerson = (bestCls == 0);
    if (!keepPerson && !allowed.empty() && allowed.find(bestCls) == allowed.end())
      continue;

    const float cx = at(0, i);
    const float cy = at(1, i);
    const float w = at(2, i);
    const float h = at(3, i);
    const float x1 = (cx - w / 2.f - static_cast<float>(padW)) / r;
    const float y1 = (cy - h / 2.f - static_cast<float>(padH)) / r;
    const float x2 = (cx + w / 2.f - static_cast<float>(padW)) / r;
    const float y2 = (cy + h / 2.f - static_cast<float>(padH)) / r;
    cv::Rect rect(static_cast<int>(x1), static_cast<int>(y1),
                  static_cast<int>(x2 - x1), static_cast<int>(y2 - y1));
    rect &= cv::Rect(0, 0, origW, origH);
    if (rect.width < 4 || rect.height < 4)
      continue;
    boxes.push_back(rect);
    scores.push_back(bestScore);
    classIds.push_back(bestCls);
  }

  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, 0.18f, nmsThreshold, keep);
  out.reserve(keep.size());
  for (int idx : keep) {
    YoloDetection d;
    d.rect = boxes[static_cast<size_t>(idx)];
    d.classId = classIds[static_cast<size_t>(idx)];
    d.score = scores[static_cast<size_t>(idx)];
    d.label = YoloDetectorDnn::className(d.classId);
    out.push_back(d);
  }
  return out;
}

}  // namespace

std::vector<std::vector<YoloDetection>>
YoloDetectorDnn::detectMany(const std::vector<cv::Mat>& frames) {
  std::vector<std::vector<YoloDetection>> out(frames.size());
  if (!ready_ || frames.empty())
    return out;

  const int n = static_cast<int>(frames.size());
  if (n == 1) {
    out[0] = detect(frames[0]);
    return out;
  }

  const auto& modelIn = session_.inputShape(0);
  const bool fixedBatch1 = modelIn.size() >= 1 && modelIn[0] == 1;

  struct Meta {
    float r = 1.f;
    int padW = 0;
    int padH = 0;
    int origW = 0;
    int origH = 0;
  };
  std::vector<Meta> meta(static_cast<size_t>(n));
  const int inSize = inputSize_;
  const size_t plane = static_cast<size_t>(3) * static_cast<size_t>(inSize) *
                       static_cast<size_t>(inSize);
  batchBlob_.create(1, static_cast<int>(plane * static_cast<size_t>(n)), CV_32F);
  float* blob = batchBlob_.ptr<float>();

  cv::parallel_for_(cv::Range(0, n), [&](const cv::Range& range) {
    for (int b = range.start; b < range.end; ++b) {
      Meta& m = meta[static_cast<size_t>(b)];
      float* dst = blob + plane * static_cast<size_t>(b);
      const float fill = 114.f / 255.f;
      std::fill(dst, dst + plane, fill);
      const cv::Mat& src = frames[static_cast<size_t>(b)];
      if (src.empty())
        continue;
      m.origW = src.cols;
      m.origH = src.rows;
      m.r = std::min(static_cast<float>(inSize) / static_cast<float>(src.cols),
                     static_cast<float>(inSize) / static_cast<float>(src.rows));
      const int newW = std::max(1, static_cast<int>(std::round(src.cols * m.r)));
      const int newH = std::max(1, static_cast<int>(std::round(src.rows * m.r)));
      m.padW = (inSize - newW) / 2;
      m.padH = (inSize - newH) / 2;
      cv::Mat resized;
      cv::resize(src, resized, cv::Size(newW, newH));
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

  static const char* kInputNameFallback = "images";
  static const char* kOutputNameFallback = "output0";
  const auto& inStore = session_.inputNames();
  const auto& outStore = session_.outputNames();
  const char* inName =
      !inStore.empty() ? inStore[0].c_str() : kInputNameFallback;
  const char* outName =
      !outStore.empty() ? outStore[0].c_str() : kOutputNameFallback;
  std::vector<const char*> inNames = {inName};
  std::vector<const char*> outNames = {outName};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  if (!fixedBatch1 && batchOk_) {
    const std::array<int64_t, 4> inputShape = {n, 3, inSize, inSize};
    Ort::Value inputVal = Ort::Value::CreateTensor<float>(
        mem, blob, plane * static_cast<size_t>(n), inputShape.data(),
        inputShape.size());
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(inputVal));
    std::vector<Ort::Value> outputs;
    if (session_.run(inNames, inputs, outNames, outputs, /*fallbackOnFail=*/false) &&
        !outputs.empty()) {
      auto info = outputs[0].GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      const float* data = outputs[0].GetTensorData<float>();
      const bool chFirst = shape.size() == 3 && shape[1] == 84;
      const bool chLast = shape.size() == 3 && shape[2] == 84;
      if (shape.size() == 3 && shape[0] == n && data && (chFirst || chLast)) {
        const int anchors = static_cast<int>(chFirst ? shape[2] : shape[1]);
        const size_t stride = static_cast<size_t>(anchors) * 84;
        for (int b = 0; b < n; ++b) {
          const Meta& m = meta[static_cast<size_t>(b)];
          out[static_cast<size_t>(b)] = decodeYoloView(
              data + stride * static_cast<size_t>(b), chFirst, anchors, m.r,
              m.padW, m.padH, m.origW, m.origH, scoreThreshold_, nmsThreshold_,
              allowed_);
        }
        return out;
      }
    }
    batchOk_ = false;
  }

  for (int b = 0; b < n; ++b) {
    const std::array<int64_t, 4> oneShape = {1, 3, inSize, inSize};
    float* slice = blob + plane * static_cast<size_t>(b);
    Ort::Value inputVal = Ort::Value::CreateTensor<float>(
        mem, slice, plane, oneShape.data(), oneShape.size());
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(inputVal));
    std::vector<Ort::Value> outputs;
    const Meta& m = meta[static_cast<size_t>(b)];
    if (!session_.run(inNames, inputs, outNames, outputs) || outputs.empty()) {
      out[static_cast<size_t>(b)] = detect(frames[static_cast<size_t>(b)]);
      continue;
    }
    auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    const float* data = outputs[0].GetTensorData<float>();
    const bool chFirst = shape.size() == 3 && shape[1] == 84;
    const bool chLast = shape.size() == 3 && shape[2] == 84;
    if (!data || (!chFirst && !chLast)) {
      out[static_cast<size_t>(b)] = detect(frames[static_cast<size_t>(b)]);
      continue;
    }
    const int anchors = static_cast<int>(chFirst ? shape[2] : shape[1]);
    out[static_cast<size_t>(b)] = decodeYoloView(
        data, chFirst, anchors, m.r, m.padW, m.padH, m.origW, m.origH,
        scoreThreshold_, nmsThreshold_, allowed_);
  }
  return out;
}

std::vector<YoloDetection>
YoloDetectorDnn::toPlateZones(const std::vector<YoloDetection>& dets,
                              float bottomRatio) {
  std::vector<YoloDetection> zones;
  for (const auto& d : dets) {
    if (!isVehicleClass(d.classId))
      continue;
    YoloDetection z = d;
    const int band = std::max(4, static_cast<int>(d.rect.height * bottomRatio));
    z.rect.y = d.rect.y + d.rect.height - band;
    z.rect.height = band;
    z.label = "plate-zone";
    zones.push_back(z);
  }
  return zones;
}

}  // namespace vb
