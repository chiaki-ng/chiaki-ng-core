#include "src/core/play/adapters/links/linked_stop.hpp"

#include <utility>

namespace libchiaking::play {

LinkedStop::LinkedStop(platform::StopToken parent) noexcept
    : parent_(std::move(parent)) {}

void LinkedStop::cancel() noexcept {
  cancelled_.store(true, std::memory_order_release);
}

bool LinkedStop::stop_requested() const noexcept {
  return cancelled_.load(std::memory_order_acquire) || parent_.stop_requested();
}

bool LinkedStop::deadline_expired() const noexcept {
  return !cancelled_.load(std::memory_order_acquire) &&
         parent_.deadline_expired();
}

}  // namespace libchiaking::play
