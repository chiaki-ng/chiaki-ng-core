/// @file
/// @brief Declares RudpPairing, which registers the client with a console
/// over a hole-punched RUDP channel using PSN-derived secrets, and its
/// failures.
#pragma once
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include "src/core/net/transport/rudp/rudp.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "src/core/net/protocol/remote_play/pairing/response.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/transport/http/status.hpp"

namespace libchiaking::play::internet {

/// @brief Why the registration request could not be built.
enum class PsnRequestError : std::uint8_t {
  /// @brief The target is neither PS4 10 nor PS5 1.
  invalid_target,
  /// @brief The account id has the wrong size.
  invalid_account_id,
  /// @brief A PSN data blob has the wrong size.
  invalid_psn_data,
  /// @brief The local address does not fit the request header.
  invalid_host,
  /// @brief The random provider failed.
  random_failure,
  /// @brief The registration keys could not be derived.
  material_failure,
  /// @brief The encrypted payload could not be built.
  payload_failure,
};

/// @brief A built registration request and the keys that read its reply.
struct PsnRegistrationRequest {
  /// @brief The request: HTTP header then encrypted payload.
  SecretBytes bytes;
  /// @brief The keys derived for this request.
  net::protocol::remote_play::RegistrationMaterial material;
};

/// @brief Why the registration reply could not be read.
enum class PsnResponseError : std::uint8_t {
  /// @brief The exchange failed; not produced by
  /// RudpPairing::finalize_response.
  exchange_failed,
  /// @brief The HTTP header could not be parsed.
  invalid_header,
  /// @brief The console answered with a status other than 200.
  refused,
  /// @brief The body is shorter than the header says.
  truncated,
  /// @brief The body is larger than a registration reply may be.
  too_large,
  /// @brief The body could not be decrypted.
  crypto_failure,
  /// @brief The decrypted body could not be parsed.
  invalid_payload,
};

/// @brief Which step of a registration failed.
enum class PsnRemoteRegistrationError : std::uint8_t {
  /// @brief The attempt was 0.
  invalid_attempt,
  /// @brief The registration was stopped.
  stopped,
  /// @brief The request could not be built.
  request_failed,
  /// @brief The RUDP handshake failed.
  handshake_failed,
  /// @brief The HTTP exchange over RUDP failed.
  exchange_failed,
  /// @brief The reply could not be read.
  response_failed,
  /// @brief No reply came within the registration limit.
  timed_out,
};

/// @brief Why a registration failed. Plain data.
struct PsnRemoteRegistrationFailure {
  /// @brief The failed step.
  PsnRemoteRegistrationError kind{};
  /// @brief The step's own error code; for a timeout, the step that was
  /// running.
  std::uint32_t cause{};
  /// @brief The reply's HTTP status, when one was read.
  std::uint16_t http_status{};
  /// @brief The reply's PS application reason; 0 for none.
  std::uint32_t protocol_reason{};
  /// @brief How many reply bytes arrived, in bytes.
  std::uint64_t received_bytes{};
  /// @brief Compares the failed step.
  ///
  /// @param other The step.
  /// @return `true` when it failed at @p other.
  constexpr bool operator==(PsnRemoteRegistrationError other) const noexcept {
    return kind == other;
  }
};

/// @brief A completed registration and where the RUDP channel stands.
/// Plain data.
struct PsnRemoteRegistrationResult {
  /// @brief The registered console.
  net::protocol::remote_play::RegisteredConsole console;
  /// @brief The RUDP local header.
  std::uint32_t local_header{};
  /// @brief The next local RUDP counter.
  std::uint16_t next_local_counter{};
  /// @brief The console's RUDP counter.
  std::uint16_t remote_counter{};
};

/// @brief Registers the client with a console over a hole-punched RUDP
/// channel: builds the request, connects RUDP, exchanges the HTTP request and
/// reads the reply.
///
/// Borrows the channel and providers, which must outlive it. A registration
/// blocks on the channel for up to 10 s waiting for the reply. Use on one
/// thread at a time.
class RudpPairing {
 public:
  /// @brief Builds the pairing.
  ///
  /// @param channel The RUDP channel; must outlive it.
  /// @param random Draws the request's random key.
  /// @param crypto Derives keys and encrypts.
  /// @param clock The monotonic clock.
  /// @param trace Records the steps; may be null.
  RudpPairing(net::transport::rudp::HandshakeChannel& channel,
              platform::SecureRandomProvider& random,
              platform::CryptoProvider& crypto, platform::ClockProvider& clock,
              const diagnostics::Trace* trace = nullptr) noexcept;

  /// @brief Registers the client; blocks until the reply, a failure, a stop
  /// or 10 s without a reply.
  ///
  /// After the reply it acknowledges it, retrying a few times until the
  /// console finishes.
  ///
  /// @param stop Stops the registration.
  /// @param attempt The RUDP attempt; not 0.
  /// @param target The firmware target; PS4 10 or PS5 1.
  /// @param account_id The account id.
  /// @param custom_data1 The PSN custom data, a Remote Play key long.
  /// @param data1 The first secret, an AES-128 key long.
  /// @param data2 The second secret, a Remote Play nonce long.
  /// @param local_ip The local address sent in the header.
  /// @return The registered console and channel counters; the failure,
  ///     PsnRemoteRegistrationError::timed_out when no reply came in time.
  [[nodiscard]] auto pair(platform::StopToken stop, std::uint64_t attempt,
                          net::protocol::remote_play::RegistrationTarget target,
                          std::span<const std::byte> account_id,
                          std::span<const std::byte> custom_data1,
                          std::span<const std::byte> data1,
                          std::span<const std::byte> data2,
                          std::string_view local_ip) const
      -> Expected<PsnRemoteRegistrationResult, PsnRemoteRegistrationFailure>;

  /// @brief Builds the registration request.
  ///
  /// @param random Draws the request's random key.
  /// @param crypto Derives keys and encrypts.
  /// @param target The firmware target; PS4 10 or PS5 1.
  /// @param account_id The account id.
  /// @param custom_data1 The PSN custom data, a Remote Play key long.
  /// @param data1 The first secret, an AES-128 key long.
  /// @param data2 The second secret, a Remote Play nonce long.
  /// @param local_ip The local address sent in the header.
  /// @return The request; the reason it could not be built.
  [[nodiscard]] static auto build_request(
      platform::SecureRandomProvider& random, platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      std::span<const std::byte> account_id,
      std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
      std::span<const std::byte> data2, std::string_view local_ip)
      -> Expected<PsnRegistrationRequest, PsnRequestError>;

  /// @brief Reads the console's registration reply.
  ///
  /// @param outcome The HTTP exchange's result.
  /// @param crypto Decrypts the body.
  /// @param target The firmware target.
  /// @param material The request's keys.
  /// @return The registered console; the reason the reply could not be read.
  [[nodiscard]] static auto finalize_response(
      const net::protocol::remote_play::RudpCtrlHttpExchangeOutcome& outcome,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      const net::protocol::remote_play::RegistrationMaterial& material)
      -> Expected<net::protocol::remote_play::RegisteredConsole,
                  PsnResponseError>;

 private:
  /// @brief How long a reply may take, in microseconds.
  static constexpr std::uint64_t registration_limit_us = 10'000'000;

  /// @brief A stop that also fires when no reply came by a deadline; once a
  /// reply arrived, only the parent stops it. Thread-safe.
  class Deadline final : public platform::StopState {
   public:
    /// @brief Builds the stop.
    ///
    /// @param parent The caller's stop.
    /// @param clock The monotonic clock; must outlive the stop.
    /// @param deadline The deadline, in microseconds of @p clock.
    Deadline(platform::StopToken parent, const platform::ClockProvider& clock,
             std::uint64_t deadline) noexcept;
    /// @brief Reports whether the parent stopped or the deadline passed.
    ///
    /// @return `true` when it should stop.
    bool stop_requested() const noexcept override;
    /// @brief Reports whether the parent's deadline or this one passed.
    ///
    /// @return `true` when either passed.
    bool deadline_expired() const noexcept override;
    /// @brief Reports whether this deadline passed without a reply or a
    /// parent stop.
    ///
    /// @return `true` when it timed out.
    bool timed_out() const noexcept;
    /// @brief Records that the reply arrived, disarming the deadline.
    void reply_received() noexcept;

   private:
    /// @brief The caller's stop.
    platform::StopToken parent_;
    /// @brief The monotonic clock.
    const platform::ClockProvider& clock_;
    /// @brief The deadline, in microseconds.
    std::uint64_t deadline_;
    /// @brief Whether the reply arrived.
    std::atomic<bool> reply_received_{false};
  };

  /// @brief Registers the client under a bounded stop.
  ///
  /// @param stop The bounded stop.
  /// @param attempt The RUDP attempt.
  /// @param target The firmware target.
  /// @param account_id The account id.
  /// @param custom_data1 The PSN custom data.
  /// @param data1 The first secret.
  /// @param data2 The second secret.
  /// @param local_ip The local address.
  /// @param deadline Told when the reply arrives.
  /// @return As RudpPairing::pair, without the timeout.
  [[nodiscard]] auto pair_unbounded(
      platform::StopToken stop, std::uint64_t attempt,
      net::protocol::remote_play::RegistrationTarget target,
      std::span<const std::byte> account_id,
      std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
      std::span<const std::byte> data2, std::string_view local_ip,
      Deadline& deadline) const
      -> Expected<PsnRemoteRegistrationResult, PsnRemoteRegistrationFailure>;

  /// @brief The RUDP channel.
  net::transport::rudp::HandshakeChannel& channel_;
  /// @brief Draws random keys.
  platform::SecureRandomProvider& random_;
  /// @brief Derives keys and encrypts.
  platform::CryptoProvider& crypto_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Records the steps; may be null.
  const diagnostics::Trace* trace_;
};

}  // namespace libchiaking::play::internet
