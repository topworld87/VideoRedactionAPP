#pragma once

#include "vb/TrackObject.h"

#include <mutex>
#include <string>
#include <vector>

namespace vb {

struct KeepIdentity {
  int id = 0;
  std::string name;
  std::vector<float> embedding;
  int sampleCount = 0;
};

struct IdentityMatch {
  int identityId = -1;
  float score = 0.f;
  MatchTier tier = MatchTier::None;
};

class IdentityGallery {
public:
  IdentityGallery() = default;
  IdentityGallery(const IdentityGallery& other);
  IdentityGallery& operator=(const IdentityGallery& other);

  void clear();
  int enroll(const std::vector<float>& embedding, const std::string& name = {});
  void addSample(int identityId, const std::vector<float>& embedding);
  IdentityMatch query(const std::vector<float>& embedding,
                      int excludeIdentityId = -1) const;
  std::vector<KeepIdentity> snapshot() const;
  bool empty() const;
  size_t size() const;

  static constexpr float kAutoThresh = 0.62f;
  static constexpr float kSuggestThresh = 0.48f;

private:
  static std::vector<float> l2(const std::vector<float>& v);
  static float cosine(const std::vector<float>& a, const std::vector<float>& b);

  mutable std::mutex mu_;
  std::vector<KeepIdentity> ids_;
  int nextId_ = 1;
};

}  // namespace vb
