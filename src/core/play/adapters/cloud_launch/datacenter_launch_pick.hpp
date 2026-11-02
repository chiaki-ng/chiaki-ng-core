/// @file
/// @brief Declares DatacenterLaunchPick, the hand-off through which a player
/// names the cloud datacenter a launch should use while the launch waits.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief Why a wait for the player's datacenter choice ended without one.
enum class LaunchPickMiss : std::uint8_t {
  /// @brief The wait ran out with no submission; the launch picks for itself.
  timed_out,
  /// @brief The stop token fired before a choice arrived; the pick is closed.
  cancelled,
};

/// @brief The window in which a cloud launch offers datacenters and waits for
/// the player to name one.
///
/// A launch opens the pick with the names it measured, then waits in
/// DatacenterLaunchPick::await. A frontend thread submits a choice with
/// DatacenterLaunchPick::submit; the launch thread takes it. Only an offered
/// name, or DatacenterLaunchPick::automatic, is accepted, and only while the
/// pick is open. Taking or closing the choice closes the pick and forgets the
/// offered names, so a later submission is refused until the next
/// DatacenterLaunchPick::open.
///
/// Every member locks one internal mutex, so the pick may be shared between
/// the launch thread and any number of submitting threads. It is owned by the
/// CloudLaunch::Core that offers it and is neither copyable nor movable.
class DatacenterLaunchPick {
 public:
  /// @enum Submit
  /// @brief The answer to a player's submission.
  ///
  /// @var Submit accepted
  /// @brief The name was held; the waiting launch takes it on its next look.
  ///
  /// @var Submit not_waiting
  /// @brief No offer is open: none was made yet, or the launch already took a
  /// choice, gave up or was cancelled.
  ///
  /// @var Submit not_offered
  /// @brief The name is neither one of the offered datacenters nor
  /// DatacenterLaunchPick::automatic.
  enum class Submit : std::uint8_t { accepted, not_waiting, not_offered };

  /// @brief Makes a closed pick.
  ///
  /// @param threading Provider of the mutex guarding the pick; must outlive
  ///     the pick.
  explicit DatacenterLaunchPick(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Opens the pick with the datacenters the player may choose from.
  ///
  /// Replaces any earlier offer and drops a choice that was not yet taken.
  /// Locks the internal mutex briefly; never waits otherwise.
  ///
  /// @param names The offered datacenter names, compared exactly against later
  ///     submissions.
  void open(std::vector<std::string> names) {
    platform::Lock lock(mutex_);
    names_ = std::move(names);
    choice_.reset();
    open_ = true;
  }

  /// @brief Holds the player's choice for the waiting launch.
  ///
  /// A later accepted submission replaces an earlier one that was not yet
  /// taken. Safe to call from any thread; locks the internal mutex briefly.
  ///
  /// @param name An offered datacenter name, or
  ///     DatacenterLaunchPick::automatic.
  /// @return Submit::accepted when the choice is held; Submit::not_waiting when
  ///     no offer is open; Submit::not_offered when the name was not offered.
  [[nodiscard]] auto submit(std::string_view name) -> Submit {
    platform::Lock lock(mutex_);
    if (!open_) return Submit::not_waiting;
    if (name != automatic &&
        std::find(names_.begin(), names_.end(), name) == names_.end())
      return Submit::not_offered;
    choice_.emplace(name);
    return Submit::accepted;
  }

  /// @brief Takes the held choice and closes the pick when there is one.
  ///
  /// Locks the internal mutex briefly.
  ///
  /// @return The submitted name, after which the pick is closed and the offered
  ///     names forgotten; empty, with the pick left open, when nothing was
  ///     submitted yet.
  [[nodiscard]] auto take() -> std::optional<std::string> {
    platform::Lock lock(mutex_);
    if (!choice_) return std::nullopt;
    open_ = false;
    names_.clear();
    return std::exchange(choice_, std::nullopt);
  }

  /// @brief Closes the pick, whether or not a choice is held.
  ///
  /// Locks the internal mutex briefly.
  ///
  /// @return The choice that was held but not yet taken, so a submission that
  ///     raced the close is not lost; empty when there was none.
  auto close() -> std::optional<std::string> {
    platform::Lock lock(mutex_);
    open_ = false;
    names_.clear();
    return std::exchange(choice_, std::nullopt);
  }

  /// @brief Reports whether an offer is open for submissions.
  ///
  /// @return `true` between DatacenterLaunchPick::open and the take or close
  ///     that ends the offer.
  auto waiting() const -> bool {
    platform::Lock lock(mutex_);
    return open_;
  }

  /// @brief The name a player submits to ask for the lowest-latency datacenter
  /// instead of a named one; the same text as DatacenterName::automatic.
  static constexpr std::string_view automatic = DatacenterName::automatic;

  /// @brief Blocks the calling launch thread until the player chooses, the time
  /// runs out or the launch is stopped.
  ///
  /// Checks the stop token, then for a held choice, then the deadline, and
  /// otherwise sleeps through @p wait_for for the time remaining; a submitting
  /// thread shortens that sleep by waking the waiter. When the deadline passes,
  /// a choice that arrived at the last moment still wins. Cancellation and the
  /// deadline close the pick. Only one thread may wait at a time.
  ///
  /// @tparam NowMicroseconds A callable taking nothing and returning the
  /// current
  ///     monotonic time in microseconds as an unsigned integer.
  /// @tparam WaitFor A callable taking a std::chrono::microseconds remaining
  /// and
  ///     returning after that long at most, or sooner when woken.
  /// @param timeout How long the player has to choose, counted from the call.
  /// @param stop Ends the wait early with Kind::cancelled.
  /// @param now_us The monotonic clock the deadline is measured against.
  /// @param wait_for How the thread idles between looks.
  /// @return The submitted datacenter name, exactly as the player gave it, or
  ///     DatacenterLaunchPick::automatic for "pick the lowest latency"; a name
  ///     submitted just as the wait ran out still counts. Blocks for at most
  ///     @p timeout plus one sleep's overrun.
  /// @retval LaunchPickMiss::timed_out The time ran out with nothing
  ///     submitted.
  /// @retval LaunchPickMiss::cancelled @p stop fired first.
  template <class NowMicroseconds, class WaitFor>
  [[nodiscard]] auto await(std::chrono::milliseconds timeout,
                           const platform::StopToken& stop,
                           NowMicroseconds now_us, WaitFor wait_for)
      -> Expected<std::string, LaunchPickMiss> {
    const auto budget = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
    const std::uint64_t deadline = now_us() + budget;
    for (;;) {
      if (stop.stop_requested()) {
        static_cast<void>(close());
        return unexpected(LaunchPickMiss::cancelled);
      }
      if (auto name = take()) return std::move(*name);
      const std::uint64_t now = now_us();
      if (now >= deadline) {
        if (auto late = close()) return std::move(*late);
        return unexpected(LaunchPickMiss::timed_out);
      }
      wait_for(std::chrono::microseconds{deadline - now});
    }
  }

 private:
  /// @brief Guards every field below; mutable so DatacenterLaunchPick::waiting
  /// can lock it.
  mutable platform::Mutex mutex_;
  /// @brief The datacenter names of the open offer; empty while closed.
  std::vector<std::string> names_;
  /// @brief The accepted submission not yet taken by the launch.
  std::optional<std::string> choice_;
  /// @brief Whether an offer is open; set by DatacenterLaunchPick::open,
  /// cleared by a take or close.
  bool open_{};
};

}  // namespace libchiaking::play
