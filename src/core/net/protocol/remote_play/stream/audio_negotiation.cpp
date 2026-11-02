#include "src/core/net/protocol/remote_play/stream/audio_negotiation.hpp"

#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

void AudioProfileSelector::configure(const ValidatedStreamInfo& info,
                                     takion::FeatureSet features,
                                     std::uint32_t local_mode) {
  reset();
  channels_.reserve(info.audio_channels.size());
  for (const auto& setup : info.audio_channels)
    channels_.push_back(ChannelState{setup, setup.header});
  delivered_header_ = info.audio_header;
  selection_enabled_ = selection_enabled(features, local_mode);
}

auto AudioProfileSelector::select(std::optional<std::uint8_t> channel,
                                  std::uint8_t requested_channels)
    -> std::optional<AudioProfileChange> {
  if (channel) {
    const auto found = std::find_if(
        channels_.begin(), channels_.end(),
        [&](const auto& state) { return state.setup.type == *channel; });
    if (found != channels_.end())
      active_ = static_cast<std::size_t>(found - channels_.begin());
  }
  if (!active_) return std::nullopt;
  auto& state = channels_[*active_];
  const auto current_channels = std::to_integer<std::uint8_t>(state.current[0]);
  if (selection_enabled_ && requested_channels != current_channels) {
    const auto profile = state.setup.profiles.find(requested_channels);
    if (profile != state.setup.profiles.end()) state.current = profile->second;
  }
  if (state.current == delivered_header_ &&
      state.setup.raw_pcm == delivered_raw_pcm_)
    return std::nullopt;
  delivered_header_ = state.current;
  delivered_raw_pcm_ = state.setup.raw_pcm;
  return AudioProfileChange{delivered_header_, delivered_raw_pcm_};
}

auto AudioProfileSelector::header_of(std::uint8_t channel) const
    -> std::optional<AudioProfileChange> {
  const auto found = std::find_if(
      channels_.begin(), channels_.end(),
      [&](const auto& state) { return state.setup.type == channel; });
  if (found == channels_.end()) return std::nullopt;
  return AudioProfileChange{found->current, found->setup.raw_pcm};
}

void AudioProfileSelector::reset() noexcept {
  channels_.clear();
  active_.reset();
  delivered_header_ = {};
  delivered_raw_pcm_ = false;
  selection_enabled_ = false;
}

auto AudioStateCodec::build(AudioStateKind kind,
                            std::span<const std::byte> data,
                            takion::FeatureSet features)
    -> Expected<takion::AudioState, AudioStateError> {
  if (!AudioProfileSelector::has_feature(features,
                                         AudioFeature::channel_profiles))
    return unexpected(AudioStateError::unsupported_by_version);
  if (kind > AudioStateKind::channel_count)
    return unexpected(AudioStateError::unsupported_kind);
  if (kind == AudioStateKind::channel_count &&
      !AudioProfileSelector::has_feature(features,
                                         AudioFeature::mode_channel_profiles))
    return unexpected(AudioStateError::unsupported_by_version);
  std::vector<std::byte> body(data.begin(), data.end());
  const auto available_bits =
      std::span{body}.subspan(std::min(available_bits_offset, body.size()));
  if (kind <= AudioStateKind::angle &&
      !AudioProfileSelector::has_feature(features,
                                         AudioFeature::wide_available_bits) &&
      available_bits.size() >= 4 &&
      bytes::load_le<std::uint32_t>(available_bits) > narrow_available_bits)
    bytes::store_le<std::uint32_t>(available_bits, narrow_available_bits);
  return takion::AudioState{
      static_cast<takion::AudioStateType>(static_cast<std::uint32_t>(kind)),
      std::move(body)};
}

}  // namespace libchiaking::net::protocol::remote_play
