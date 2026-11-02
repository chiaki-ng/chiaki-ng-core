/// @file
/// @brief Declares BoundedQueue, a first-in, first-out queue that never grows
/// past a count of items and, optionally, a budget of bytes.
///
/// A live stream queues events for the frontend in one, so that a frontend
/// falling behind costs dropped events rather than unbounded memory.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace libchiaking::containers {

/// @brief A FIFO queue capped by item count and, optionally, by total weight
/// in bytes, which either refuses or makes room when full.
///
/// Invariants: size() never exceeds the capacity and, with a byte budget,
/// bytes() never exceeds the budget. An item whose weight alone exceeds the
/// budget, or any item when the capacity is zero, is always refused, even
/// when shedding.
///
/// Items are held in a ring that grows by doubling, up to the capacity, and
/// never shrinks, so a queue that has reached its working size pushes and
/// takes without allocating. Not synchronised: the owner must guard it when
/// several threads use it.
///
/// @tparam Item The queued value; must be movable.
template <class Item>
class BoundedQueue {
 public:
  /// @brief What push() does when the new item does not fit.
  enum class WhenFull : std::uint8_t {
    /// @brief Refuses the new item and keeps the queue as it is.
    refuse_newest,
    /// @brief Drops items from the front, oldest first, until the new
    /// item fits, counting each one in shed().
    shed_oldest,
  };

  /// @brief Says how many bytes an item counts for against the byte
  /// budget.
  using Weigh = std::function<std::size_t(const Item&)>;

  /// @brief Makes an empty queue limited by item count only.
  ///
  /// @param capacity The most items held at once; zero refuses every
  ///     item.
  /// @param when_full What to do when a push does not fit.
  explicit BoundedQueue(std::size_t capacity,
                        WhenFull when_full = WhenFull::refuse_newest)
      : capacity_(capacity), when_full_(when_full) {}

  /// @brief Makes an empty queue limited by item count and by total weight.
  ///
  /// @param capacity The most items held at once; zero refuses every
  ///     item.
  /// @param byte_budget The most bytes, as weighed by `weigh`, held at
  ///     once.
  /// @param weigh Weighs each item once, when it is pushed; must not be
  ///     empty.
  /// @param when_full What to do when a push does not fit.
  BoundedQueue(std::size_t capacity, std::size_t byte_budget, Weigh weigh,
               WhenFull when_full = WhenFull::refuse_newest)
      : capacity_(capacity),
        when_full_(when_full),
        budget_(ByteBudget{byte_budget, std::move(weigh)}) {}

  /// @brief Adds an item at the back, shedding older items first when the
  /// queue sheds.
  ///
  /// @param item The item to add.
  /// @return True when the item was queued; false when it was refused,
  ///     because the queue is full and refuses the newest, or because the
  ///     item could never fit.
  auto push(Item item) -> bool {
    const auto weight = weight_of(item);
    if (!could_ever_hold(weight)) return false;
    if (when_full_ == WhenFull::shed_oldest)
      while (!has_room_for(weight)) shed_front();
    if (!has_room_for(weight)) return false;
    if (size_ == ring_.size()) grow();
    ring_[slot(size_)].emplace(Entry{std::move(item), weight});
    ++size_;
    bytes_ += weight;
    return true;
  }

  /// @brief Removes the item at the front.
  ///
  /// @return The oldest item, or nothing when the queue is empty.
  auto take() -> std::optional<Item> {
    if (size_ == 0) return std::nullopt;
    auto front = pop_front();
    bytes_ -= front.weight;
    return std::move(front.item);
  }

  /// @brief Drops every item and keeps the ring for reuse; the shed count is
  /// kept.
  void clear() noexcept {
    for (auto& entry : ring_) entry.reset();
    head_ = 0;
    size_ = 0;
    bytes_ = 0;
  }

  /// @brief Counts the items queued.
  ///
  /// @return The number of items.
  auto size() const noexcept -> std::size_t { return size_; }

  /// @brief Tells whether nothing is queued.
  ///
  /// @return True when the queue holds no item.
  auto empty() const noexcept -> bool { return size_ == 0; }

  /// @brief Totals the weight of the items queued.
  ///
  /// @return The weight in bytes; always zero without a byte budget.
  auto bytes() const noexcept -> std::size_t { return bytes_; }

  /// @brief Counts the items dropped to make room since construction.
  ///
  /// @return The number of shed items; clear() and take() do not count.
  auto shed() const noexcept -> std::uint64_t { return shed_; }

 private:
  /// @brief A queued item with the weight it was charged on push.
  struct Entry {
    /// @brief The queued item.
    Item item;
    /// @brief Its weight in bytes, zero without a byte budget.
    std::size_t weight{};
  };

  /// @brief The optional limit on total weight and how to weigh an item.
  struct ByteBudget {
    /// @brief The most bytes held at once.
    std::size_t limit{};
    /// @brief Weighs an item in bytes.
    Weigh weigh;
  };

  /// @brief Weighs an item against the budget.
  ///
  /// @param item The item.
  /// @return Its weight in bytes, or zero without a byte budget.
  auto weight_of(const Item& item) const -> std::size_t {
    return budget_ ? budget_->weigh(item) : 0;
  }

  /// @brief Tells whether an item of this weight fits an empty queue.
  ///
  /// @param weight The item's weight in bytes.
  /// @return False when the capacity is zero or the weight exceeds the
  ///     whole budget.
  auto could_ever_hold(std::size_t weight) const noexcept -> bool {
    return capacity_ != 0 && (!budget_ || weight <= budget_->limit);
  }

  /// @brief Tells whether an item of this weight fits now.
  ///
  /// @param weight The item's weight in bytes.
  /// @return True when there is a free slot and enough unused budget.
  auto has_room_for(std::size_t weight) const noexcept -> bool {
    return size_ < capacity_ && (!budget_ || weight <= budget_->limit - bytes_);
  }

  /// @brief Drops the oldest item and counts it as shed; the queue must
  /// not be empty.
  void shed_front() noexcept {
    bytes_ -= pop_front().weight;
    ++shed_;
  }

  /// @brief The ring slot of the item at a position from the front.
  ///
  /// @param position The position; less than the ring's size.
  /// @return The slot's index.
  auto slot(std::size_t position) const noexcept -> std::size_t {
    return (head_ + position) % ring_.size();
  }

  /// @brief Removes the item at the front; the queue must not be empty.
  ///
  /// @return The removed entry.
  auto pop_front() noexcept -> Entry {
    auto front = std::move(*ring_[head_]);
    ring_[head_].reset();
    head_ = slot(1);
    --size_;
    return front;
  }

  /// @brief Doubles the ring, at least to the first growth's size and at
  /// most to the capacity, moving the items to its start in order.
  void grow() {
    const auto larger =
        std::min(capacity_, std::max(first_ring_size, ring_.size() * 2));
    std::vector<std::optional<Entry>> ring(larger);
    for (std::size_t position = 0; position < size_; ++position)
      ring[position] = std::move(ring_[slot(position)]);
    ring_ = std::move(ring);
    head_ = 0;
  }

  /// @brief The ring's size when the first item arrives.
  static constexpr std::size_t first_ring_size = 8;

  /// @brief The most items held at once.
  std::size_t capacity_;
  /// @brief What a push that does not fit does.
  WhenFull when_full_;
  /// @brief The byte budget, or nothing when only the count is limited.
  std::optional<ByteBudget> budget_;
  /// @brief The ring; slots outside the queued run are empty.
  std::vector<std::optional<Entry>> ring_;
  /// @brief The ring slot of the oldest item.
  std::size_t head_{};
  /// @brief How many items are queued.
  std::size_t size_{};
  /// @brief The total weight of the queued items, in bytes.
  std::size_t bytes_{};
  /// @brief How many items have been shed since construction.
  std::uint64_t shed_{};
};

}  // namespace libchiaking::containers
