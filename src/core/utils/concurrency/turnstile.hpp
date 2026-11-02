/// @file
/// @brief Declares Turnstile, which lets a bounded number of callers do
/// something at once and queues a bounded number more in arrival order.
///
/// The catalogue uses one to cap how many title details are downloaded at
/// once, refusing further requests rather than queueing without limit.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace libchiaking::concurrency {

/// @brief A counting gate with a fair, bounded waiting line.
///
/// At most `inside` callers hold a Pass at once. A caller arriving when the
/// gate is full joins the line, unless the line already holds `waiting`
/// callers, in which case it is refused at once. The line is first in,
/// first out: a newcomer never overtakes someone already waiting, even when
/// a place is free, and each waiter is admitted only when it is at the front.
///
/// The object is safe to share between threads; one internal mutex guards
/// it. It must outlive every Pass it hands out and every caller waiting in
/// enter().
class Turnstile {
 public:
  /// @brief Why enter() turned a caller away.
  enum class Refusal : std::uint8_t { stopped, crowded };

  /// @var Refusal::stopped
  /// @brief The caller's stop token fired before or while it waited; it
  /// has left the line.
  /// @var Refusal::crowded
  /// @brief The gate was full and the line already held as many callers
  /// as it may.

  /// @brief One caller's place inside the gate, given up when the pass is
  /// destroyed.
  ///
  /// Move-only; a moved-from pass holds no place. Must not outlive the
  /// Turnstile that issued it. Releasing it may happen on any thread.
  class Pass {
   public:
    /// @brief Takes over another pass's place, leaving the other empty.
    ///
    /// @param other The pass to take the place from.
    Pass(Pass&& other) noexcept
        : turnstile_(std::exchange(other.turnstile_, nullptr)) {}
    /// @brief Gives up this pass's place, if it holds one, and takes over
    /// another's.
    ///
    /// @param other The pass to take the place from.
    /// @return This pass.
    auto operator=(Pass&& other) noexcept -> Pass& {
      if (this != &other) {
        release();
        turnstile_ = std::exchange(other.turnstile_, nullptr);
      }
      return *this;
    }
    Pass(const Pass&) = delete;
    auto operator=(const Pass&) -> Pass& = delete;
    /// @brief Gives up the place, letting the next waiter in; briefly
    /// takes the turnstile's mutex.
    ~Pass() { release(); }

   private:
    friend class Turnstile;

    /// @brief Makes a pass holding a place the turnstile has already
    /// counted.
    ///
    /// @param turnstile The issuing turnstile.
    explicit Pass(Turnstile& turnstile) noexcept : turnstile_(&turnstile) {}

    /// @brief Gives up the place, if this pass holds one, and empties the
    /// pass.
    void release() noexcept {
      if (turnstile_ != nullptr) std::exchange(turnstile_, nullptr)->leave();
    }

    /// @brief The turnstile this pass holds a place in, or null when it
    /// holds none.
    Turnstile* turnstile_;
  };

  /// @brief Makes an empty gate.
  ///
  /// @param threading Provider of the gate's mutex and condition variable;
  ///     must outlive the gate.
  /// @param clock Monotonic clock the condition variable measures on; must
  ///     outlive the gate.
  /// @param inside The most callers let in at once; zero is taken as one.
  /// @param waiting The most callers that may wait in line; zero refuses
  ///     everyone who finds the gate full.
  Turnstile(platform::ThreadingProvider& threading,
            const platform::ClockProvider& clock, std::size_t inside,
            std::size_t waiting)
      : most_inside_(std::max<std::size_t>(inside, 1)),
        most_waiting_(waiting),
        mutex_(threading),
        changed_(threading, clock) {}
  Turnstile(const Turnstile&) = delete;
  auto operator=(const Turnstile&) -> Turnstile& = delete;

  /// @brief Goes through the gate, waiting in line when it is full.
  ///
  /// Lets the caller straight in when nobody waits and a place is free.
  /// Otherwise joins the back of the line and blocks until it reaches the
  /// front and a place is free, or until `stop` fires.
  /// How long that takes depends entirely on how long earlier passes are
  /// held.
  ///
  /// @param stop Cancels the wait; already fired on entry, the caller is
  ///     refused without joining the line.
  /// @return A pass holding a place inside.
  /// @retval Refusal::stopped `stop` fired before a place was granted.
  /// @retval Refusal::crowded The gate was full and the line at its limit.
  [[nodiscard]] auto enter(const platform::StopToken& stop)
      -> Expected<Pass, Refusal> {
    platform::Lock lock{mutex_};
    if (stop.stop_requested()) return unexpected(Refusal::stopped);
    if (queue_.empty() && inside_ < most_inside_) {
      ++inside_;
      return Pass{*this};
    }
    if (queue_.size() >= most_waiting_) return unexpected(Refusal::crowded);
    const auto ticket = next_ticket_++;
    queue_.push_back(ticket);
    for (;;) {
      if (stop.stop_requested()) {
        queue_.erase(std::ranges::find(queue_, ticket));
        changed_.notify_all();
        return unexpected(Refusal::stopped);
      }
      if (queue_.front() == ticket && inside_ < most_inside_) break;
      static_cast<void>(changed_.wait(lock, stop));
    }
    queue_.pop_front();
    ++inside_;
    changed_.notify_all();
    return Pass{*this};
  }

  /// @brief Counts the passes held now; the answer may be stale as soon
  /// as it is returned.
  ///
  /// @return The number of callers inside.
  auto inside() const -> std::size_t {
    const platform::Lock lock{mutex_};
    return inside_;
  }

  /// @brief Counts the callers waiting in line now; the answer may be
  /// stale as soon as it is returned.
  ///
  /// @return The number of callers in line.
  auto waiting() const -> std::size_t {
    const platform::Lock lock{mutex_};
    return queue_.size();
  }

 private:
  /// @brief Frees one place and wakes the line; called by a Pass giving
  /// up its place.
  void leave() noexcept {
    const platform::Lock lock{mutex_};
    --inside_;
    changed_.notify_all();
  }

  /// @brief The most callers let in at once; at least one.
  const std::size_t most_inside_;
  /// @brief The most callers that may wait in line.
  const std::size_t most_waiting_;
  /// @brief Guards the counters and the line.
  mutable platform::Mutex mutex_;
  /// @brief Signalled whenever a place frees up or the line changes.
  platform::ConditionVariable changed_;
  /// @brief How many passes are held now.
  std::size_t inside_{};
  /// @brief The ticket the next caller to join the line receives; only
  /// grows.
  std::uint64_t next_ticket_{};
  /// @brief The tickets of the callers waiting, in arrival order.
  std::deque<std::uint64_t> queue_;
};

}  // namespace libchiaking::concurrency
