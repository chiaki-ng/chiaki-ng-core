#include "src/core/play/adapters/start/link_request.hpp"

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/consoles/paired_play.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace libchiaking::play {

auto VideoRequest::create(std::uint32_t height, std::uint32_t fps,
                          std::uint32_t bitrate_kbps,
                          net::protocol::remote_play::VideoCodec codec,
                          const VideoRequestLimits& limits)
    -> Expected<VideoRequest, ConfigurationError> {
  const bool hevc = codec == net::protocol::remote_play::VideoCodec::h265 ||
                    codec == net::protocol::remote_play::VideoCodec::h265_hdr;
  const bool known_codec =
      hevc || codec == net::protocol::remote_play::VideoCodec::h264;
  const bool known_height = std::ranges::find(heights, height) != heights.end();
  const bool allowed_height = height >= limits.minimum_height &&
                              (height <= limits.maximum_height ||
                               (hevc && height <= limits.maximum_hevc_height));
  if (!known_codec || !known_height || !allowed_height ||
      (fps != video_fps_30 && fps != video_fps_60) ||
      bitrate_kbps < limits.minimum_bitrate_kbps ||
      bitrate_kbps > limits.maximum_bitrate_kbps)
    return unexpected(ConfigurationError::invalid_video);
  return VideoRequest{height, fps, bitrate_kbps, codec};
}

auto HomeLinkConfiguration::create(const consoles::Console& console,
                                   const users::PsUser& as, VideoRequest video,
                                   ConsolePreferences preferences)
    -> Expected<HomeLinkConfiguration, ConfigurationError> {
  using Refusal = consoles::PairedPlay::Refusal;
  auto target = consoles::PairedPlay{console, as}.at_home();
  if (!target) {
    switch (target.error()) {
      case Refusal::no_address:
        return unexpected(ConfigurationError::invalid_host);
      case Refusal::forgotten:
        return unexpected(ConfigurationError::invalid_console);
      default:
        break;
    }
    return unexpected(ConfigurationError::not_paired);
  }
  HomeLinkConfiguration configuration;
  configuration.host = target.value().host.text();
  configuration.console.target = target.value().firmware.target();
  std::ranges::copy(target.value().keys.registration_key(),
                    configuration.console.registration_key.begin());
  std::ranges::copy(target.value().keys.remote_play_key(),
                    configuration.console.key.begin());
  configuration.ctrl_endpoint.port = rp_session_port;
  configuration.active_stream_port = rp_stream_port;
  configuration.video = video.settings();
  configuration.codec = video.codec_for(console.generation());
  configuration.feedback = preferences.feedback;
  configuration.enable_keyboard = preferences.keyboard;
  configuration.enable_idr_on_fec_failure = preferences.idr_on_fec_failure;
  configuration.tuning = preferences.tuning;
  if (!configuration.prefer_controller(preferences.controller))
    return unexpected(ConfigurationError::rejected);
  auto accepted = std::move(configuration).validated();
  if (!accepted) return unexpected(ConfigurationError::rejected);
  return std::move(accepted.value());
}

}  // namespace libchiaking::play
