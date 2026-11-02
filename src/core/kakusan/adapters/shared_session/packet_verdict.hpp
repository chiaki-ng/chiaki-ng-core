/// @file
/// @brief Declares PacketVerdict, the host's check of each encrypted stream
/// datagram before it is forwarded to guests.
#pragma once

#include "libchiaking/detail/play/session_share.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <cstddef>
#include <cstdint>

namespace libchiaking::kakusan {

/// @brief Whether a datagram is forwarded.
enum class Admission : std::uint8_t {
  /// @brief Well formed; forward it.
  forward,
  /// @brief Well formed but not for guests (auxiliary audio); skip it.
  exclude,
  /// @brief Malformed or inconsistent with the stream; skip it.
  reject,
};

/// @brief What the host decided about one datagram, and which media it is.
///
/// Only the clear header fields the play context reported alongside the
/// datagram, and its first byte, are checked; the payload stays encrypted.
/// Stateless apart from the result; classify() may be called from any
/// thread.
struct PacketVerdict {
  /// @brief Checks one datagram.
  ///
  /// Rejects an empty or oversized datagram, one whose sealed size does not
  /// fit inside it, whose first byte does not name the media family the play
  /// context reported, or whose key position would overflow. Video must have
  /// a consistent unit index, total and redundancy count and name an existing
  /// profile; audio must be Opus with units that add up to the sealed size,
  /// and auxiliary audio is excluded.
  ///
  /// @param profile_count The number of video profiles in the stream.
  /// @param datagram The datagram and what the play context knows about it.
  /// @return The verdict.
  [[nodiscard]] static auto classify(
      std::size_t profile_count, const play::SharedDatagram& datagram) noexcept
      -> PacketVerdict;

  /// @brief Whether to forward it.
  Admission admission{Admission::reject};
  /// @brief Video or audio, as the media sender queues it.
  net::kakusan::MediaKind kind{net::kakusan::MediaKind::video};

  /// @brief Compares both fields.
  /// @return True when they are equal.
  constexpr bool operator==(const PacketVerdict&) const noexcept = default;

 private:
  /// @brief Bits of the first byte that name the packet family.
  static constexpr std::uint8_t family_mask = 0x0f;
  /// @brief Family value of a video packet.
  static constexpr std::uint8_t family_video = 2;
  /// @brief Family value of an audio packet.
  static constexpr std::uint8_t family_audio = 3;
  /// @brief The audio codec value for Opus, the only one forwarded.
  static constexpr std::uint8_t opus_codec = 5;
  /// @brief Margin, in bytes, that classify() adds to the key position and
  /// datagram size when checking that the key stream position cannot
  /// overflow 64 bits.
  static constexpr std::uint64_t cipher_offset = 16;

  /// @brief Checks a video datagram's unit fields and profile.
  /// @param profile_count The number of video profiles.
  /// @param datagram The datagram.
  /// @return Admission::forward or Admission::reject.
  [[nodiscard]] static auto video(std::size_t profile_count,
                                  const play::SharedDatagram& datagram) noexcept
      -> Admission;
  /// @brief Checks an audio datagram's codec and units.
  /// @param datagram The datagram.
  /// @return Admission::exclude for auxiliary audio, otherwise forward or
  ///     reject.
  [[nodiscard]] static auto audio(const play::SharedDatagram& datagram) noexcept
      -> Admission;
};

}  // namespace libchiaking::kakusan
