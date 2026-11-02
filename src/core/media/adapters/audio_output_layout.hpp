/// @file
/// @brief Declares AudioOutputLayout, the plan that routes a stream's decoded
/// audio channels onto a speaker layout the host can play.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace libchiaking::media {

/// @enum AudioLayoutSource
/// @brief Where an AudioOutputLayout learned the stream's channel positions.
///
/// @var AudioLayoutSource direct
/// @brief Mono or stereo; each channel is passed straight through.
///
/// @var AudioLayoutSource reported
/// @brief The stream reported a complete, unambiguous position for every
/// channel.
///
/// @var AudioLayoutSource assumed
/// @brief The reported positions were missing or unusable, so the standard
/// order for the channel count was assumed.
enum class AudioLayoutSource : std::uint8_t { direct, reported, assumed };
/// @brief Why no audio layout could be planned.
enum class AudioLayoutError : std::uint8_t {
  /// @brief The channel count is zero or above the stream's maximum.
  invalid_channel_count,
  /// @brief The positions are unusable and the channel count has no standard
  /// order to assume.
  unknown_layout
};

/// @brief How each channel of a decoded stream is mixed into the host's
/// output channels.
///
/// Mono and stereo pass through. A 6-channel stream goes to 5.1, a 12-channel
/// one to 7.1.4, anything else to 7.1, using the positions the stream reported
/// or the standard order for the count. When the output has no height
/// speakers, the height channels fold into the front and back pairs at
/// AudioOutputLayout::height_gain. Valid from construction, which only
/// AudioOutputLayout::plan performs. Immutable value; safe to share.
class AudioOutputLayout {
 public:
  /// @brief The most output channels a plan produces (7.1.4).
  static constexpr std::size_t max_output_channels = 12;
  /// @brief A gain of 1.0 in Q15 fixed point.
  static constexpr std::int32_t unity_gain = 32768;
  /// @brief A gain of about 0.707 (minus 3 dB) in Q15, applied to height
  /// channels folded into a layout without height speakers.
  static constexpr std::int32_t height_gain = 23170;

  /// @brief Where one input channel goes.
  struct Route {
    /// @brief The output channel index it is mixed into.
    std::uint8_t target{};
    /// @brief The gain applied, in Q15 fixed point.
    std::int32_t gain{};

    /// @brief Compares target and gain.
    ///
    /// @return `true` when both routes are the same.
    constexpr bool operator==(const Route&) const noexcept = default;
  };

  /// @brief Plans the routing for a stream's channels.
  ///
  /// @param channels The number of decoded channels.
  /// @param positions The positions the stream reported, one per channel; may
  ///     be empty.
  /// @return The plan; AudioLayoutError::invalid_channel_count for zero or too
  ///     many channels; AudioLayoutError::unknown_layout when the positions are
  ///     incomplete, unknown or repeated and the count is not 6, 8 or 12.
  static auto plan(
      std::size_t channels,
      std::span<const net::protocol::remote_play::AudioChannelPosition>
          positions = {}) -> Expected<AudioOutputLayout, AudioLayoutError>;

  /// @brief Reads the number of input channels.
  ///
  /// @return The decoded channel count the plan expects.
  auto input_channels() const noexcept -> std::size_t {
    return input_channels_;
  }
  /// @brief Reads the number of output channels.
  ///
  /// @return 1, 2, 6, 8 or 12.
  auto output_channels() const noexcept -> std::size_t {
    return output_channels_;
  }
  /// @brief Reads where the positions came from.
  ///
  /// @return The source of the plan's positions.
  auto source() const noexcept -> AudioLayoutSource { return source_; }
  /// @brief Reports whether samples must be mixed rather than copied.
  ///
  /// @return `true` for anything beyond mono and stereo.
  auto remaps() const noexcept -> bool {
    return source_ != AudioLayoutSource::direct;
  }
  /// @brief Reads the route of every input channel.
  ///
  /// @return One route per input channel, in input order; valid while the plan
  ///     lives.
  auto routes() const noexcept -> std::span<const Route> {
    return std::span{routes_}.first(input_channels_);
  }

  /// @brief Mixes interleaved 16-bit samples from input to output channels.
  ///
  /// Each output sample is the rounded sum of its routed inputs, clamped to the
  /// 16-bit range. Does not allocate or block.
  ///
  /// @param input Interleaved samples with AudioOutputLayout::input_channels
  ///     per frame.
  /// @param output Room for interleaved samples with
  ///     AudioOutputLayout::output_channels per frame.
  /// @return The number of whole frames mixed, bounded by both spans.
  auto apply(std::span<const std::int16_t> input,
             std::span<std::int16_t> output) const noexcept -> std::size_t;

 private:
  /// @brief A speaker position as the stream names it.
  using Position = net::protocol::remote_play::AudioChannelPosition;

  /// @brief Where one position goes in each surround layout.
  struct PositionRoute {
    /// @brief Output index in a 5.1 layout.
    std::uint8_t surround_5_1;
    /// @brief Output index in a 7.1 layout.
    std::uint8_t surround_7_1;
    /// @brief Output index in a 7.1.4 layout.
    std::uint8_t surround_7_1_4;
    /// @brief Gain in Q15 when the position folds into 5.1 or 7.1.
    std::int32_t gain;
  };

  /// @brief The number of distinct named positions, mono included.
  static constexpr std::size_t position_count =
      static_cast<std::size_t>(Position::mono) + 1;

  /// @brief The standard order of a 6-channel stream.
  static constexpr std::array assumed_5_1{
      Position::front_left, Position::front_right, Position::front_center,
      Position::lfe,        Position::side_left,   Position::side_right};

  /// @brief The standard order of an 8- or 12-channel stream; an 8-channel
  /// stream uses the first eight.
  static constexpr std::array assumed_7_1_4{
      Position::front_left,     Position::front_right,
      Position::front_center,   Position::lfe,
      Position::side_left,      Position::side_right,
      Position::back_left,      Position::back_right,
      Position::top_front_left, Position::top_front_right,
      Position::top_back_left,  Position::top_back_right};

  /// @brief Gives the routes of one position.
  ///
  /// @param position The speaker position.
  /// @return Its output index in each layout and its fold gain; mono goes to
  ///     the centre; an unknown position gets gain 0.
  static constexpr auto position_route(Position position) noexcept
      -> PositionRoute;
  /// @brief Picks the output layout for a channel count.
  ///
  /// @param channels The input channel count, above 2.
  /// @return 6 for six, 12 for twelve, otherwise 8.
  static auto output_channels_for(std::size_t channels) noexcept -> std::size_t;
  /// @brief Reports whether reported positions can be trusted.
  ///
  /// @param channels The input channel count.
  /// @param positions The reported positions.
  /// @return `true` when there is one known, distinct position per channel.
  static auto known(std::size_t channels,
                    std::span<const Position> positions) noexcept -> bool;
  /// @brief Gives the standard order for a channel count.
  ///
  /// @param channels The input channel count.
  /// @return The order for 6, 8 or 12 channels; empty for any other count.
  static auto assumed(std::size_t channels) noexcept
      -> std::optional<std::span<const Position>>;

  /// @brief Builds an empty plan for AudioOutputLayout::plan to fill.
  AudioOutputLayout() noexcept = default;

  /// @brief Decoded channels per frame.
  std::size_t input_channels_{};
  /// @brief Output channels per frame.
  std::size_t output_channels_{};
  /// @brief Where the positions came from.
  AudioLayoutSource source_{AudioLayoutSource::direct};
  /// @brief The route of each input channel; only the first
  /// AudioOutputLayout::input_channels_ are used.
  std::array<Route, net::protocol::remote_play::audio_max_channels> routes_{};
};

}  // namespace libchiaking::media
