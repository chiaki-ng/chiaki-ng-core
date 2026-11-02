#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"

#include "src/core/net/protocol/remote_play/session_codec.hpp"

#include <chrono>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto RemoteSessionInit::finish(rudp::HandshakeChannel& channel,
                               const RudpCtrlHttpExchangeOutcome& outcome,
                               const platform::StopToken& stop)
    -> Expected<void, RemoteSessionInitFailure> {
  auto finish_ack = rudp::FrameCodec::encode_ack_message(
      outcome.next_local_counter, outcome.remote_counter);
  if (!finish_ack) return {};
  for (int finish_attempt = 0; finish_attempt < rudp_finish_attempts;
       ++finish_attempt) {
    if (stop.stop_requested())
      return unexpected(
          RemoteSessionInitFailure{RemoteSessionInitError::stopped});
    auto sent = channel.send(finish_ack.value());
    if (!sent || sent.value() != finish_ack.value().size()) break;
    auto received = channel.receive_for(
        std::chrono::microseconds{rudp::rudp_expect_timeout_us}, stop);
    if (stop.stop_requested())
      return unexpected(
          RemoteSessionInitFailure{RemoteSessionInitError::stopped});
    if (!received) break;
    if (received.value() &&
        (received.value()->size() <= rudp::FrameCodec::header_bytes ||
         rudp::FrameCodec::is_finish_packet(*received.value())))
      break;
  }
  return {};
}

auto RemoteSessionInit::run(rudp::HandshakeChannel& channel,
                            rudp::HandshakeParameters rudp_parameters,
                            platform::ClockProvider& clock,
                            platform::StopToken stop, std::uint64_t attempt,
                            const RegisteredConsole& console,
                            std::string_view host, std::uint16_t port)
    -> Expected<RemoteSessionInitResult, RemoteSessionInitFailure> {
  if (attempt == 0)
    return unexpected(
        RemoteSessionInitFailure{RemoteSessionInitError::invalid_attempt});
  if (stop.stop_requested())
    return unexpected(
        RemoteSessionInitFailure{RemoteSessionInitError::stopped});

  auto target = console.target;
  auto parameters = rudp_parameters;
  for (unsigned attempt_index = 0;; ++attempt_index) {
    if (stop.stop_requested())
      return unexpected(
          RemoteSessionInitFailure{RemoteSessionInitError::stopped});
    auto request = SessionCodec::encode_request(
        target, host, port,
        {console.registration_key.data(), console.registration_key.size()});
    if (!request)
      return unexpected(RemoteSessionInitFailure{
          RemoteSessionInitError::request_failed,
          static_cast<std::uint32_t>(request.error())});

    auto connected = rudp::Connector::connect_over_channel(
        channel, parameters, clock, stop, attempt);
    if (!connected)
      return unexpected(RemoteSessionInitFailure{
          connected.error() == rudp::ConnectError::stopped
              ? RemoteSessionInitError::stopped
              : RemoteSessionInitError::handshake_failed,
          static_cast<std::uint32_t>(connected.error())});

    RudpCtrlHttpExchange exchange{connected.value().next_local_counter,
                                  connected.value().remote_counter,
                                  request.value().bytes(),
                                  channel,
                                  clock,
                                  stop,
                                  kReferenceRudpRetryPolicy,
                                  false};
    auto exchanged = exchange.run();
    if (!exchanged)
      return unexpected(RemoteSessionInitFailure{
          exchanged.error().error == RudpCtrlHttpExchangeError::stopped
              ? RemoteSessionInitError::stopped
              : RemoteSessionInitError::exchange_failed,
          static_cast<std::uint32_t>(exchanged.error().error), 0, 0,
          exchanged.error().received_bytes});
    const auto& outcome = exchanged.value();
    if (auto finished = finish(channel, outcome, stop); !finished)
      return unexpected(finished.error());

    auto parsed = SessionCodec::parse_response(
        target, {outcome.response.received.data(), outcome.received_bytes});
    if (!parsed)
      return unexpected(
          RemoteSessionInitFailure{RemoteSessionInitError::response_failed,
                                   static_cast<std::uint32_t>(parsed.error()),
                                   0, 0, outcome.received_bytes});
    if (!parsed.value()) {
      const auto refusal = attempt_index >= 2 ? parsed.value().error().settled()
                                              : parsed.value().error();
      if (refusal.kind() == SessionRefusal::Kind::version_mismatch &&
          refusal.retry_target()) {
        target = *refusal.retry_target();
        parameters = {outcome.next_local_counter,
                      connected.value().local_header};
        continue;
      }
      return unexpected(RemoteSessionInitFailure{
          RemoteSessionInitError::response_rejected,
          static_cast<std::uint32_t>(refusal.kind()), refusal.http_status(),
          refusal.application_reason(), outcome.received_bytes});
    }
    return RemoteSessionInitResult{
        std::move(parsed.value().value()),
        {connected.value().local_header, outcome.next_local_counter,
         outcome.remote_counter},
        target};
  }
}

}  // namespace libchiaking::net::protocol::remote_play
