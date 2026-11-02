/// @file
/// @brief Declares RemoteSessionInit, the session request sent to a console
/// over RUDP when it is reached through PSN rather than on the home network.
///
/// Over the internet the `sess/init` request travels inside RUDP session
/// messages on a hole-punched UDP route. Each attempt runs the RUDP handshake,
/// the request and a closing acknowledgement; a version mismatch retries with
/// the console's stated version, at most twice.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"

#include <cstdint>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

/// @brief Which step of the remote session request failed.
enum class RemoteSessionInitError : std::uint8_t {
  /// @brief The attempt id is 0.
  invalid_attempt,
  /// @brief The stop token fired.
  stopped,
  /// @brief The request could not be encoded; the cause is a
  /// RequestCodecError.
  request_failed,
  /// @brief The RUDP handshake failed; the cause is a rudp::ConnectError.
  handshake_failed,
  /// @brief The request/reply exchange failed; the cause is a
  /// RudpCtrlHttpExchangeError.
  exchange_failed,
  /// @brief The reply could not be read; the cause is a ResponseCodecError.
  response_failed,
  /// @brief The console refused; the cause is a SessionRefusal::Kind.
  response_rejected,
};

/// @brief A failed remote session request, with what the console said.
struct RemoteSessionInitFailure {
  /// @brief The failing step.
  RemoteSessionInitError kind;
  /// @brief The step's own error code, as a number (see
  /// RemoteSessionInitError).
  std::uint32_t cause{};
  /// @brief The reply's HTTP status, when one was read.
  std::uint16_t http_status{};
  /// @brief The reply's `RP-Application-Reason`, when one was read.
  std::uint32_t protocol_reason{};
  /// @brief How many reply bytes had arrived.
  std::size_t received_bytes{};
};

/// @brief An accepted remote session request.
struct RemoteSessionInitResult {
  /// @brief The console's grant, with its nonce.
  SessionGrant response;
  /// @brief The RUDP header and counters to continue on.
  rudp::HandshakeCompletion completion;
  /// @brief The variant that was accepted, which may differ from the stored one
  /// after a version retry.
  RegistrationTarget target{};
};

/// @brief Runs the session request through PSN over RUDP.
///
/// Stateless; every member is static.
class RemoteSessionInit {
 public:
  /// @brief Sends the session request to a registered console over RUDP and
  /// returns its reply.
  ///
  /// Blocks the calling thread for the whole exchange: per attempt, the RUDP
  /// handshake, up to three sends of the request each waiting up to 1 s, and up
  /// to three closing acknowledgements each waiting up to 1 s. A version
  /// mismatch with a known retry target starts another attempt (at most three
  /// in all) from the counters reached.
  ///
  /// @param channel The RUDP datagram channel to the console.
  /// @param parameters The RUDP counter and header to start from.
  /// @param clock The monotonic clock.
  /// @param stop Cancels the exchange between steps.
  /// @param attempt A nonzero attempt id.
  /// @param console The registered console, with its registration key and
  ///     variant.
  /// @param host The host for the `Host` header.
  /// @param port The port for the `Host` header.
  /// @return The accepted reply.
  /// @retval RemoteSessionInitError Any failing step; see
  ///     RemoteSessionInitFailure.
  [[nodiscard]] static auto run(rudp::HandshakeChannel& channel,
                                rudp::HandshakeParameters parameters,
                                platform::ClockProvider& clock,
                                platform::StopToken stop, std::uint64_t attempt,
                                const RegisteredConsole& console,
                                std::string_view host, std::uint16_t port)
      -> Expected<RemoteSessionInitResult, RemoteSessionInitFailure>;

 private:
  /// @brief Sends the closing acknowledgement of an exchange up to three times,
  /// stopping at a finish packet or a header-only reply.
  ///
  /// @param channel The RUDP channel.
  /// @param outcome The exchange's counters.
  /// @param stop Cancels the wait.
  /// @return Nothing, also when the acknowledgement was not answered.
  /// @retval RemoteSessionInitError::stopped The stop token fired.
  [[nodiscard]] static auto finish(rudp::HandshakeChannel& channel,
                                   const RudpCtrlHttpExchangeOutcome& outcome,
                                   const platform::StopToken& stop)
      -> Expected<void, RemoteSessionInitFailure>;
};

}  // namespace libchiaking::net::protocol::remote_play
