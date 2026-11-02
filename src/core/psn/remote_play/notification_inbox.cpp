#include "src/core/psn/remote_play/notification_inbox.hpp"

#include <cstddef>
#include <utility>

namespace libchiaking::psn::remote_play {

auto NotificationInbox::push_back(psn::rest::Notification notification)
    -> bool {
  if (!has_room_for(notification.size())) return false;
  bytes_ += notification.size();
  items_.push_back(std::move(notification));
  return true;
}

auto NotificationInbox::push_shedding_oldest(
    psn::rest::Notification notification) -> std::optional<std::size_t> {
  if (notification.size() > byte_budget_) return std::nullopt;
  std::size_t shed = 0;
  while (!items_.empty() && !has_room_for(notification.size())) {
    bytes_ -= items_.front().size();
    items_.pop_front();
    ++shed;
  }
  bytes_ += notification.size();
  items_.push_back(std::move(notification));
  return shed;
}

auto NotificationInbox::push_front(
    std::deque<psn::rest::Notification> notifications) -> std::size_t {
  std::size_t kept = 0;
  for (auto& notification : notifications) {
    if (!has_room_for(notification.size())) break;
    bytes_ += notification.size();
    items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(kept),
                  std::move(notification));
    ++kept;
  }
  return notifications.size() - kept;
}

auto NotificationInbox::take_front() -> std::optional<psn::rest::Notification> {
  if (items_.empty()) return std::nullopt;
  auto front = std::move(items_.front());
  items_.pop_front();
  bytes_ -= front.size();
  return front;
}

void NotificationInbox::clear() noexcept {
  items_.clear();
  bytes_ = 0;
}

}  // namespace libchiaking::psn::remote_play
