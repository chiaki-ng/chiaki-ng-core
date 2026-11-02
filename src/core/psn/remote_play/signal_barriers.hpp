/// @file
/// @brief Declares the barriers a Remote Play internet start waits at: the
/// session is created (CreationBarrier) and the console is ready
/// (ConsoleReadyBarrier), each completed by two push notifications that may
/// arrive in either order.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/psn/rest/push/push.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace libchiaking::psn::remote_play {

/// @brief Why a wait at a barrier ended without its value.
enum class CreationWaitError : std::uint8_t {
  /// @brief The wait was for another start attempt than the barrier's, or
  /// attempt 0.
  stale_attempt,
  /// @brief The deadline passed.
  timed_out,
  /// @brief The stop token fired.
  cancelled,
  /// @brief The push channel failed.
  receive_failed,
  /// @brief A notification the barrier needed was unreadable or inconsistent.
  invalid_notification,
  /// @brief Too many notifications arrived; not produced at present.
  too_many_notifications,
  /// @brief The barrier had already been abandoned or failed.
  failed,
};

/// @brief An observer that does nothing after each notification.
struct NoNotificationObservation {
  /// @brief Observes nothing.
  ///
  /// @return Success.
  auto operator()() const -> Expected<void, CreationWaitError> { return {}; }
};

/// @brief What a ready console reported. Plain data.
struct ConsoleReady {
  /// @brief The console's device unique id, from the member that joined.
  std::array<std::byte, psn_device_uid_size> device_uid{};
  /// @brief The session's customData1 as the console set it.
  std::array<std::byte, psn_custom_data_size> custom_data1{};
};

/// @brief The two-signal state machine.
class TwoSignalBarrier;

/// @brief A wait for two notifications of one start attempt, completed once
/// both have arrived and failed for good once abandoned.
///
/// It is bound to the attempt number it was made for, so notifications and
/// waits of an earlier attempt are not mistaken for this one's. Not
/// thread-safe: one thread waits.
///
/// @tparam Barrier The derived barrier, which provides `on_notification
///     (attempt, notification)` (returning `Expected<void, Fault>` for its
///     own psn::rest::Fault) and `value()`.
/// @tparam Value What the barrier yields when ready.
template <class Barrier, class Value>
class SignalBarrier {
 public:
  /// @brief Not copyable.
  SignalBarrier(const SignalBarrier&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never.
  SignalBarrier& operator=(const SignalBarrier&) = delete;

  /// @brief Waits until both notifications have arrived.
  ///
  /// Each notification `next` gives is handed to the barrier; after each, the
  /// barrier's state is checked and `observe` is called. A timeout or
  /// cancellation abandons the barrier.
  ///
  /// @tparam Next Callable as `next()` returning
  ///     `Expected<psn::rest::Notification, CreationWaitError>`; it may
  ///     block.
  /// @tparam Observe Callable as `observe()` returning
  ///     `Expected<void, CreationWaitError>`.
  /// @param attempt The start attempt waiting.
  /// @param next Gives the next notification.
  /// @param observe Called after each notification that left the barrier
  ///     unfinished or ready.
  /// @return The barrier's value.
  /// @retval CreationWaitError::stale_attempt `attempt` is 0 or not the
  ///     barrier's.
  /// @retval CreationWaitError::failed The barrier failed or was abandoned.
  /// @retval CreationWaitError::invalid_notification A notification was
  ///     refused; the barrier is failed.
  /// @retval CreationWaitError The failure `next` or `observe` gave.
  template <class Next, class Observe = NoNotificationObservation>
  [[nodiscard]] auto wait(std::uint64_t attempt, Next&& next,
                          Observe observe = {})
      -> Expected<Value, CreationWaitError> {
    auto& self = static_cast<Barrier&>(*this);
    if (attempt == 0 || attempt != attempt_)
      return unexpected(CreationWaitError::stale_attempt);
    if (failed()) return unexpected(CreationWaitError::failed);
    if (ready()) return self.value();
    for (;;) {
      auto notification = next();
      if (!notification) {
        if (notification.error() == CreationWaitError::timed_out ||
            notification.error() == CreationWaitError::cancelled)
          abandon(attempt);
        return unexpected(notification.error());
      }
      if (!self.on_notification(attempt, notification.value()))
        return unexpected(CreationWaitError::invalid_notification);
      if (failed()) return unexpected(CreationWaitError::failed);
      auto observed = observe();
      if (!observed) return unexpected(observed.error());
      if (ready()) return self.value();
    }
  }

  /// @brief Abandons the barrier because its attempt timed out.
  ///
  /// @param attempt The attempt; ignored when not the barrier's or already
  ///     settled.
  auto on_timeout(std::uint64_t attempt) -> void { abandon(attempt); }
  /// @brief Abandons the barrier because its attempt was cancelled.
  ///
  /// @param attempt The attempt; ignored when not the barrier's or already
  ///     settled.
  auto on_cancel(std::uint64_t attempt) -> void { abandon(attempt); }
  /// @brief Tells whether both notifications arrived.
  ///
  /// @return True once ready.
  auto ready() const noexcept -> bool;
  /// @brief Tells whether the barrier was abandoned or failed.
  ///
  /// @return True once failed.
  auto failed() const noexcept -> bool;
  /// @brief Reads the attempt the barrier is for.
  ///
  /// @return The attempt number.
  auto attempt() const noexcept -> std::uint64_t { return attempt_; }

 protected:
  /// @brief Makes an empty barrier.
  ///
  /// @param attempt The start attempt it is for; 0 is never valid.
  explicit SignalBarrier(std::uint64_t attempt);
  /// @brief Destroys the state machine.
  ~SignalBarrier();

  /// @brief Tells whether notifications for an attempt can still change the
  /// barrier.
  ///
  /// @param attempt The attempt.
  /// @return True when the barrier is ready or failed, or is for another
  ///     attempt.
  auto settled_for(std::uint64_t attempt) const noexcept -> bool {
    return failed() || ready() || attempt != attempt_;
  }
  /// @brief Records the first notification.
  void first();
  /// @brief Records the second notification.
  void second();
  /// @brief Fails the barrier, unless it is ready.
  void stop();

 private:
  /// @brief Fails the barrier for its own attempt if still unsettled.
  ///
  /// @param attempt The attempt.
  void abandon(std::uint64_t attempt) {
    if (!settled_for(attempt)) stop();
  }

  /// @brief The attempt the barrier is for.
  std::uint64_t attempt_;
  /// @brief The two-signal state machine; never null.
  std::unique_ptr<TwoSignalBarrier> machine_;
};

/// @brief Waits for a new Remote Play session to be fully created: the
/// session-created notification (naming the user's online id) and a
/// members-created notification.
class CreationBarrier : public SignalBarrier<CreationBarrier, std::string> {
 public:
  /// @brief Makes the barrier.
  ///
  /// @param attempt The start attempt.
  explicit CreationBarrier(std::uint64_t attempt);

  /// @brief Takes in a notification.
  ///
  /// Other types, and anything once settled or for another attempt, are
  /// ignored. A second session-created notification must name the same online
  /// id.
  ///
  /// @param attempt The attempt it arrived for.
  /// @param notification The notification.
  /// @return Nothing, when it was taken in or ignored.
  /// @retval psn::rest::Notification::OnlineIdFault The online id was
  ///     unreadable, or differed (bad_reply::InvalidOnlineId); the barrier is
  ///     failed.
  [[nodiscard]] auto on_notification(
      std::uint64_t attempt, const psn::rest::Notification& notification)
      -> Expected<void, psn::rest::Notification::OnlineIdFault>;

  /// @brief Reads the online id the session was created for.
  ///
  /// @return It; empty until the session-created notification.
  auto online_id() const noexcept -> const std::string& { return online_id_; }
  /// @brief Gives the barrier's value once ready.
  ///
  /// @return The online id.
  auto value() const -> std::string { return online_id_; }

 private:
  /// @brief The online id from the session-created notification.
  std::string online_id_;
};

/// @brief Waits for the console to be ready to holepunch: a members-created
/// notification naming its device unique id, and a customData1-updated
/// notification carrying the data it set.
class ConsoleReadyBarrier
    : public SignalBarrier<ConsoleReadyBarrier, ConsoleReady> {
 public:
  /// @brief A device unique id.
  using DeviceUid = std::array<std::byte, psn_device_uid_size>;
  /// @brief A customData1 block.
  using CustomData = std::array<std::byte, psn_custom_data_size>;
  /// @brief The fault of on_notification().
  using ReadyFault = psn::rest::Fault<
      psn::rest::bad_reply::TooLarge, psn::rest::bad_reply::TooDeep,
      psn::rest::bad_reply::InvalidJson, psn::rest::bad_reply::InvalidSchema,
      psn::rest::bad_reply::InvalidDeviceUid,
      psn::rest::bad_reply::InvalidCustomData1>;

  /// @brief Makes the barrier.
  ///
  /// @param attempt The start attempt.
  /// @param expected_uid The console expected, when known; another console
  ///     joining fails the barrier.
  ConsoleReadyBarrier(std::uint64_t attempt,
                      std::optional<DeviceUid> expected_uid);

  /// @brief Takes in a notification.
  ///
  /// Other types, and anything once settled or for another attempt, are
  /// ignored; a later value replaces an earlier one.
  ///
  /// @param attempt The attempt it arrived for.
  /// @param notification The notification.
  /// @return Nothing, when it was taken in or ignored.
  /// @retval ReadyFault The device id or customData1 was unreadable, or
  ///     another console joined (bad_reply::InvalidDeviceUid); the barrier
  ///     is failed.
  [[nodiscard]] auto on_notification(
      std::uint64_t attempt, const psn::rest::Notification& notification)
      -> Expected<void, ReadyFault>;

  /// @brief Reads the console's device id.
  ///
  /// @return It, or nothing yet.
  auto device_uid() const noexcept -> const std::optional<DeviceUid>& {
    return device_uid_;
  }
  /// @brief Reads the console's customData1.
  ///
  /// @return It, or nothing yet.
  auto custom_data1() const noexcept -> const std::optional<CustomData>& {
    return custom_data1_;
  }
  /// @brief Gives the barrier's value; both parts must have arrived.
  ///
  /// @return The device id and customData1.
  auto value() const -> ConsoleReady { return {*device_uid_, *custom_data1_}; }

 private:
  /// @brief Fails the barrier.
  ///
  /// @param error Why.
  /// @return The failure.
  auto fail(ReadyFault error) -> Unexpected<ReadyFault>;
  /// @brief Signals each part that has arrived.
  void update_readiness();

  /// @brief The console expected, when known.
  std::optional<DeviceUid> expected_uid_;
  /// @brief The console's device id, once a member joined.
  std::optional<DeviceUid> device_uid_;
  /// @brief The customData1, once updated.
  std::optional<CustomData> custom_data1_;
};

/// @brief Instantiated in signal_barriers.cpp.
extern template class SignalBarrier<CreationBarrier, std::string>;
/// @brief Instantiated in signal_barriers.cpp.
extern template class SignalBarrier<ConsoleReadyBarrier, ConsoleReady>;

}  // namespace libchiaking::psn::remote_play
