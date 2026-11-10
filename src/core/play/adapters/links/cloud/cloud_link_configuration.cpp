#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"

#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "src/core/psn/gk/launch_spec.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::play {

auto CloudAllocation::valid() const noexcept -> bool {
  try {
    return service <= cloud::CloudService::cloud_stream &&
           platform <= cloud::CloudPlatform::ps5 && !server_ip.empty() &&
           !handshake_key.empty() &&
           play::HandshakeKey::decode(handshake_key.view()).has_value() &&
           !launch_spec.empty() &&
           launch_spec.view().size() <= launch_spec_limit &&
           launch_spec.view().find('\0') == std::string_view::npos;
  } catch (...) {
    return false;
  }
}

auto CloudLinkConfiguration::validated() && -> Expected<
    CloudLinkConfiguration, net::protocol::remote_play::StreamError> {
  const bool provisions = !allocation.has_value();
  if ((allocation && !allocation->valid()) ||
      (provisions && (user == nullptr || !user->has_psn_account() ||
                      catalog == nullptr || !product)) ||
      time_zone.size() > locale_text_limit ||
      time_zone.find('\0') != std::string::npos ||
      (time_zone.empty() && provisions) || bitrate_kbps <= 0)
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_configuration);
  if (feedback >
          net::protocol::remote_play::FeedbackMode::switch_compatibility ||
      !tuning.valid())
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_configuration);
  return std::move(*this);
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
