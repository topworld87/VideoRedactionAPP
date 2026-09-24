#include "vb/FaceLandmarks.h"

#include "vb/fs.h"

#include <algorithm>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace vb {

bool FaceLandmarks::load(const std::string& onnxPathUtf8, bool tryGpu) {
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

bool FaceLandmarks::infer(const cv::Mat& bgr, const cv::Rect& face,
                          std::array<cv::Point2f, 5>& points) const {
  if (!ready_ || bgr.empty())
    return false;
  const cv::Rect bounds(0, 0, bgr.cols, bgr.rows);
  cv::Rect roi = face & bounds;
  if (roi.width < 8 || roi.height < 8)
    return false;

  cv::Mat crop = bgr(roi);
  cv::Mat resized;
  cv::resize(crop, resized, cv::Size(inW_, inH_), 0, 0, cv::INTER_LINEAR);
  cv::dnn::blobFromImage(resized, blob_, 1.0, cv::Size(), cv::Scalar(), false, false,
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
    return false;

  auto info = outputs[0].GetTensorTypeAndShapeInfo();
  const size_t count = info.GetElementCount();
  if (count < 10)
    return false;
  const float* data = outputs[0].GetTensorData<float>();
  for (int i = 0; i < 5; ++i) {
    points[static_cast<size_t>(i)] = cv::Point2f(
        roi.x + data[i * 2] * static_cast<float>(roi.width),
        roi.y + data[i * 2 + 1] * static_cast<float>(roi.height));
  }
  return true;
}

}  // namespace vb
