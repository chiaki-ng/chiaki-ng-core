#include "src/core/play/adapters/start/link_request.hpp"

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/consoles/paired_play.hpp"
#include "src/core/psn/gk/launch_spec.hpp"

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

auto CloudLinkConfiguration::credentials_valid(
    const CloudLinkRequest& launch) noexcept -> bool {
  return launch.user != nullptr && launch.user->has_psn_account() &&
         launch.catalog != nullptr;
}

auto CloudLinkConfiguration::create(CloudLinkRequest launch,
                                    const platform::UtcOffsetProvider& clock)
    -> Expected<CloudLinkConfiguration, ConfigurationError> {
  if (!credentials_valid(launch))
    return unexpected(ConfigurationError::invalid_credential);
  if (!launch.time_zone) {
    auto time_zone = psn::gk::TimeZone::text_for(clock);
    if (!time_zone)
      return unexpected(time_zone.error() ==
                                psn::gk::TimeZone::Unreadable::out_of_range
                            ? ConfigurationError::time_zone_out_of_range
                            : ConfigurationError::clock_unavailable);
    launch.time_zone = std::move(time_zone.value());
  }
  return create(std::move(launch));
}

auto CloudLinkConfiguration::create(CloudLinkRequest launch)
    -> Expected<CloudLinkConfiguration, ConfigurationError> {
  if (!credentials_valid(launch))
    return unexpected(ConfigurationError::invalid_credential);
  if (!launch.time_zone)
    return unexpected(ConfigurationError::clock_unavailable);
  if (!psn::gk::TimeZone::is_canonical(*launch.time_zone))
    return unexpected(ConfigurationError::time_zone_out_of_range);
  CloudLinkConfiguration configuration;
  configuration.user = std::move(launch.user);
  configuration.catalog = launch.catalog;
  configuration.product = std::move(launch.product);
  configuration.cleared = std::move(launch.cleared);
  configuration.datacenters = launch.datacenters;
  configuration.time_zone = std::move(*launch.time_zone);
  configuration.datacenter = launch.datacenter;
  configuration.prior_datacenters = std::move(launch.prior_datacenters);
  configuration.resolution = static_cast<int>(launch.video.height());
  configuration.bitrate_kbps = static_cast<int>(launch.video.bitrate_kbps());
  configuration.measure_bandwidth = launch.measure_bandwidth;
  configuration.hdr =
      launch.video.codec() == net::protocol::remote_play::VideoCodec::h265_hdr;
  if (!configuration.prefer_controller(launch.controller))
    return unexpected(ConfigurationError::rejected);
  configuration.feedback = launch.feedback;
  configuration.enable_idr_on_fec_failure = launch.idr_on_fec_failure;
  configuration.tuning = launch.tuning;
  configuration.high_quality = launch.quality == CloudQuality::high;
  configuration.accessibility = {
      .screen_reader = launch.accessibility.screen_reader(),
      .screen_reader_speed = launch.accessibility.screen_reader_speed(),
      .screen_reader_volume = launch.accessibility.screen_reader_volume(),
      .text_size = launch.accessibility.text_size(),
      .bold_text = launch.accessibility.bold_text(),
      .higher_contrast = launch.accessibility.higher_contrast()};
  auto accepted = std::move(configuration).validated();
  if (!accepted) return unexpected(ConfigurationError::rejected);
  return std::move(accepted.value());
}

}  // namespace libchiaking::play
