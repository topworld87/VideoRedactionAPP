#include "vb/FaceReId.h"

#include "vb/fs.h"

#include <algorithm>
#include <cmath>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace vb {
namespace {
const cv::Point2f kArcFace128[5] = {
    {34.6224f, 59.0815f}, {74.8935f, 58.8587f}, {54.8860f, 81.9847f},
    {37.7706f, 105.5606f}, {71.6913f, 105.3761f},
};
}  // namespace

bool FaceReId::load(const std::string& onnxPathUtf8, bool tryGpu) {
  ready_ = false;
  if (!fileExistsUtf8(onnxPathUtf8))
    return false;
  if (!session_.load(onnxPathUtf8, tryGpu))
    return false;
  if (session_.inputWidth() > 0)
    inW_ = session_.inputWidth();
  if (session_.inputHeight() > 0)
    inH_ = session_.inputHeight();
  ready_ = true;
  return true;
}

cv::Mat FaceReId::alignFace(const cv::Mat& bgr, const std::array<cv::Point2f, 5>& pts,
                            int outW, int outH) {
  cv::Point2f src[3] = {pts[0], pts[1], pts[2]};
  cv::Point2f dst[3] = {kArcFace128[0], kArcFace128[1], kArcFace128[2]};
  if (outW != 128 || outH != 128) {
    const float sx = static_cast<float>(outW) / 128.f;
    const float sy = static_cast<float>(outH) / 128.f;
    for (auto& p : dst) {
      p.x *= sx;
      p.y *= sy;
    }
  }
  const cv::Mat m = cv::getAffineTransform(src, dst);
  cv::Mat aligned;
  cv::warpAffine(bgr, aligned, m, cv::Size(outW, outH), cv::INTER_LINEAR,
                 cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
  return aligned;
}

std::vector<float> FaceReId::l2(const std::vector<float>& v) {
  double acc = 0;
  for (float x : v)
    acc += static_cast<double>(x) * static_cast<double>(x);
  const float n = static_cast<float>(std::sqrt(std::max(acc, 1e-12)));
  std::vector<float> out(v.size());
  for (size_t i = 0; i < v.size(); ++i)
    out[i] = v[i] / n;
  return out;
}

std::vector<float> FaceReId::embed(const cv::Mat& bgr, const cv::Rect& face,
                                   const std::array<cv::Point2f, 5>* landmarks) const {
  std::vector<float> empty;
  if (!ready_ || bgr.empty())
    return empty;

  cv::Mat crop;
  if (landmarks) {
    crop = alignFace(bgr, *landmarks, inW_, inH_);
  } else {
    const cv::Rect bounds(0, 0, bgr.cols, bgr.rows);
    cv::Rect roi = face & bounds;
    if (roi.width < 8 || roi.height < 8)
      return empty;
    cv::resize(bgr(roi), crop, cv::Size(inW_, inH_), 0, 0, cv::INTER_LINEAR);
  }
  if (crop.empty())
    return empty;

  cv::dnn::blobFromImage(crop, blob_, 1.0, cv::Size(), cv::Scalar(), false, false,
                         CV_32F);
  if (!blob_.isContinuous())
    blob_ = blob_.clone();

  const std::array<int64_t, 4> shape = {1, 3, inH_, inW_};
  Ort::MemoryInfo mem =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input = Ort::Value::CreateTensor<float>(
      mem, blob_.ptr<float>(), static_cast<size_t>(blob_.total()), shape.data(),
      shape.size());
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(input));
  std::vector<Ort::Value> outputs;
  if (!const_cast<OrtModelSession&>(session_).run(inputs, outputs) || outputs.empty())
    return empty;

  const float* data = outputs[0].GetTensorData<float>();
  const size_t n = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
  std::vector<float> emb(data, data + n);
  return l2(emb);
}

}  // namespace vb
