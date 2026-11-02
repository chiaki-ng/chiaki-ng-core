/// @file
/// @brief Declares BindingClient, a single STUN Binding query for a socket's
/// public address.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/nat/stun/message.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::net::nat::stun {

/// @brief Asks a STUN server what public address and port a UDP socket is seen
/// from.
///
/// Sends one 20-byte Binding request with no attributes and waits for the
/// success response carrying the same transaction id. Borrows a bound socket,
/// its poller and a monotonic clock, which must outlive it; use from one
/// thread.
class BindingClient {
 public:
  /// @brief Why a datagram is not a usable Binding success response.
  enum class ResponseError : std::uint8_t {
    /// @brief Shorter than a STUN header.
    short_packet,
    /// @brief Not a Binding success response.
    invalid_type,
    /// @brief The length field does not match the datagram.
    invalid_length,
    /// @brief The magic cookie is wrong.
    invalid_cookie,
    /// @brief The transaction id is not the request's.
    invalid_transaction,
    /// @brief An attribute runs past the datagram, or the mapped address is
    /// invalid.
    invalid_attribute,
    /// @brief No MAPPED-ADDRESS or XOR-MAPPED-ADDRESS.
    no_mapped_address,
  };

  /// @brief Why a query got no mapped address.
  enum class Error : std::uint8_t {
    /// @brief The socket and poller have different owners, the server has
    /// port
    /// 0 or no address, or the socket is unbound or of another family.
    invalid_input,
    /// @brief The request could not be sent whole.
    send_failed,
    /// @brief Polling or receiving failed.
    net_failed,
    /// @brief A response with the request's transaction id was invalid, or
    /// a datagram was oversized.
    protocol,
    /// @brief No answer in time, or the stop token's deadline ran out.
    timeout,
    /// @brief The stop token was stopped by its caller.
    cancelled,
  };

  /// @brief Bytes read per datagram.
  static constexpr std::size_t receive_buffer_size = 256;

  /// @brief Prepares queries over a bound socket.
  ///
  /// @param socket The socket whose mapping is wanted; must outlive the
  ///     client.
  /// @param poller A poller of the same owner as `socket`.
  /// @param clock The monotonic clock timing the wait.
  BindingClient(platform::UdpSocket& socket, platform::Poller& poller,
                const platform::ClockProvider& clock) noexcept;

  /// @brief Builds a Binding request.
  ///
  /// @param transaction The random transaction id.
  /// @return The 20-byte header with an empty body.
  static auto request(const TransactionId& transaction) noexcept
      -> std::array<std::byte, header_size>;

  /// @brief Reads the mapped address from a Binding success response.
  ///
  /// The first MAPPED-ADDRESS or XOR-MAPPED-ADDRESS is used.
  ///
  /// @param response The datagram.
  /// @param transaction The request's transaction id.
  /// @return The public endpoint.
  /// @retval ResponseError The first check the datagram failed.
  [[nodiscard]] static auto mapped_endpoint(std::span<const std::byte> response,
                                            const TransactionId& transaction)
      -> Expected<platform::SocketEndpoint, ResponseError>;

  [[nodiscard]] static auto
  /// @brief Judges a datagram received while waiting.
  ///
  /// @param datagram The datagram.
  /// @param transaction The request's transaction id.
  /// @return The public endpoint; nothing when the datagram belongs to
  ///     another
  ///     transaction.
  /// @retval ResponseError The datagram is invalid for any other reason.
  answer(std::span<const std::byte> datagram, const TransactionId& transaction)
      -> Expected<std::optional<platform::SocketEndpoint>, ResponseError>;

  /// @brief Sends a Binding request and waits for its answer.
  ///
  /// Sent once, without retransmission. Blocks for up to `timeout`.
  ///
  /// @param server The STUN server, of the socket's address family.
  /// @param transaction The random transaction id.
  /// @param stop_token Ends the wait; a deadline stop reads as a timeout.
  /// @param timeout How long to wait.
  /// @return The socket's public endpoint as the server sees it.
  /// @retval Error::invalid_input The arguments cannot make a query.
  /// @retval Error::send_failed The request was not sent whole.
  /// @retval Error::net_failed Polling or receiving failed.
  /// @retval Error::protocol The answer was invalid.
  /// @retval Error::timeout No answer in time.
  /// @retval Error::cancelled The stop token fired.
  [[nodiscard]] auto query(const platform::SocketEndpoint& server,
                           const TransactionId& transaction,
                           const platform::StopToken& stop_token,
                           std::chrono::nanoseconds timeout)
      -> Expected<platform::SocketEndpoint, Error>;

 private:
  /// @brief Names why a stopped token stopped.
  ///
  /// @param stop_token The stopped token.
  /// @return Error::timeout for a deadline, otherwise Error::cancelled.
  static auto stopped(const platform::StopToken& stop_token) noexcept -> Error;

  /// @brief The borrowed socket.
  platform::UdpSocket& socket_;
  /// @brief The borrowed poller.
  platform::Poller& poller_;
  /// @brief The borrowed monotonic clock.
  const platform::ClockProvider& clock_;
};

}  // namespace libchiaking::net::nat::stun
