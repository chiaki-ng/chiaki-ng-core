/// @file
/// @brief Declares Packet, the SNTP client's encoder for requests and validator
/// for server replies.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/ntp/timestamp.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::net::ntp {

/// @brief A validated SNTP server reply: the times the server received the
/// request and sent its answer.
///
/// The 48-byte NTP header, all fields big-endian (RFC 5905 figure 8):
///
/// | Offset | Size | Field |
/// | ------ | ---- | ----- |
/// | 0 | 1 | leap indicator (bits 7-6), version (bits 5-3), mode (bits 2-0) |
/// | 1 | 1 | stratum |
/// | 2 | 1 | poll interval |
/// | 3 | 1 | precision |
/// | 4 | 4 | root delay, 16.16 fixed-point seconds |
/// | 8 | 4 | root dispersion, 16.16 fixed-point seconds |
/// | 12 | 4 | reference id |
/// | 16 | 8 | reference timestamp |
/// | 24 | 8 | origin timestamp |
/// | 32 | 8 | receive timestamp |
/// | 40 | 8 | transmit timestamp |
///
/// Requests carry a random nonce in the transmit timestamp instead of the real
/// time, and a reply is accepted only when it echoes that nonce in its origin
/// timestamp; this keeps the local time private and ties replies to requests.
/// Only made by reply(). A value type, safe to share between threads.
class Packet {
 public:
  /// @brief Bytes in an NTP header without extensions or authenticator.
  static constexpr std::size_t size = 48;
  /// @brief A request as sent: exactly one header.
  using Datagram = std::array<std::byte, size>;

  /// @brief Largest root dispersion a server may report: 1 second. A server
  /// less sure of its time than this is not used.
  static constexpr std::chrono::microseconds max_root_dispersion{
      std::chrono::seconds{1}};

  /// @brief Why a datagram was not accepted as the answer to a request.
  enum class Rejection : std::uint8_t {
    /// @brief The datagram is shorter than 48 bytes.
    short_packet,
    /// @brief The origin timestamp is not the nonce sent; the datagram
    /// answers
    /// some other request, or none.
    origin_mismatch,
    /// @brief The mode is not 4 (server).
    not_a_server,
    /// @brief The version is neither 3 nor 4.
    unsupported_version,
    /// @brief The leap indicator is 3: the server's clock is not
    /// synchronised.
    unsynchronised,
    /// @brief The stratum is 0: a kiss-of-death packet telling the client
    /// to
    /// back off or go elsewhere.
    kiss_of_death,
    /// @brief The stratum is above 15.
    stratum_out_of_range,
    /// @brief The transmit timestamp is zero.
    no_transmit_time,
    /// @brief The root dispersion is 1 second or more.
    dispersed,
  };

  /// @brief Builds a client request.
  ///
  /// Byte 0 is 0x23 (no leap warning, version 4, mode 3 client), the transmit
  /// timestamp is the nonce, and every other byte is zero.
  ///
  /// @param nonce The value the server must echo as its origin timestamp;
  ///     should be
  ///     random and non-zero.
  /// @return The 48-byte request.
  static auto request(Timestamp nonce) noexcept -> Datagram;

  /// @brief Validates a datagram as the server's answer to the request
  /// carrying
  /// `nonce`.
  ///
  /// Bytes past the first 48 (extensions, authenticator) are ignored. The
  /// checks
  /// run in the order of the Rejection values after short_packet.
  ///
  /// @param datagram The received datagram.
  /// @param nonce The nonce the request carried.
  /// @return The server's receive and transmit times.
  /// @retval Rejection The first check the datagram failed.
  [[nodiscard]] static auto reply(std::span<const std::byte> datagram,
                                  Timestamp nonce)
      -> Expected<Packet, Rejection>;

  /// @brief When the server received the request, by the server's clock.
  ///
  /// @return The receive timestamp.
  constexpr auto received() const noexcept -> Timestamp { return received_; }

  /// @brief When the server sent the reply, by the server's clock.
  ///
  /// @return The transmit timestamp; never zero.
  constexpr auto transmitted() const noexcept -> Timestamp {
    return transmitted_;
  }

 private:
  /// @brief NTP mode 3, client.
  static constexpr std::uint8_t client_mode = 3;
  /// @brief NTP mode 4, server.
  static constexpr std::uint8_t server_mode = 4;
  /// @brief The NTP version requests are sent with: 4.
  static constexpr std::uint8_t client_version = 4;
  /// @brief The leap indicator meaning the server is not synchronised.
  static constexpr std::uint8_t unsynchronised_li = 3;
  /// @brief Highest stratum accepted; 16 and above mean unsynchronised.
  static constexpr std::uint8_t max_stratum = 15;

  /// @brief Holds the times of a reply that passed every check.
  ///
  /// @param received The server's receive timestamp.
  /// @param transmitted The server's transmit timestamp.
  constexpr Packet(Timestamp received, Timestamp transmitted) noexcept
      : received_(received), transmitted_(transmitted) {}

  /// @brief Converts an NTP short-format (16.16 fixed-point seconds) value.
  ///
  /// @param short_format The field as read, host order.
  /// @return The value in microseconds, rounded down.
  static constexpr auto root_dispersion_of(std::uint32_t short_format) noexcept
      -> std::chrono::microseconds {
    return std::chrono::microseconds{static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(short_format) * 1'000'000U) >> 16U)};
  }

  /// @brief When the server received the request.
  Timestamp received_;
  /// @brief When the server sent the reply.
  Timestamp transmitted_;
};

}  // namespace libchiaking::net::ntp
