#include "src/core/psn/remote_play/signal_barriers.hpp"

#include "src/core/psn/remote_play/two_signal_barrier.hpp"

#include <utility>

namespace libchiaking::psn::remote_play {

template <class Barrier, class Value>
SignalBarrier<Barrier, Value>::SignalBarrier(std::uint64_t attempt)
    : attempt_(attempt), machine_(std::make_unique<TwoSignalBarrier>()) {}

template <class Barrier, class Value>
SignalBarrier<Barrier, Value>::~SignalBarrier() = default;

template <class Barrier, class Value>
auto SignalBarrier<Barrier, Value>::ready() const noexcept -> bool {
  return machine_->ready();
}

template <class Barrier, class Value>
auto SignalBarrier<Barrier, Value>::failed() const noexcept -> bool {
  return machine_->failed();
}

template <class Barrier, class Value>
void SignalBarrier<Barrier, Value>::first() {
  machine_->first();
}

template <class Barrier, class Value>
void SignalBarrier<Barrier, Value>::second() {
  machine_->second();
}

template <class Barrier, class Value>
void SignalBarrier<Barrier, Value>::stop() {
  machine_->stop();
}

template class SignalBarrier<CreationBarrier, std::string>;
template class SignalBarrier<ConsoleReadyBarrier, ConsoleReady>;

CreationBarrier::CreationBarrier(std::uint64_t attempt)
    : SignalBarrier(attempt) {}

auto CreationBarrier::on_notification(
    std::uint64_t attempt, const psn::rest::Notification& notification)
    -> Expected<void, psn::rest::Notification::OnlineIdFault> {
  if (settled_for(attempt)) return {};
  if (notification.type() == psn::rest::NotificationType::members_created) {
    second();
    return {};
  }
  if (notification.type() !=
      psn::rest::NotificationType::remote_play_session_created)
    return {};
  auto decoded = notification.session_created_online_id();
  if (!decoded) {
    stop();
    return unexpected(psn::rest::Notification::OnlineIdFault{decoded.error()});
  }
  if (!online_id_.empty() && online_id_ != decoded.value()) {
    stop();
    return unexpected(psn::rest::Notification::OnlineIdFault{
        psn::rest::bad_reply::InvalidOnlineId{}});
  }
  online_id_ = std::move(decoded.value());
  first();
  return {};
}

ConsoleReadyBarrier::ConsoleReadyBarrier(std::uint64_t attempt,
                                         std::optional<DeviceUid> expected_uid)
    : SignalBarrier(attempt), expected_uid_(std::move(expected_uid)) {}

auto ConsoleReadyBarrier::fail(ReadyFault error) -> Unexpected<ReadyFault> {
  stop();
  return unexpected(error);
}

void ConsoleReadyBarrier::update_readiness() {
  if (device_uid_) first();
  if (custom_data1_) second();
}

auto ConsoleReadyBarrier::on_notification(
    std::uint64_t attempt, const psn::rest::Notification& notification)
    -> Expected<void, ReadyFault> {
  if (settled_for(attempt)) return {};
  if (notification.type() == psn::rest::NotificationType::members_created) {
    auto decoded = notification.member_device_uid();
    if (!decoded) return fail(ReadyFault{decoded.error()});
    if (expected_uid_ && decoded.value() != *expected_uid_)
      return fail(ReadyFault{psn::rest::bad_reply::InvalidDeviceUid{}});
    device_uid_ = decoded.value();
    update_readiness();
    return {};
  }
  if (notification.type() ==
      psn::rest::NotificationType::custom_data1_updated) {
    auto decoded = notification.custom_data1();
    if (!decoded) return fail(ReadyFault{decoded.error()});
    custom_data1_ = decoded.value();
    update_readiness();
  }
  return {};
}

}  // namespace libchiaking::psn::remote_play
