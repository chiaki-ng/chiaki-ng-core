#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <memory>
#include <utility>

namespace libchiaking::play::internet {

auto RudpPairing::build_request(
    platform::SecureRandomProvider& random, platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    std::span<const std::byte> account_id,
    std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
    std::span<const std::byte> data2, std::string_view local_ip)
    -> Expected<PsnRegistrationRequest, PsnRequestError> {
  if (target != net::protocol::remote_play::RegistrationTarget::ps4_10 &&
      target != net::protocol::remote_play::RegistrationTarget::ps5_1)
    return unexpected(PsnRequestError::invalid_target);
  if (account_id.size() != psn_account_id_size)
    return unexpected(PsnRequestError::invalid_account_id);
  if (custom_data1.size() != rp_key_size ||
      data1.size() != platform::aes128_key_size ||
      data2.size() != rp_nonce_size)
    return unexpected(PsnRequestError::invalid_psn_data);
  if (!net::protocol::remote_play::RegistrationRequestCodec::encode_header(
          target,
          net::protocol::remote_play::RegistrationRequestCodec::
              max_payload_size,
          local_ip))
    return unexpected(PsnRequestError::invalid_host);

  SecretArray<rp_key_size> ambassador;
  if (!random.fill_bytes({ambassador.data(), ambassador.size()}))
    return unexpected(PsnRequestError::random_failure);
  auto material = net::protocol::remote_play::RegistrationMaterial::derive_psn(
      crypto, target, {ambassador.data(), ambassador.size()}, custom_data1,
      data1, data2);
  if (!material) return unexpected(PsnRequestError::material_failure);
  auto payload =
      net::protocol::remote_play::RegistrationRequestCodec::encode_payload(
          crypto, target, material.value(), account_id);
  if (!payload) return unexpected(PsnRequestError::payload_failure);
  auto header =
      net::protocol::remote_play::RegistrationRequestCodec::encode_header(
          target, payload.value().size(), local_ip);
  if (!header) return unexpected(PsnRequestError::invalid_host);
  SecretBytes bytes;
  bytes.reserve(header.value().size() + payload.value().size());
  bytes.append(header.value());
  bytes.append(payload.value().span());
  return PsnRegistrationRequest{std::move(bytes), std::move(material.value())};
}

auto RudpPairing::finalize_response(
    const net::protocol::remote_play::RudpCtrlHttpExchangeOutcome& outcome,
    platform::CryptoProvider& crypto,
    net::protocol::remote_play::RegistrationTarget target,
    const net::protocol::remote_play::RegistrationMaterial& material)
    -> Expected<net::protocol::remote_play::RegisteredConsole,
                PsnResponseError> {
  const auto received = std::span<const std::byte>(outcome.response.received);
  auto header =
      net::protocol::remote_play::RegistrationResponseCodec::parse_header(
          received);
  if (!header) return unexpected(PsnResponseError::invalid_header);
  if (net::HttpStatus{header.value().status} != net::HttpStatus::ok)
    return unexpected(PsnResponseError::refused);
  const auto body_start = header.value().header_size;
  const auto body_size = header.value().content_size;
  if (body_start > received.size() || body_size > received.size() - body_start)
    return unexpected(PsnResponseError::truncated);
  SecretArray<net::protocol::remote_play::max_response_size> plain;
  const auto received_body_size = received.size() - body_start;
  if (received_body_size > plain.size())
    return unexpected(PsnResponseError::too_large);
  auto decrypted = net::protocol::remote_play::RegistrationCipher::transform(
      crypto, target, {material.bright.data(), material.bright.size()},
      {material.ambassador.data(), material.ambassador.size()}, 0,
      net::protocol::remote_play::CipherDirection::decrypt,
      received.subspan(body_start, received_body_size),
      {plain.data(), received_body_size});
  if (!decrypted) return unexpected(PsnResponseError::crypto_failure);
  auto console =
      net::protocol::remote_play::RegistrationResponseCodec::parse_payload(
          target, {plain.data(), received_body_size});
  if (!console) return unexpected(PsnResponseError::invalid_payload);
  return std::move(console.value());
}

RudpPairing::Deadline::Deadline(platform::StopToken parent,
                                const platform::ClockProvider& clock,
                                std::uint64_t deadline) noexcept
    : parent_(std::move(parent)), clock_(clock), deadline_(deadline) {}

bool RudpPairing::Deadline::stop_requested() const noexcept {
  return parent_.stop_requested() || timed_out();
}

bool RudpPairing::Deadline::deadline_expired() const noexcept {
  return parent_.deadline_expired() || timed_out();
}

bool RudpPairing::Deadline::timed_out() const noexcept {
  return !reply_received_.load(std::memory_order_acquire) &&
         !parent_.stop_requested() && clock_.now_microseconds() >= deadline_;
}

void RudpPairing::Deadline::reply_received() noexcept {
  reply_received_.store(true, std::memory_order_release);
}

RudpPairing::RudpPairing(net::transport::rudp::HandshakeChannel& channel,
                         platform::SecureRandomProvider& random,
                         platform::CryptoProvider& crypto,
                         platform::ClockProvider& clock,
                         const diagnostics::Trace* trace) noexcept
    : channel_(channel),
      random_(random),
      crypto_(crypto),
      clock_(clock),
      trace_(trace) {}

auto RudpPairing::pair_unbounded(
    platform::StopToken stop, std::uint64_t attempt,
    net::protocol::remote_play::RegistrationTarget target,
    std::span<const std::byte> account_id,
    std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
    std::span<const std::byte> data2, std::string_view local_ip,
    Deadline& deadline) const
    -> Expected<PsnRemoteRegistrationResult, PsnRemoteRegistrationFailure> {
  if (attempt == 0)
    return unexpected(PsnRemoteRegistrationFailure{
        PsnRemoteRegistrationError::invalid_attempt});
  if (stop.stop_requested())
    return unexpected(
        PsnRemoteRegistrationFailure{PsnRemoteRegistrationError::stopped});
  auto request = diagnostics::observe(
      trace_, diagnostics::Operation::registration_request, [&] {
        return build_request(random_, crypto_, target, account_id, custom_data1,
                             data1, data2, local_ip);
      });
  if (!request)
    return unexpected(PsnRemoteRegistrationFailure{
        PsnRemoteRegistrationError::request_failed,
        static_cast<std::uint32_t>(request.error())});
  auto connected =
      diagnostics::observe(trace_, diagnostics::Operation::rudp_connect, [&] {
        return net::transport::rudp::Connector::connect_over_channel(
            channel_, random_, clock_, stop, attempt);
      });
  if (!connected)
    return unexpected(PsnRemoteRegistrationFailure{
        PsnRemoteRegistrationError::handshake_failed,
        static_cast<std::uint32_t>(connected.error())});
  net::protocol::remote_play::RudpCtrlHttpExchange exchange{
      connected.value().next_local_counter,
      connected.value().remote_counter,
      request.value().bytes,
      channel_,
      clock_,
      stop,
      net::protocol::remote_play::kReferenceRudpRetryPolicy,
      false};
  auto exchanged = exchange.run();
  if (!exchanged)
    return unexpected(PsnRemoteRegistrationFailure{
        PsnRemoteRegistrationError::exchange_failed,
        static_cast<std::uint32_t>(exchanged.error().error), 0, 0,
        exchanged.error().received_bytes});
  const auto& outcome = exchanged.value();
  deadline.reply_received();
  auto finish_ack = net::transport::rudp::FrameCodec::encode_ack_message(
      outcome.next_local_counter, outcome.remote_counter);
  if (finish_ack) {
    for (int finish_attempt = 0;
         finish_attempt < net::protocol::remote_play::finish_attempt_count;
         ++finish_attempt) {
      if (stop.stop_requested())
        return unexpected(
            PsnRemoteRegistrationFailure{PsnRemoteRegistrationError::stopped});
      if (trace_)
        trace_->record(diagnostics::Operation::registration_finish,
                       diagnostics::Outcome::observed,
                       static_cast<unsigned>(finish_attempt));
      auto sent = channel_.send(finish_ack.value());
      if (!sent || sent.value() != finish_ack.value().size()) break;
      auto received = channel_.receive_for(
          net::protocol::remote_play::finish_retry_wait, stop);
      if (stop.stop_requested())
        return unexpected(
            PsnRemoteRegistrationFailure{PsnRemoteRegistrationError::stopped});
      if (!received) break;
      if (received.value() &&
          (received.value()->size() <=
               net::protocol::remote_play::min_finish_response_size ||
           net::transport::rudp::FrameCodec::is_finish_packet(
               *received.value())))
        break;
    }
  }
  auto console =
      finalize_response(outcome, crypto_, target, request.value().material);
  if (!console) {
    PsnRemoteRegistrationFailure failure{
        PsnRemoteRegistrationError::response_failed,
        static_cast<std::uint32_t>(console.error())};
    failure.received_bytes = outcome.response.received.size();
    auto header =
        net::protocol::remote_play::RegistrationResponseCodec::parse_header(
            outcome.response.received);
    if (header) {
      failure.http_status = header.value().status;
      failure.protocol_reason = header.value().application_reason.value_or(0);
    } else {
      const auto status =
          net::protocol::remote_play::RegistrationResponseCodec::scan_status(
              outcome.response.received);
      failure.http_status = status.status;
      failure.protocol_reason = status.application_reason.value_or(0);
    }
    return unexpected(failure);
  }
  return PsnRemoteRegistrationResult{
      std::move(console.value()), connected.value().local_header,
      outcome.next_local_counter, outcome.remote_counter};
}

auto RudpPairing::pair(platform::StopToken stop, std::uint64_t attempt,
                       net::protocol::remote_play::RegistrationTarget target,
                       std::span<const std::byte> account_id,
                       std::span<const std::byte> custom_data1,
                       std::span<const std::byte> data1,
                       std::span<const std::byte> data2,
                       std::string_view local_ip) const
    -> Expected<PsnRemoteRegistrationResult, PsnRemoteRegistrationFailure> {
  const auto now = clock_.now_microseconds();
  const auto deadline = timing::Deadline::after_us(now, registration_limit_us);
  auto bounded = std::make_shared<Deadline>(stop, clock_, deadline);
  auto result =
      pair_unbounded(platform::StopToken{bounded}, attempt, target, account_id,
                     custom_data1, data1, data2, local_ip, *bounded);
  if (!result && bounded->timed_out()) {
    auto failure = result.error();
    failure.cause = static_cast<std::uint32_t>(failure.kind);
    failure.kind = PsnRemoteRegistrationError::timed_out;
    return unexpected(failure);
  }
  return result;
}

}  // namespace libchiaking::play::internet
