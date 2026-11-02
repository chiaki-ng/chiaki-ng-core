#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"

#include <cstddef>
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

auto HomeLinkConfiguration::validated() && -> Expected<
    HomeLinkConfiguration, net::protocol::remote_play::StreamError> {
  if (host.empty() || host.size() > host_limit ||
      host.find('\0') != std::string::npos ||
      console.target > net::protocol::remote_play::RegistrationTarget::ps5_1 ||
      codec > net::protocol::remote_play::VideoCodec::h265_hdr ||
      feedback >
          net::protocol::remote_play::FeedbackMode::switch_compatibility ||
      ctrl_endpoint.port == 0 || active_stream_port == 0 || !tuning.valid())
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_configuration);
  return std::move(*this);
}

auto InternetLinkConfiguration::validated() && -> Expected<
    InternetLinkConfiguration, net::protocol::remote_play::StreamError> {
  const bool remote_target =
      console.target ==
          net::protocol::remote_play::RegistrationTarget::ps4_10 ||
      console.target == net::protocol::remote_play::RegistrationTarget::ps5_1;
  if (user == nullptr || !user->has_psn_account() || !remote_target ||
      codec > net::protocol::remote_play::VideoCodec::h265_hdr ||
      feedback >
          net::protocol::remote_play::FeedbackMode::switch_compatibility ||
      start_timeout_us == 0 || port_guess_count > port_guess_limit ||
      nat_probe_socket_count > nat_probe_socket_limit || !tuning.valid())
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_configuration);
  return std::move(*this);
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

}  // namespace libchiaking::play
