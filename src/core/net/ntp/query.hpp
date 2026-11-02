/// @file
/// @brief Declares Query, one SNTP request and its answer over a UDP socket.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/ntp/packet.hpp"
#include "src/core/net/ntp/timestamp.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::net::ntp {

/// @brief Asks one NTP server for the time over a caller's UDP socket and
/// turns the answer into a sample of UTC.
///
/// The sample's time is the server's transmit time plus half the network delay,
/// the delay being the round trip on the monotonic clock less the time the
/// server reports holding the request. Datagrams from other sources, too short,
/// or not echoing the nonce are skipped and the wait goes on. The query borrows
/// its socket, poller and clock, which must outlive it; use one query from one
/// thread at a time.
class Query {
 public:
  /// @brief Why a query gave no sample.
  enum class Error : std::uint8_t {
    /// @brief The timeout is not positive, the server has port 0 or an
    /// unspecified address, or the socket and poller belong to different
    /// owners.
    invalid_input,
    /// @brief The request could not be sent whole.
    send_failed,
    /// @brief Polling or receiving failed for a reason other than
    /// would-block.
    net_failed,
    /// @brief A reply from the server failed validation for a reason other
    /// than a missing nonce, or its timing was impossible or too slow.
    rejected,
    /// @brief No valid reply came in time, or the stop token's deadline ran
    /// out.
    timeout,
    /// @brief The stop token was stopped by its caller.
    cancelled,
  };

  /// @brief One server's estimate of the current UTC time, tied to the
  /// monotonic instant it was taken.
  struct Sample {
    /// @brief The server that answered.
    platform::IpAddress server;
    /// @brief UTC at `at_us`, by the server's clock corrected for half the
    /// delay.
    Timestamp::Time time;
    /// @brief The monotonic instant the reply arrived, in microseconds.
    std::uint64_t at_us{};
  };

  /// @brief Largest network delay accepted, the round trip less the server's
  /// hold time: 1 second.
  static constexpr std::chrono::microseconds max_round_trip{
      std::chrono::seconds{1}};
  /// @brief Bytes read per datagram; larger datagrams from the server are
  /// dropped.
  static constexpr std::size_t receive_buffer_size = 128;

  /// @brief Prepares queries over a bound socket.
  ///
  /// @param socket The UDP socket requests go out on and replies come in on;
  ///     must outlive the query.
  /// @param poller A poller of the same owner as `socket`; must outlive the
  ///     query.
  /// @param monotonic The monotonic clock used to time the round trip; must
  ///     outlive the query.
  Query(platform::UdpSocket& socket, platform::Poller& poller,
        const platform::ClockProvider& monotonic) noexcept;

  /// @brief Sends one request and waits for the server's valid answer.
  ///
  /// Blocks for up to `timeout` from when the request was sent, polling the
  /// socket; stray datagrams do not extend the wait.
  ///
  /// @param server The server's address and port (123).
  /// @param nonce The random non-zero value to send as the transmit
  ///     timestamp.
  /// @param stop Cancels the wait; a stop caused by a deadline is reported as
  ///     a
  ///     timeout.
  /// @param timeout How long to wait for the answer; must be positive.
  /// @return The sample.
  /// @retval Error::invalid_input The arguments cannot make a query.
  /// @retval Error::send_failed The request was not sent whole.
  /// @retval Error::net_failed Polling or receiving failed.
  /// @retval Error::rejected The server's reply was invalid or too slow.
  /// @retval Error::timeout No answer within `timeout`.
  /// @retval Error::cancelled The stop token fired.
  [[nodiscard]] auto ask(const platform::SocketEndpoint& server,
                         Timestamp nonce, const platform::StopToken& stop,
                         std::chrono::microseconds timeout)
      -> Expected<Sample, Error>;

  /// @brief Turns a validated reply into a time sample.
  ///
  /// @param reply The server's reply.
  /// @param server The server that sent it.
  /// @param sent_us The monotonic instant the request was sent, in
  ///     microseconds.
  /// @param received_us The monotonic instant the reply arrived, in
  ///     microseconds.
  /// @return The sample: the server's transmit time plus half the delay,
  ///     taken at
  ///     `received_us`.
  /// @retval Error::rejected The reply arrived before it was sent, the
  ///     server's
  ///     hold time is negative or longer than the round trip, or the delay is
  ///     1
  ///     second or more.
  [[nodiscard]] static auto sample_of(const Packet& reply,
                                      const platform::IpAddress& server,
                                      std::uint64_t sent_us,
                                      std::uint64_t received_us)
      -> Expected<Sample, Error>;

 private:
  /// @brief Names why a stopped token stopped.
  ///
  /// @param stop The stopped token.
  /// @return Error::timeout when its deadline ran out, otherwise
  ///     Error::cancelled.
  static auto stopped(const platform::StopToken& stop) noexcept -> Error;
  /// @brief Whether a rejected datagram is noise to skip rather than a bad
  /// answer.
  ///
  /// @param rejection Why the datagram was rejected.
  /// @return True for a short datagram or one not echoing the nonce.
  static auto is_stray(Packet::Rejection rejection) noexcept -> bool;

  /// @brief Reads one datagram after the socket became readable and judges
  /// it.
  ///
  /// Does not block: the socket is read once.
  ///
  /// @param server The server asked.
  /// @param nonce The nonce sent.
  /// @param sent_us The monotonic instant the request was sent, in
  ///     microseconds.
  /// @return The sample when the datagram answers the request; nothing when
  ///     the
  ///     read would block or the datagram came from elsewhere, was oversized
  ///     or
  ///     is stray.
  /// @retval Error::net_failed The read failed.
  /// @retval Error::rejected The server's reply is invalid or its timing is
  ///     impossible or too slow.
  [[nodiscard]] auto answer(const platform::SocketEndpoint& server,
                            Timestamp nonce, std::uint64_t sent_us)
      -> Expected<std::optional<Sample>, Error>;

  /// @brief The borrowed socket.
  platform::UdpSocket& socket_;
  /// @brief The borrowed poller for `socket_`.
  platform::Poller& poller_;
  /// @brief The borrowed monotonic clock.
  const platform::ClockProvider& monotonic_;
};

}  // namespace libchiaking::net::ntp
