/// @file
/// @brief Declares MicrophoneSender, which packs the client's microphone
/// into Takion audio packets for the console.
///
/// The microphone is sent as 40-byte Opus frames at 48 kHz mono, three frames
/// per packet for redundancy, in the same order on every route: the new
/// frame, then the two before it, oldest first. No FEC
/// units are sent. The packet header is: type 0x03, big-endian
/// 16-bit frame index, big-endian 16-bit next index, big-endian 32-bit unit
/// word (the FEC word with the frame count less one in bits 16..23), codec tag
/// 0x05, then zeros up to 19 bytes, or 20 when the negotiated AV layout tags
/// audio with a lane byte (version 11 on).
#pragma once

#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief The microphone packet header size when audio is tagged, in bytes.
inline constexpr std::size_t microphone_header_size_tagged = 20;
/// @brief The microphone packet header size when audio is untagged, in bytes.
inline constexpr std::size_t microphone_header_size_untagged = 19;

/// @brief Whether microphone packets take the tagged header, as every audio
/// packet does from version 11; PS Now and PS5 consoles read the microphone
/// one byte late without it.
///
/// @param layout The negotiated AV layout.
/// @return False for the version 7 and versions 9 to 10 layouts, true for
///     later ones.
[[nodiscard]] constexpr auto microphone_header_tagged(
    takion::AvLayout layout) noexcept -> bool {
  return layout != takion::AvLayout::version_7 &&
         layout != takion::AvLayout::versions_9_to_10;
}
/// @brief The unit word sent normally.
inline constexpr std::uint32_t microphone_fec_word = 0x2821U;
/// @brief The unit word sent when Takion encodes unit counts only
/// (av_unit_count_only_encoding).
inline constexpr std::uint32_t microphone_fec_word_unit_count_only = 0x2001U;
/// @brief The packet type byte of microphone audio.
inline constexpr std::byte microphone_packet_type{0x03};
/// @brief The codec tag byte of microphone audio.
inline constexpr std::byte microphone_codec_tag{0x05};
/// @brief The audio header announcing the microphone format: one channel, 16
/// bits, 48000 Hz, 480 samples per frame.
inline constexpr std::array<std::byte, 14> microphone_audio_header{
    std::byte{0x01}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00},
    std::byte{0xbb}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x01}, std::byte{0xe0}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x01}};

/// @brief Packs microphone frames into packets.
///
/// The history starts as two copies of the first frame, so every frame,
/// including the first, yields a packet holding it and then the two frames
/// before it, oldest first. Not thread-safe.
class MicrophoneSender {
 public:
  /// @brief The size of one encoded frame, in bytes.
  static constexpr std::size_t frame_size = 40;
  /// @brief How many frames each packet carries.
  static constexpr std::size_t frames_per_packet = 3;

  /// @brief Takes one encoded frame.
  ///
  /// @param frame A 40-byte Opus frame.
  /// @param tagged Whether audio is tagged, which sets the header size.
  /// @param unit_count_only Whether Takion encodes unit counts only.
  /// @return The packet to send, or nothing for a frame of the wrong size.
  [[nodiscard]] auto push(std::span<const std::byte> frame, bool tagged,
                          bool unit_count_only)
      -> std::optional<takion::SendMicrophone> {
    if (frame.size() != frame_size) return std::nullopt;
    if (!history_) {
      history_.emplace();
      for (auto& earlier : *history_)
        std::copy(frame.begin(), frame.end(), earlier.begin());
    }

    const std::size_t header_size =
        tagged ? microphone_header_size_tagged : microphone_header_size_untagged;
    takion::SendMicrophone command;
    command.payload_offset = header_size;
    auto& packet = command.packet;
    packet.assign(header_size + frames_per_packet * frame_size, std::byte{});
    const std::uint32_t fec_word = unit_count_only
                                       ? microphone_fec_word_unit_count_only
                                       : microphone_fec_word;
    const std::uint32_t units =
        fec_word | (std::uint32_t{frames_per_packet - 1} << 16U);
    const auto following = static_cast<std::uint16_t>(frame_index_ + 1U);
    auto& [oldest, newest] = *history_;
    bytes::Writer writer{packet};
    if (!writer.u8(std::to_integer<std::uint8_t>(microphone_packet_type)) ||
        !writer.u16_be(frame_index_) || !writer.u16_be(following) ||
        !writer.u32_be(units) ||
        !writer.u8(std::to_integer<std::uint8_t>(microphone_codec_tag)) ||
        !writer.zeros(header_size - writer.offset()) || !writer.bytes(frame) ||
        !writer.bytes(oldest) || !writer.bytes(newest))
      return std::nullopt;
    oldest = newest;
    std::copy(frame.begin(), frame.end(), newest.begin());
    frame_index_ = following;
    return command;
  }

 private:
  /// @brief The two frames before the current one, oldest first, or nothing
  /// before the first frame.
  std::optional<std::array<std::array<std::byte, frame_size>, 2>> history_;
  /// @brief The next packet's frame index.
  std::uint16_t frame_index_{};
};

}  // namespace libchiaking::net::protocol::remote_play
