#include "change_planner/persist/repository.hpp"

#include <algorithm>

namespace cplan {

PlanRepository::PlanRepository(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

Result<void> PlanRepository::publish(const Plan& plan) {
  const Status structure = plan.verify_structure();
  if (!structure.ok()) {
    return Status::error(structure.code(),
                         "refusing to publish an invalid plan: " + structure.message());
  }
  // Serialisation (integrity digest) happens before the lock is taken.
  const std::vector<std::byte> bytes = serialize_plan_artifact(plan);
  auto artifact = parse_plan_artifact(bytes);
  if (!artifact.ok()) {
    return artifact.status();
  }
  const std::pair<PlanId, PlanGeneration> key{plan.id, plan.generation};

  std::lock_guard<std::mutex> guard(mutex_);
  const auto existing = slots_.find(key);
  if (existing != slots_.end()) {
    if (existing->second.payload_digest == artifact.value().payload_digest) {
      return Status::success();  // idempotent republication
    }
    ++rejections_;
    return Status::error(ErrorCode::conflict,
                         "plan '" + plan.id.str() + "' generation " +
                             std::to_string(plan.generation.value()) +
                             " is already published with different content");
  }
  while (slots_.size() >= capacity_) {
    // Deterministic eviction: the lowest (plan id, generation) slot retires.
    slots_.erase(slots_.begin());
    ++evictions_;
  }
  slots_.emplace(key, std::move(artifact.value()));
  return Status::success();
}

Result<PlanArtifact> PlanRepository::find(const PlanId& id, PlanGeneration generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto slot = slots_.find({id, generation});
  if (slot == slots_.end()) {
    return Status::error(ErrorCode::not_found,
                         "plan '" + id.str() + "' generation " +
                             std::to_string(generation.value()) + " is not in the repository");
  }
  return slot->second;
}

std::vector<PlanArtifact> PlanRepository::list() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<PlanArtifact> result;
  result.reserve(slots_.size());
  for (const auto& entry : slots_) {
    result.push_back(entry.second);
  }
  return result;
}

std::size_t PlanRepository::size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return slots_.size();
}

std::size_t PlanRepository::capacity() const { return capacity_; }

std::uint64_t PlanRepository::evictions() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evictions_;
}

std::uint64_t PlanRepository::rejections() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return rejections_;
}

std::size_t PlanRepository::retire_below(PlanGeneration generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t retired = 0;
  for (auto slot = slots_.begin(); slot != slots_.end();) {
    if (slot->first.second < generation) {
      slot = slots_.erase(slot);
      ++retired;
    } else {
      ++slot;
    }
  }
  return retired;
}

}  // namespace cplan
