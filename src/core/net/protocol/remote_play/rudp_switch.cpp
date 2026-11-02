#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

RemoteDataRouteGate::~RemoteDataRouteGate() { close_owned_route(); }

bool RemoteDataRouteGate::arm(EstablishedRoute route,
                              std::uint64_t attempt_id) {
  if (!route.transport || route.kind != RouteKind::remote_rudp ||
      route_.has_value() || !barrier_.begin(attempt_id))
    return false;
  route_.emplace(std::move(route));
  close_error_.reset();
  return true;
}

bool RemoteDataRouteGate::on_ack(ctrl_lifecycle_detail::PublishSwitchAck ack) {
  return barrier_.on_ack(ack.attempt_id);
}

bool RemoteDataRouteGate::timeout(std::uint64_t attempt_id) {
  if (!barrier_.timeout(attempt_id)) return false;
  close_owned_route();
  return true;
}

bool RemoteDataRouteGate::cancel(std::uint64_t attempt_id) {
  if (!barrier_.cancel(attempt_id)) return false;
  close_owned_route();
  return true;
}

std::optional<EstablishedRoute> RemoteDataRouteGate::take_route() {
  if (!route_.has_value() || !barrier_.take_ready()) return std::nullopt;
  std::optional<EstablishedRoute> out{std::move(*route_)};
  route_.reset();
  return out;
}

void RemoteDataRouteGate::close_owned_route() {
  if (route_)
    if (auto result = route_->transport.close(); !result)
      close_error_ = result.error();
  route_.reset();
}

void RudpSwitchRequestRunner::report(platform::DiagnosticsProvider* diagnostics,
                                     std::string_view message) {
  if (diagnostics) diagnostics->write({platform::LogSeverity::error, message});
}

auto RudpSwitchRequestRunner::barrier_error(
    RudpCtrlSwitchStepError error) noexcept
    -> std::optional<RudpSwitchBarrierError> {
  switch (error) {
    case RudpCtrlSwitchStepError::timeout:
      return std::nullopt;
    case RudpCtrlSwitchStepError::malformed:
      return RudpSwitchBarrierError::malformed;
    case RudpCtrlSwitchStepError::stopped:
      return RudpSwitchBarrierError::stopped;
    case RudpCtrlSwitchStepError::peer_closed:
    case RudpCtrlSwitchStepError::receive_failed:
      return RudpSwitchBarrierError::receive_failed;
    case RudpCtrlSwitchStepError::crypto_failed:
      return RudpSwitchBarrierError::crypto_failed;
    case RudpCtrlSwitchStepError::counter_exhausted:
      return RudpSwitchBarrierError::counter_exhausted;
    case RudpCtrlSwitchStepError::ack_encode_failed:
    case RudpCtrlSwitchStepError::ack_send_failed:
    case RudpCtrlSwitchStepError::ack_short_send:
      return RudpSwitchBarrierError::ack_failed;
  }
  return RudpSwitchBarrierError::receive_failed;
}

auto RudpSwitchRequestRunner::run(
    rudp::HandshakeChannel& channel,
    transport_switch_detail::RemoteTransportSwitch& state,
    RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
    const RudpSwitchHooks& hooks)
    -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError> {
  if (services.stop.stop_requested())
    return unexpected(RudpSwitchBarrierError::stopped);
  if (!state.begin(input.attempt_id))
    return unexpected(RudpSwitchBarrierError::invalid_attempt);
  return await_confirmation(channel, state, input, services, hooks);
}

auto RudpSwitchRequestRunner::run_armed(
    rudp::HandshakeChannel& channel,
    transport_switch_detail::RemoteTransportSwitch& state,
    RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
    const RudpSwitchHooks& hooks)
    -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError> {
  if (services.stop.stop_requested())
    return unexpected(RudpSwitchBarrierError::stopped);
  if (!state.awaits(input.attempt_id))
    return unexpected(RudpSwitchBarrierError::invalid_attempt);
  return await_confirmation(channel, state, input, services, hooks);
}

auto RudpSwitchRequestRunner::await_confirmation(
    rudp::HandshakeChannel& channel,
    transport_switch_detail::RemoteTransportSwitch& state,
    RudpCtrlSwitchStepInput input, const RudpSwitchServices& services,
    const RudpSwitchHooks& hooks)
    -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError> {
  const auto& stop = services.stop;
  const auto failed = [&](RudpSwitchBarrierError error)
      -> Expected<RudpSwitchBarrierResult, RudpSwitchBarrierError> {
    static_cast<void>(state.cancel(input.attempt_id));
    report(services.diagnostics,
           std::format("remote RUDP switch: failed; cause={} pending "
                       "confirmation abandoned",
                       std::to_underlying(error)));
    return unexpected(error);
  };
  const auto switch_counter = input.local_counter;
  auto packet = rudp::FrameCodec::encode_switch_request_random(switch_counter,
                                                               services.random);
  if (!packet) return failed(RudpSwitchBarrierError::packet_failed);
  input.local_counter = static_cast<std::uint16_t>(switch_counter + 1U);
  rudp::ReliableChannel reliable{channel, services.pending, services.clock,
                                 services.diagnostics};
  auto sent = reliable.send(packet.value());
  if (!sent) return failed(RudpSwitchBarrierError::send_failed);
  if (sent.value() != packet.value().size())
    return failed(RudpSwitchBarrierError::short_send);
  const auto deadline = timing::Deadline::after_us(
      services.clock.now_microseconds(),
      timing::Deadline::Duration{rudp_switch_timeout}.count());
  RudpCtrlReceiver receiver;
  for (;;) {
    if (stop.stop_requested()) return failed(RudpSwitchBarrierError::stopped);
    const auto now = services.clock.now_microseconds();
    if (now >= deadline) break;
    auto step = receiver.step(
        reliable, input,
        std::chrono::microseconds(static_cast<std::int64_t>(deadline - now)),
        stop);
    if (!step) {
      if (const auto error = barrier_error(step.error())) return failed(*error);
      continue;
    }
    bool notification = false;
    for (auto& message : step.value()) {
      if (message.delivery == RudpCtrlDelivery::acknowledgement) continue;
      input.remote_crypt_counter = message.message.next_remote_crypt_counter;
      if (stop.stop_requested()) return failed(RudpSwitchBarrierError::stopped);
      if (message.delivery == RudpCtrlDelivery::transport) continue;
      notification = notification || message.switch_notification;
      if (message.attempt_id != input.attempt_id ||
          (hooks.accept_message && !hooks.accept_message(std::move(message))))
        return failed(RudpSwitchBarrierError::invalid_attempt);
    }
    if (stop.stop_requested()) return failed(RudpSwitchBarrierError::stopped);
    if (hooks.service_effects) {
      auto next = hooks.service_effects(reliable, input.local_counter);
      if (!next) return failed(RudpSwitchBarrierError::service_failed);
      input.local_counter = *next;
    }
    if (stop.stop_requested()) return failed(RudpSwitchBarrierError::stopped);
    if (notification) {
      if (!state.on_ack(input.attempt_id))
        return failed(RudpSwitchBarrierError::invalid_attempt);
      return RudpSwitchBarrierResult{
          input.attempt_id, input.remote_crypt_counter, input.local_counter};
    }
  }
  static_cast<void>(state.timeout(input.attempt_id));
  report(
      services.diagnostics,
      std::format("remote RUDP switch: timed out after {} ms waiting for Ctrl "
                  "switch confirmation",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      rudp_switch_timeout)
                      .count()));
  return unexpected(RudpSwitchBarrierError::timeout);
}

auto RudpDataSwitch::switch_route(EstablishedRoute data_route,
                                  rudp::HandshakeChannel& ctrl_channel,
                                  RudpCtrlSwitchStepInput input,
                                  const RudpSwitchServices& services,
                                  const RudpSwitchHooks& hooks)
    -> Expected<RudpDataSwitchResult, RudpSwitchBarrierError> {
  RemoteDataRouteGate gate;
  if (!gate.arm(std::move(data_route), input.attempt_id))
    return unexpected(RudpSwitchBarrierError::invalid_attempt);
  auto barrier = RudpSwitchRequestRunner::run_armed(
      ctrl_channel, gate.barrier(), input, services, hooks);
  if (!barrier) return unexpected(barrier.error());
  auto route = gate.take_route();
  if (!route) return unexpected(RudpSwitchBarrierError::invalid_attempt);
  return RudpDataSwitchResult{barrier.value(), std::move(*route)};
}

}  // namespace libchiaking::net::protocol::remote_play
