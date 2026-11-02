#include "stream_info.hpp"

#include "src/core/net/protocol/remote_play/stream/audio_negotiation.hpp"
#include "src/core/utils/bytes/byte_io.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto StreamInfoCodec::header_bytes(std::span<const std::byte> bytes) noexcept
    -> AudioHeaderBytes {
  AudioHeaderBytes result{};
  std::copy_n(bytes.begin(), audio_header_size, result.begin());
  return result;
}

auto StreamInfoCodec::channel_setups(
    std::vector<takion::StreamInfoAudioChannel>& channels,
    takion::FeatureSet features) -> std::vector<AudioChannelSetup> {
  std::vector<AudioChannelSetup> setups;
  if (!AudioProfileSelector::has_feature(features, AudioFeature::channel_setup))
    return setups;
  const bool profiles = AudioProfileSelector::has_feature(
      features, AudioFeature::channel_profiles);
  for (const auto& channel : channels) {
    if (channel.audio_header.size() < audio_header_size ||
        std::any_of(setups.begin(), setups.end(), [&](const auto& setup) {
          return setup.type == channel.audio_channel_type;
        }))
      continue;
    AudioChannelSetup setup;
    setup.type = channel.audio_channel_type;
    setup.header = header_bytes(channel.audio_header);
    setup.raw_pcm = channel.is_raw_pcm.value_or(false);
    if (profiles) {
      setup.fec_mode = channel.fec_mode;
      for (const auto& profile : channel.profile_headers) {
        const auto parsed = parse_audio_header(profile);
        if (parsed && parsed->channels != 0)
          setup.profiles[parsed->channels] = header_bytes(profile);
      }
    }
    setups.push_back(std::move(setup));
  }
  return setups;
}

auto StreamInfoCodec::parse_audio_header(
    std::span<const std::byte> bytes) noexcept -> std::optional<AudioHeader> {
  if (bytes.size() < audio_header_size) return std::nullopt;
  bytes::Reader reader{bytes};
  AudioHeader audio;
  audio.channels = *reader.u8();
  audio.bits = *reader.u8();
  audio.rate = *reader.u32_be();
  audio.frame_size = *reader.u32_be();
  audio.unknown = *reader.u32_be();
  return audio;
}

auto StreamInfoCodec::to_public_audio_format(const AudioHeader& header) noexcept
    -> event::AudioFormat {
  event::AudioFormat format{header.channels, header.bits, header.rate,
                            header.frame_size};
  if (const auto layout = opus_multistream_layout(header.channels)) {
    format.streams = layout->streams;
    format.coupled_streams = layout->coupled_streams;
    format.mapping = layout->mapping;
    format.positions = layout->positions;
  }
  return format;
}

auto StreamInfoCodec::validate(takion::StreamInfo info,
                               takion::FeatureSet features)
    -> Expected<ValidatedStreamInfo, StreamInfoError> {
  const std::span<const std::byte> header{info.audio_header};
  if (header.size() != audio_header_size)
    return unexpected(StreamInfoError::invalid_audio_header);
  const auto audio = *parse_audio_header(header);

  std::vector<takion::StreamInfoResolution> resolutions;
  resolutions.reserve(std::min(info.resolutions.size(), max_video_profiles));
  for (auto& resolution : info.resolutions) {
    if (resolutions.size() == max_video_profiles) break;
    if (resolution.video_header.empty()) continue;
    resolutions.push_back(std::move(resolution));
  }
  ValidatedStreamInfo validated{audio, std::move(resolutions),
                                header_bytes(header),
                                channel_setups(info.audio_channels, features),
                                info.congestion_control_interval};
  return validated;
}

}  // namespace libchiaking::net::protocol::remote_play
