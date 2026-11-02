/// @file
/// @brief Declares PairingExchange, the TCP half of pairing a console at
/// home: it sends the prepared registration request to the console that
/// answered the knock and decrypts the console's reply.
///
/// The exchange never blocks. Each call does what the socket allows at once
/// and reports how far it got as a Progress, so HomePairing can drive it from
/// its own loop, wait on pollable() and cancel it at any point.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace libchiaking::consoles {

/// @brief One registration request and its reply, exchanged over a
/// non-blocking TCP connection.
///
/// The request is staged first, copied into wiping buffers; begin() then
/// connects, and the send and read calls are made in order, each again
/// until it stops reporting Waiting. Every phase (connecting, sending the
/// header, sending the payload, reading the reply) has its own deadline of
/// net::protocol::remote_play::phase_timeout on the
/// monotonic clock, restarted when the phase before it completes. Any
/// failure, and the decoded reply, close the socket and wipe the request,
/// the reply and the key material; the exchange can then be staged again.
///
/// Not safe to share between threads: one thread drives it. The providers
/// must outlive it.
class PairingExchange {
 public:
  /// @brief Largest request header that can be staged, in bytes.
  static constexpr std::size_t header_limit =
      net::protocol::remote_play::RegistrationRequestCodec::max_header_size;
  /// @brief Largest encrypted request body that can be staged, in bytes.
  static constexpr std::size_t payload_limit =
      net::protocol::remote_play::RegistrationRequestCodec::max_payload_size;
  /// @brief Largest reply accepted, header and body together, in bytes.
  static constexpr std::size_t reply_limit =
      net::protocol::remote_play::max_response_size;

  /// @brief Why an exchange call failed.
  enum class Error : std::uint8_t {
    /// @brief An empty or oversized request, an unknown target, a zero
    /// port, or begin() before anything was staged.
    invalid_argument,
    /// @brief The call does not fit: staging or beginning while connected,
    /// or a send or read with no connection.
    invalid_state,
    /// @brief The socket could not be created, connected, written or read,
    /// or it reported an impossible byte count.
    network_failure,
    /// @brief The current phase's deadline passed.
    timeout,
    /// @brief The console closed the connection before the reply was
    /// complete.
    truncated,
    /// @brief The reply header could not be parsed.
    invalid_header,
    /// @brief The reply header or the whole reply exceeds reply_limit.
    too_large,
    /// @brief The console answered with an HTTP status other than 200;
    /// http_status() and ps_reason() say why.
    refused,
    /// @brief The reply body could not be decrypted.
    crypto_failure,
    /// @brief The decrypted reply body could not be parsed.
    invalid_payload,
  };

  /// @brief Progress: nothing completed yet; call the same step again once
  /// the socket is ready.
  struct Waiting {};
  /// @brief Progress: the connection is open; send the header next.
  struct Connected {};
  /// @brief Progress: the header could not be sent because the connection is
  /// not open after all; wait for the connection again.
  struct ConnectionPending {};
  /// @brief Progress: the header is sent; send the payload next.
  struct HeaderSent {};
  /// @brief Progress: the whole request is sent; read the reply header next.
  struct RequestSent {};
  /// @brief Progress: the reply header said 200 and the body is still
  /// arriving; read the body next.
  struct ReplyHeaderAccepted {};
  /// @brief What a step achieved; a RegisteredConsole means the reply was
  /// decoded and the exchange is over.
  using Progress = std::variant<Waiting, Connected, ConnectionPending,
                                HeaderSent, RequestSent, ReplyHeaderAccepted,
                                net::protocol::remote_play::RegisteredConsole>;

  /// @brief Whether the exchange can pair with a target.
  ///
  /// @param target The registration target.
  /// @return True for every RegistrationTarget value, false for a value
  ///     outside the enumeration.
  static auto known(
      net::protocol::remote_play::RegistrationTarget target) noexcept -> bool;

  /// @brief Makes an idle exchange.
  ///
  /// @param sockets Where the TCP socket comes from; must outlive this
  ///     object.
  /// @param crypto Decrypts the reply; must outlive this object.
  /// @param clock The monotonic clock the deadlines are measured by; must
  ///     outlive this object.
  PairingExchange(platform::SocketProvider& sockets,
                  platform::CryptoProvider& crypto,
                  const platform::ClockProvider& clock) noexcept;
  /// @brief Not copyable: it owns a socket and secrets.
  PairingExchange(const PairingExchange&) = delete;
  /// @brief Not copyable.
  auto operator=(const PairingExchange&) -> PairingExchange& = delete;
  /// @brief Closes the socket and wipes everything, as cancel() does.
  ~PairingExchange();

  /// @brief Copies the encoded request in, ready for begin().
  ///
  /// @param header The HTTP request header.
  /// @param payload The encrypted request body.
  /// @param target The console variant the request was encoded for, which
  ///     selects how the reply is decrypted and parsed.
  /// @return Nothing on success.
  /// @retval Error::invalid_state A connection is open.
  /// @retval Error::invalid_argument A part is empty or over its limit, or
  ///     the target is unknown.
  [[nodiscard]] auto stage(
      std::span<const std::byte> header, std::span<const std::byte> payload,
      net::protocol::remote_play::RegistrationTarget target)
      -> Expected<void, Error>;
  /// @brief Wipes a staged request that was never sent; does nothing while a
  /// connection is open.
  void unstage() noexcept;
  /// @brief Opens a non-blocking TCP connection to the console and starts the
  /// connecting phase's deadline.
  ///
  /// @param console Where the console answered the knock.
  /// @param material The key material the request was encrypted with; it is
  ///     kept to decrypt the reply and wiped with the rest.
  /// @return Connected when the connection opened at once, otherwise Waiting.
  /// @retval Error::invalid_state A connection is already open.
  /// @retval Error::invalid_argument The port is zero or nothing is staged.
  /// @retval Error::network_failure The socket could not be created or the
  ///     connect failed outright; everything is wiped.
  [[nodiscard]] auto begin(
      platform::SocketEndpoint console,
      net::protocol::remote_play::RegistrationMaterial&& material)
      -> Expected<Progress, Error>;
  /// @brief Checks whether the connection has opened.
  ///
  /// @return Connected once the connection is open, which starts the header's
  ///     deadline; Waiting while it is still opening.
  /// @retval Error::invalid_state No connection was begun.
  /// @retval Error::timeout The connecting phase's deadline passed.
  /// @retval Error::network_failure The connection failed.
  [[nodiscard]] auto await_connection() -> Expected<Progress, Error>;
  /// @brief Sends as much of the request header as the socket takes.
  ///
  /// @return HeaderSent once all of it is sent, which wipes the header and
  ///     starts the payload's deadline; Waiting while some remains;
  ///     ConnectionPending when the socket says it is not connected yet,
  ///     which puts the connecting deadline back.
  /// @retval Error::invalid_state No connection is open.
  /// @retval Error::timeout The phase's deadline passed.
  /// @retval Error::network_failure The write failed.
  [[nodiscard]] auto send_header() -> Expected<Progress, Error>;
  /// @brief Sends as much of the encrypted request body as the socket takes.
  ///
  /// @return RequestSent once all of it is sent, which wipes the payload and
  ///     starts the reply's deadline; Waiting while some remains.
  /// @retval Error::invalid_state No connection is open.
  /// @retval Error::timeout The phase's deadline passed.
  /// @retval Error::network_failure The write failed or the socket is not
  ///     connected.
  [[nodiscard]] auto send_payload() -> Expected<Progress, Error>;
  /// @brief Reads what has arrived of the reply and tries to parse its
  /// header.
  ///
  /// @return ReplyHeaderAccepted when the header says 200 and the body is
  ///     still to come, which restarts the deadline; the RegisteredConsole
  ///     when the whole reply has arrived and decoded; Waiting otherwise.
  /// @retval Error::refused The console answered with another status.
  /// @retval Error::invalid_header The header could not be parsed.
  /// @retval Error::too_large The reply exceeds reply_limit.
  /// @retval Error::truncated The console closed the connection early.
  /// @retval Error::timeout The phase's deadline passed.
  /// @retval Error Any failure of finishing; see read_reply_body().
  [[nodiscard]] auto read_reply_header() -> Expected<Progress, Error>;
  /// @brief Reads more of the reply body, and decodes the reply once it has
  /// all arrived.
  ///
  /// @return The RegisteredConsole once the reply is decoded, after which the
  ///     socket is closed and everything wiped; Waiting while some remains.
  /// @retval Error::truncated The console closed the connection early.
  /// @retval Error::timeout The phase's deadline passed.
  /// @retval Error::crypto_failure The body could not be decrypted.
  /// @retval Error::invalid_payload The decrypted body could not be parsed.
  /// @retval Error::network_failure The read failed, or closing the socket
  ///     afterwards failed.
  [[nodiscard]] auto read_reply_body() -> Expected<Progress, Error>;
  /// @brief Closes the socket and wipes the request, the reply and the key
  /// material. Safe to call at any time and more than once.
  ///
  /// @return Nothing on success.
  /// @retval Error::network_failure The socket did not close cleanly; it is
  ///     dropped regardless.
  [[nodiscard]] auto cancel() -> Expected<void, Error>;

  /// @brief The socket to wait on between steps.
  ///
  /// @return The socket, or null when no connection is open.
  auto pollable() const noexcept -> const platform::Pollable* {
    return socket_.get();
  }
  /// @brief The HTTP status of the console's reply.
  ///
  /// @return The status once a reply header was parsed; it is kept after the
  ///     exchange ends.
  auto http_status() const noexcept -> std::optional<std::uint16_t> {
    return http_status_;
  }
  /// @brief The RP-Application-Reason the console gave in its reply header.
  ///
  /// @return The reason code, or nothing when the header carried none.
  auto ps_reason() const noexcept -> std::optional<std::uint32_t> {
    return ps_reason_;
  }

 private:
  /// @brief Starts a fresh phase deadline of phase_timeout from now.
  void reset_deadline() noexcept;
  /// @brief Wipes the staged header and payload.
  void forget_request() noexcept;
  /// @brief How far one send went.
  enum class Delivery : std::uint8_t { partial, complete, not_connected };
  /// @var Delivery::partial
  /// @brief Some of the part remains to be sent.
  /// @var Delivery::complete
  /// @brief The whole part is sent.
  /// @var Delivery::not_connected
  /// @brief The socket is not connected yet.

  /// @brief Sends what the socket takes of a part, resuming after the bytes
  /// already sent.
  ///
  /// @param part The whole header or payload.
  /// @return How far the part has gone.
  /// @retval Error::invalid_state No connection is open.
  /// @retval Error::timeout The phase's deadline passed.
  /// @retval Error::network_failure The write failed or wrote a nonsensical
  ///     count.
  [[nodiscard]] auto send_part(std::span<const std::byte> part)
      -> Expected<Delivery, Error>;
  /// @brief Reads what has arrived into the reply buffer, never past `limit`.
  ///
  /// @param limit Total reply bytes wanted, in bytes.
  /// @return True when bytes were read, false when none had arrived yet.
  /// @retval Error::invalid_state No connection is open.
  /// @retval Error::too_large The buffer already holds `limit` bytes.
  /// @retval Error::timeout Nothing arrived before the deadline.
  /// @retval Error::truncated The console closed the connection.
  /// @retval Error::network_failure The read failed.
  [[nodiscard]] auto receive_up_to(std::size_t limit) -> Expected<bool, Error>;
  /// @brief Parses the reply header from what has arrived and records its
  /// status and reason.
  ///
  /// @return As read_reply_header() describes.
  [[nodiscard]] auto parse_header() -> Expected<Progress, Error>;
  /// @brief Decrypts the reply body in place, parses it and closes the
  /// exchange.
  ///
  /// @return The registered console.
  /// @retval Error::crypto_failure The body could not be decrypted.
  /// @retval Error::invalid_payload The body could not be parsed.
  /// @retval Error::network_failure Closing the socket failed.
  [[nodiscard]] auto finish() -> Expected<Progress, Error>;
  /// @brief Cancels the exchange and returns `error`.
  ///
  /// @param error The failure to report.
  /// @return `error`, as an unexpected value.
  [[nodiscard]] auto fail(Error error) -> Unexpected<Error>;
  /// @brief Closes and drops the socket.
  ///
  /// @return Nothing when there was no socket or it closed.
  /// @retval Error::network_failure The socket did not close cleanly.
  [[nodiscard]] auto close() -> Expected<void, Error>;

  /// @brief Where the TCP socket comes from.
  platform::SocketProvider& sockets_;
  /// @brief Decrypts the reply.
  platform::CryptoProvider& crypto_;
  /// @brief The monotonic clock deadlines are measured by.
  const platform::ClockProvider& clock_;
  /// @brief The connection to the console; null when none is open.
  std::unique_ptr<platform::TcpSocket> socket_;
  /// @brief The staged request header, wiped once sent.
  SecretArray<header_limit> header_;
  /// @brief The staged encrypted request body, wiped once sent.
  SecretArray<payload_limit> payload_;
  /// @brief The reply as it arrives; the body is decrypted in place.
  SecretArray<reply_limit> reply_;
  /// @brief The key material the reply is decrypted with.
  net::protocol::remote_play::RegistrationMaterial material_;
  /// @brief The console variant the request was encoded for.
  net::protocol::remote_play::RegistrationTarget target_{};
  /// @brief Bytes of #header_ in use; zero when nothing is staged.
  std::size_t header_size_{};
  /// @brief Bytes of #payload_ in use; zero when nothing is staged.
  std::size_t payload_size_{};
  /// @brief Bytes of the part being sent that are already sent.
  std::size_t sent_{};
  /// @brief Bytes of the reply received so far.
  std::size_t filled_{};
  /// @brief Bytes of the reply that are its header, once parsed.
  std::size_t reply_header_size_{};
  /// @brief Bytes of the whole reply, header and body, once the header is
  /// parsed.
  std::size_t reply_size_{};
  /// @brief When the current phase gives up, on the monotonic clock.
  timing::Deadline deadline_;
  /// @brief The connecting phase's deadline, restored when a header send
  /// finds the socket not yet connected.
  timing::Deadline connect_deadline_;
  /// @brief The RP-Application-Reason of the last reply header, if any.
  std::optional<std::uint32_t> ps_reason_;
  /// @brief The HTTP status of the last reply header, once parsed.
  std::optional<std::uint16_t> http_status_;
};

}  // namespace libchiaking::consoles
