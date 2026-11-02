/// @file
/// @brief Declares NotificationInbox, a bounded queue of push notifications
/// held until the stage that wants them asks.
#pragma once

#include "src/core/psn/rest/push/push.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace libchiaking::psn::remote_play {

/// @brief Push notifications in arrival order, bounded by a count and a total
/// body size.
///
/// Not thread-safe; its owner locks it.
class NotificationInbox {
 public:
  /// @brief What scan() does with a notification.
  enum class Verdict : std::uint8_t {
    /// @brief Leave it and look at the next.
    keep,
    /// @brief Remove it and look at the next.
    drop,
    /// @brief Remove it and return it.
    take,
    /// @brief End the walk, returning nothing.
    stop
  };

  /// @brief Makes an empty queue.
  ///
  /// @param capacity The most notifications held.
  /// @param byte_budget The most body bytes held in all.
  NotificationInbox(std::size_t capacity, std::size_t byte_budget) noexcept
      : capacity_(capacity), byte_budget_(byte_budget) {}

  /// @brief Queues a notification if there is room.
  ///
  /// @param notification The notification.
  /// @return False, dropping it, when the count or the byte budget is full.
  [[nodiscard]] auto push_back(psn::rest::Notification notification) -> bool;
  /// @brief Queues a notification, dropping the oldest until it fits.
  ///
  /// @param notification The notification.
  /// @return How many were dropped; nothing, queuing nothing, when it alone
  ///     is larger than the budget.
  [[nodiscard]] auto push_shedding_oldest(psn::rest::Notification notification)
      -> std::optional<std::size_t>;
  /// @brief Puts notifications back at the front, ahead of those queued,
  /// keeping their order, as far as the bounds allow.
  ///
  /// They are admitted oldest first; from the first one that would exceed
  /// the count or the byte budget, it and every newer one are dropped. The
  /// notifications already queued are kept.
  ///
  /// @param notifications The notifications, oldest first; moved from.
  /// @return How many were dropped; 0 when all fitted.
  [[nodiscard]] auto push_front(
      std::deque<psn::rest::Notification> notifications) -> std::size_t;
  /// @brief Takes the oldest notification.
  ///
  /// @return It, or nothing when empty.
  [[nodiscard]] auto take_front() -> std::optional<psn::rest::Notification>;

  /// @brief Walks the queue oldest first, letting a judge keep, drop or take
  /// each notification or stop the walk.
  ///
  /// @tparam Judge Callable as `judge (const psn::rest::Notification&)`
  ///     returning a Verdict.
  /// @param judge The judge.
  /// @return The first notification taken; nothing when the walk ended or was
  ///     stopped first.
  template <class Judge>
  [[nodiscard]] auto scan(Judge&& judge)
      -> std::optional<psn::rest::Notification> {
    for (auto item = items_.begin(); item != items_.end();) {
      switch (judge(*item)) {
        case Verdict::keep:
          ++item;
          break;
        case Verdict::drop:
          bytes_ -= item->size();
          item = items_.erase(item);
          break;
        case Verdict::take: {
          bytes_ -= item->size();
          auto taken = std::move(*item);
          items_.erase(item);
          return taken;
        }
        case Verdict::stop:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  /// @brief Empties the queue.
  void clear() noexcept;
  /// @brief Counts the notifications.
  ///
  /// @return The count.
  auto size() const noexcept -> std::size_t { return items_.size(); }
  /// @brief Tells whether the queue is empty.
  ///
  /// @return True when it is.
  auto empty() const noexcept -> bool { return items_.empty(); }

 private:
  /// @brief Tells whether one more notification fits.
  ///
  /// @param bytes Its body size.
  /// @return True when below the count and within the remaining budget;
  ///     false whenever the bytes held already reach or exceed the budget.
  auto has_room_for(std::size_t bytes) const noexcept -> bool {
    return items_.size() < capacity_ && bytes_ <= byte_budget_ &&
           bytes <= byte_budget_ - bytes_;
  }

  /// @brief The most notifications held.
  std::size_t capacity_;
  /// @brief The most body bytes held.
  std::size_t byte_budget_;
  /// @brief The notifications, oldest first.
  std::deque<psn::rest::Notification> items_;
  /// @brief The body bytes held.
  std::size_t bytes_{};
};

}  // namespace libchiaking::psn::remote_play
