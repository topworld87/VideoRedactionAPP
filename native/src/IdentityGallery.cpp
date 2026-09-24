#include "vb/IdentityGallery.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vb {

void IdentityGallery::clear() {
  std::lock_guard<std::mutex> lock(mu_);
  ids_.clear();
  nextId_ = 1;
}

IdentityGallery::IdentityGallery(const IdentityGallery& other) { *this = other; }

IdentityGallery& IdentityGallery::operator=(const IdentityGallery& other) {
  if (this == &other)
    return *this;
  auto snap = other.snapshot();
  int maxId = 0;
  for (const auto& id : snap)
    maxId = std::max(maxId, id.id);
  std::lock_guard<std::mutex> lock(mu_);
  ids_ = std::move(snap);
  nextId_ = maxId + 1;
  return *this;
}

std::vector<float> IdentityGallery::l2(const std::vector<float>& v) {
  double acc = 0;
  for (float x : v)
    acc += static_cast<double>(x) * static_cast<double>(x);
  const float n = static_cast<float>(std::sqrt(std::max(acc, 1e-12)));
  std::vector<float> out(v.size());
  for (size_t i = 0; i < v.size(); ++i)
    out[i] = v[i] / n;
  return out;
}

float IdentityGallery::cosine(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || a.size() != b.size())
    return 0.f;
  double acc = 0;
  for (size_t i = 0; i < a.size(); ++i)
    acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  return static_cast<float>(acc);
}

int IdentityGallery::enroll(const std::vector<float>& embedding, const std::string& name) {
  if (embedding.empty())
    return -1;
  std::lock_guard<std::mutex> lock(mu_);
  KeepIdentity id;
  id.id = nextId_++;
  id.name = name.empty() ? ("Keep #" + std::to_string(id.id)) : name;
  id.embedding = l2(embedding);
  id.sampleCount = 1;
  ids_.push_back(id);
  return id.id;
}

void IdentityGallery::addSample(int identityId, const std::vector<float>& embedding) {
  if (embedding.empty())
    return;
  const auto sample = l2(embedding);
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& id : ids_) {
    if (id.id != identityId)
      continue;
    if (id.embedding.size() != sample.size()) {
      id.embedding = sample;
      id.sampleCount = 1;
      return;
    }
    const float n = static_cast<float>(id.sampleCount);
    for (size_t i = 0; i < sample.size(); ++i)
      id.embedding[i] = (id.embedding[i] * n + sample[i]) / (n + 1.f);
    id.embedding = l2(id.embedding);
    ++id.sampleCount;
    return;
  }
}

IdentityMatch IdentityGallery::query(const std::vector<float>& embedding,
                                     int excludeIdentityId) const {
  IdentityMatch m;
  if (embedding.empty())
    return m;
  const auto q = l2(embedding);
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& id : ids_) {
    if (id.id == excludeIdentityId)
      continue;
    const float s = cosine(q, id.embedding);
    if (s > m.score) {
      m.score = s;
      m.identityId = id.id;
    }
  }
  if (m.identityId < 0)
    return m;
  if (m.score >= kAutoThresh)
    m.tier = MatchTier::AutoLinked;
  else if (m.score >= kSuggestThresh)
    m.tier = MatchTier::Suggest;
  else {
    m.identityId = -1;
    m.tier = MatchTier::None;
  }
  return m;
}

std::vector<KeepIdentity> IdentityGallery::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ids_;
}

bool IdentityGallery::empty() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ids_.empty();
}

size_t IdentityGallery::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ids_.size();
}

}  // namespace vb
