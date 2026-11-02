#include "src/core/media/adapters/audio_output_layout.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace libchiaking::media {

constexpr auto AudioOutputLayout::position_route(Position position) noexcept
    -> PositionRoute {
  constexpr auto unity = unity_gain;
  constexpr auto height = height_gain;
  switch (position) {
    case Position::front_left:
      return {0, 0, 0, unity};
    case Position::front_right:
      return {1, 1, 1, unity};
    case Position::front_center:
      return {2, 2, 2, unity};
    case Position::lfe:
      return {3, 3, 3, unity};
    case Position::back_left:
      return {4, 4, 6, unity};
    case Position::back_right:
      return {5, 5, 7, unity};
    case Position::side_left:
      return {4, 6, 4, unity};
    case Position::side_right:
      return {5, 7, 5, unity};
    case Position::top_front_left:
      return {0, 0, 8, height};
    case Position::top_front_right:
      return {1, 1, 9, height};
    case Position::top_back_left:
      return {4, 4, 10, height};
    case Position::top_back_right:
      return {5, 5, 11, height};
    case Position::mono:
      return {2, 2, 2, unity};
    case Position::unknown:
      break;
  }
  return {0, 0, 0, 0};
}

auto AudioOutputLayout::known(std::size_t channels,
                              std::span<const Position> positions) noexcept
    -> bool {
  if (positions.size() < channels) return false;
  std::array<bool, position_count> seen{};
  for (const auto position : positions.first(channels)) {
    const auto index = static_cast<std::size_t>(position);
    if (position == Position::unknown || index >= position_count || seen[index])
      return false;
    seen[index] = true;
  }
  return true;
}

auto AudioOutputLayout::assumed(std::size_t channels) noexcept
    -> std::optional<std::span<const Position>> {
  switch (channels) {
    case 6:
      return std::span<const Position>{assumed_5_1};
    case 8:
    case 12:
      return std::span<const Position>{assumed_7_1_4};
    default:
      return std::nullopt;
  }
}

auto AudioOutputLayout::output_channels_for(std::size_t channels) noexcept
    -> std::size_t {
  switch (channels) {
    case 6:
      return 6;
    case 12:
      return max_output_channels;
    default:
      return 8;
  }
}

auto AudioOutputLayout::plan(std::size_t channels,
                             std::span<const Position> positions)
    -> Expected<AudioOutputLayout, AudioLayoutError> {
  if (channels < 1 || channels > net::protocol::remote_play::audio_max_channels)
    return unexpected(AudioLayoutError::invalid_channel_count);
  AudioOutputLayout layout;
  layout.input_channels_ = channels;
  if (channels <= 2) {
    layout.output_channels_ = channels;
    for (std::size_t index = 0; index < channels; ++index)
      layout.routes_[index] = {static_cast<std::uint8_t>(index), unity_gain};
    return layout;
  }
  layout.source_ = AudioLayoutSource::reported;
  if (!known(channels, positions)) {
    const auto fallback = assumed(channels);
    if (!fallback) return unexpected(AudioLayoutError::unknown_layout);
    positions = *fallback;
    layout.source_ = AudioLayoutSource::assumed;
  }
  layout.output_channels_ = output_channels_for(channels);
  for (std::size_t index = 0; index < channels; ++index) {
    const auto route = position_route(positions[index]);
    switch (layout.output_channels_) {
      case 6:
        layout.routes_[index] = {route.surround_5_1, route.gain};
        break;
      case max_output_channels:
        layout.routes_[index] = {route.surround_7_1_4, unity_gain};
        break;
      default:
        layout.routes_[index] = {route.surround_7_1, route.gain};
        break;
    }
  }
  return layout;
}

auto AudioOutputLayout::apply(std::span<const std::int16_t> input,
                              std::span<std::int16_t> output) const noexcept
    -> std::size_t {
  const auto frames = std::min(input.size() / input_channels_,
                               output.size() / output_channels_);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    std::array<std::int32_t, max_output_channels> mix{};
    const auto source = input.subspan(frame * input_channels_, input_channels_);
    for (std::size_t channel = 0; channel < input_channels_; ++channel) {
      const auto& route = routes_[channel];
      const auto scaled = std::int32_t{source[channel]} * route.gain;
      mix[route.target] += (scaled + unity_gain / 2) >> 15;
    }
    const auto destination =
        output.subspan(frame * output_channels_, output_channels_);
    for (std::size_t channel = 0; channel < output_channels_; ++channel)
      destination[channel] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
          mix[channel], std::numeric_limits<std::int16_t>::min(),
          std::numeric_limits<std::int16_t>::max()));
  }
  return frames;
}

}  // namespace libchiaking::media
