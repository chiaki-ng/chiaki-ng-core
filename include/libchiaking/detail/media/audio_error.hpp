/// @file
/// @brief Declares the failures reported by the audio decoder and the
/// microphone encoder.
#pragma once

#include <cstdint>

namespace libchiaking::media {

/// @brief Why AudioDecoder or MicrophoneEncoder could not be created or could
/// not process a block.
///
/// @see media::AudioDecoder
/// @see MicrophoneEncoder
enum class AudioError : std::uint8_t {
  /// @brief The audio format is outside what the decoder accepts: channel
  /// count, sample width, sample rate or frame size.
  invalid_format,
  /// @brief The channel layout cannot be mapped to an output layout, or the
  /// codec rejected it.
  unsupported_layout,
  /// @brief The Opus codec could not be created.
  codec_unavailable,
  /// @brief The packet is too large, or a raw PCM packet does not hold whole
  /// frames.
  invalid_packet,
  /// @brief The codec failed to decode the packet.
  decode_failed,
  /// @brief The codec did not produce a microphone frame of the required
  /// size.
  encode_failed,
};

}  // namespace libchiaking::media
