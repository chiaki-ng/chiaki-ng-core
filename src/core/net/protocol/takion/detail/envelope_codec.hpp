/// @file
/// @brief Declares the framing a Takion datagram may carry in front of its
/// first byte, and the codec that adds and removes it.
///
/// Console Remote Play sends Takion packets bare. LegacyCloudStream and some
/// CloudStream servers sit behind a relay that expects a 4-byte prefix: three
/// zero bytes and a one-byte server type (a u32 big-endian), so the relay can
/// route the datagram.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief No framing: the datagram is the Takion packet.
struct PlainEnvelope {};

/// @brief The 4-byte relay prefix.
struct PsnEnvelope {
  /// @brief The routing byte in the prefix's last position; for a CloudStream
  /// server it is taken from the last octet of the server's private address,
  /// 1 when that is unknown.
  std::uint8_t type{};
};

/// @brief The framing used on one direction of a session.
using PacketEnvelope = std::variant<PlainEnvelope, PsnEnvelope>;

/// @brief Adds and checks the datagram framing. Stateless; safe from any
/// thread.
class EnvelopeCodec {
 public:
  /// @brief Frames a Takion packet for the socket.
  ///
  /// @param plain_packet The packet, starting with its type byte.
  /// @param envelope The framing to add.
  /// @return The datagram; never fails today.
  [[nodiscard]] static auto encode(std::span<const std::byte> plain_packet,
                                   const PacketEnvelope& envelope)
      -> Expected<SecretBytes, ParseFailure>;

  /// @brief Strips and checks the framing of a received datagram.
  ///
  /// @param datagram The datagram as read from the socket.
  /// @param envelope The framing expected.
  /// @return The Takion packet inside, as a view of `datagram`.
  /// @retval ParseFailureReason::truncated A PSN prefix was expected but the
  ///     datagram is shorter than 4 bytes.
  /// @retval ParseFailureReason::invalid_field A padding byte is not zero, or
  /// the
  ///     type byte differs; the failure's offset names the byte.
  [[nodiscard]] static auto decode(std::span<const std::byte> datagram,
                                   const PacketEnvelope& envelope)
      -> Expected<std::span<const std::byte>, ParseFailure>;

  /// @brief Tells how many bytes the framing adds.
  ///
  /// @param envelope The framing.
  /// @return 4 for a PSN prefix, 0 otherwise.
  static constexpr auto overhead(const PacketEnvelope& envelope) noexcept
      -> std::size_t {
    return std::holds_alternative<PsnEnvelope>(envelope) ? psn_envelope_size
                                                         : 0;
  }
};

}  // namespace libchiaking::net::protocol::takion
