/// @file
/// @brief The Opus multistream layouts of stream audio and which channel
/// counts each service may be asked for.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libchiaking::net::protocol::remote_play {

/// @brief The most audio channels a stream carries (7.1.4).
inline constexpr std::size_t audio_max_channels = 12;

/// @brief The speaker a decoded audio channel feeds.
enum class AudioChannelPosition : std::uint8_t {
  /// @brief Not stated by the layout.
  unknown = 0,
  /// @brief Front left.
  front_left = 1,
  /// @brief Front right.
  front_right = 2,
  /// @brief Front centre.
  front_center = 3,
  /// @brief Low-frequency effects.
  lfe = 4,
  /// @brief Side left.
  side_left = 5,
  /// @brief Side right.
  side_right = 6,
  /// @brief Back left.
  back_left = 7,
  /// @brief Back right.
  back_right = 8,
  /// @brief Top front left.
  top_front_left = 9,
  /// @brief Top front right.
  top_front_right = 10,
  /// @brief Top back left.
  top_back_left = 11,
  /// @brief Top back right.
  top_back_right = 12,
  /// @brief A single mono channel.
  mono = 13,
};

/// @brief How an Opus multistream decoder splits a channel count into
/// streams.
///
/// Plain data.
struct OpusMultistreamLayout {
  /// @brief The number of Opus streams.
  std::uint8_t streams{};
  /// @brief How many of them are stereo pairs.
  std::uint8_t coupled_streams{};
  /// @brief The decoder's channel mapping; identity for the first channels.
  std::array<std::uint8_t, audio_max_channels> mapping{};
  /// @brief The speaker of each channel, when known.
  std::array<AudioChannelPosition, audio_max_channels> positions{};

  /// @brief Compares every field.
  ///
  /// @return True when equal.
  constexpr bool operator==(const OpusMultistreamLayout&) const noexcept =
      default;
};

/// @brief The layout of a channel count.
///
/// Mono and stereo name their speakers; 5.1 (6), 7.1 (8) and 7.1.4 (12) use
/// 4/2, 5/3 and 7/5 streams/coupled with positions left unknown.
///
/// @param channels The channel count.
/// @return The layout, or nothing for any other count.
[[nodiscard]] constexpr auto opus_multistream_layout(
    std::uint8_t channels) noexcept -> std::optional<OpusMultistreamLayout> {
  OpusMultistreamLayout layout;
  switch (channels) {
    case 1:
      layout.streams = 1;
      layout.coupled_streams = 0;
      layout.positions[0] = AudioChannelPosition::mono;
      break;
    case 2:
      layout.streams = 1;
      layout.coupled_streams = 1;
      layout.positions[0] = AudioChannelPosition::front_left;
      layout.positions[1] = AudioChannelPosition::front_right;
      break;
    case 6:
      layout.streams = 4;
      layout.coupled_streams = 2;
      break;
    case 8:
      layout.streams = 5;
      layout.coupled_streams = 3;
      break;
    case 12:
      layout.streams = 7;
      layout.coupled_streams = 5;
      break;
    default:
      return std::nullopt;
  }
  for (std::uint8_t index = 0; index < channels; ++index)
    layout.mapping[index] = index;
  return layout;
}

/// @brief Whether a requested channel count is one the stream can carry.
///
/// @param channels The request; 0 means the service's default.
/// @return True for 0, 2, 6, 8 or 12.
constexpr auto valid_requested_audio_channels(std::uint64_t channels) noexcept
    -> bool {
  return channels == 0 || channels == 2 || channels == 6 || channels == 8 ||
         channels == 12;
}

/// @brief The service an audio channel request is for.
enum class AudioRequestTarget : std::uint8_t {
  ps4,
  ps5,
  legacy_cloud_stream,
  cloud_stream
};
/// @var AudioRequestTarget::ps4
/// @brief A PS4 over Remote Play.
/// @var AudioRequestTarget::ps5
/// @brief A PS5 over Remote Play.
/// @var AudioRequestTarget::legacy_cloud_stream
/// @brief PlayStation Now.
/// @var AudioRequestTarget::cloud_stream
/// @brief PS5 cloud streaming.

/// @brief Whether a service can be asked for a channel count.
///
/// Every service takes 0 or 2; only a PS5 and PS5 cloud take 6, 8 or 12.
///
/// @param target The service.
/// @param channels The request.
/// @return True when supported.
constexpr auto requested_audio_channels_supported(
    AudioRequestTarget target, std::uint64_t channels) noexcept -> bool {
  if (target > AudioRequestTarget::cloud_stream ||
      !valid_requested_audio_channels(channels))
    return false;
  return channels <= 2 || target == AudioRequestTarget::ps5 ||
         target == AudioRequestTarget::cloud_stream;
}

}  // namespace libchiaking::net::protocol::remote_play
