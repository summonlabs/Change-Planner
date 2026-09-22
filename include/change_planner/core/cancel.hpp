#pragma once

#include <atomic>
#include <memory>

namespace cplan {

// Cooperative cancellation. A cancelled planning run reports
// ErrorCode::cancelled and never publishes a plan: cancellation is checked at
// bounded intervals by every search loop.
class StopSource;

class StopToken {
 public:
  StopToken() = default;

  bool stop_requested() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }

  bool cancellable() const noexcept { return flag_ != nullptr; }

 private:
  friend class StopSource;
  explicit StopToken(std::shared_ptr<const std::atomic<bool>> flag) : flag_(std::move(flag)) {}

  std::shared_ptr<const std::atomic<bool>> flag_{};
};

class StopSource {
 public:
  StopSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  StopToken token() const noexcept { return StopToken(flag_); }

  void request_stop() noexcept { flag_->store(true, std::memory_order_release); }

  bool stop_requested() const noexcept { return flag_->load(std::memory_order_acquire); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace cplan
