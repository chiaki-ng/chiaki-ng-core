#include "src/core/kakusan/adapters/shared_setup.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace libchiaking::kakusan {

static_assert(media::StreamDescription::audio_header_size ==
              net::kakusan::control::audio_header_size);
static_assert(play::ReceiveKeys::size ==
              net::kakusan::control::SourceKey::extent);

auto SharedSetup::build(const CapturedKeys& keys,
                        const media::StreamDescription& description,
                        std::uint32_t video_fps, SessionId source_session_id)
    -> net::kakusan::Result<net::kakusan::control::SourceSetup> {
  if (keys.version() > std::numeric_limits<std::uint8_t>::max() ||
      !play::SessionShare::shares_protocol_version(keys.version()))
    return net::kakusan::fail(net::kakusan::Error::version);
  if (!net::kakusan::control::SourceSetup::valid_fps(video_fps) ||
      description.profiles.empty() ||
      description.profiles.size() > net::kakusan::control::max_video_profiles ||
      (description.picture &&
       description.picture->codec == media::VideoCodec::h265_hdr))
    return net::kakusan::fail(net::kakusan::Error::codec);
  const auto codec = net::kakusan::control::SourceSetup::detect_codec(
      description.profiles.front().header);
  if (!codec) return net::kakusan::fail(codec.error());
  net::kakusan::control::SourceSetup setup;
  setup.source_codec = codec.value();
  std::ranges::copy(source_session_id, setup.source_session_id.begin());
  setup.source_protocol = static_cast<std::uint8_t>(keys.version());
  std::ranges::copy(keys.key(), setup.receive_key.begin());
  std::ranges::copy(keys.iv(), setup.receive_iv.begin());
  setup.video_fps = static_cast<std::uint8_t>(video_fps);
  setup.video_profiles.reserve(description.profiles.size());
  for (const auto& profile : description.profiles) {
    if (!net::kakusan::control::SourceSetup::valid_geometry(
            profile.width, profile.height, profile.header.size()) ||
        !net::kakusan::control::SourceSetup::validate_profile_header(
            setup.source_codec, profile.header))
      return net::kakusan::fail(net::kakusan::Error::codec);
    setup.video_profiles.push_back(net::kakusan::control::VideoProfile{
        static_cast<std::uint16_t>(profile.width),
        static_cast<std::uint16_t>(profile.height), profile.header});
  }
  setup.audio_header = description.audio_header;
  if (const auto audio =
          net::kakusan::control::SourceSetup::validate_audio_header(
              setup.audio_header);
      !audio)
    return net::kakusan::fail(audio.error());
  return setup;
}

auto SharedSetup::stream(net::kakusan::control::SourceSetup setup)
    -> net::kakusan::Result<Stream> {
  if (const auto valid = setup.validate(); !valid)
    return net::kakusan::fail(valid.error());
  if (const auto headers = setup.validate_profile_headers(); !headers)
    return net::kakusan::fail(headers.error());
  play::SharedStream shared;
  shared.protocol_version = setup.source_protocol;
  shared.hevc = setup.source_codec == net::kakusan::control::SourceCodec::h265;
  shared.frames_per_second = setup.video_fps;
  shared.description.audio_header = setup.audio_header;
  shared.description.profiles.reserve(setup.video_profiles.size());
  for (auto& profile : setup.video_profiles)
    shared.description.profiles.push_back(media::VideoProfile{
        profile.width, profile.height, std::move(profile.header)});
  return Stream{std::move(shared),
                play::ReceiveKeys{std::move(setup.receive_key).secret(),
                                  std::move(setup.receive_iv).secret()}};
}

}  // namespace libchiaking::kakusan
