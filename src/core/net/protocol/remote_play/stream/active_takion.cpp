#include "src/core/net/protocol/remote_play/stream/active_takion.hpp"

#include "src/core/net/protocol/takion/control_chunk_codec.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

ActiveTakion::ActiveTakion(std::unique_ptr<takion::Client> client,
                           EstablishedRoute route) noexcept
    : client_(std::move(client)), route_(std::move(route)) {}

auto ActiveTakion::start(EstablishedRoute route, platform::ClockProvider& clock,
                         platform::ThreadingProvider& threading,
                         platform::SecureRandomProvider& random,
                         takion::EventSink& events, takion::ClientConfig config,
                         ActiveDatagramBudget budget)
    -> Expected<std::unique_ptr<ActiveTakion>, ActiveTakionError> {
  if (!route.transport) return unexpected(ActiveTakionError::invalid_route);
  const auto version = route.calibration.takion_version;
  if (version != route.calibration.wire_profile.version())
    return unexpected(ActiveTakionError::version_mismatch);
  if (!takion::is_implemented_takion_version(version))
    return unexpected(ActiveTakionError::unsupported_version);
  if (route.calibration.inbound_mtu == 0 || route.calibration.outbound_mtu == 0)
    return unexpected(ActiveTakionError::invalid_mtu);

  const std::uint64_t mtu =
      std::min(route.calibration.inbound_mtu, route.calibration.outbound_mtu);
  const std::uint64_t reserved =
      static_cast<std::uint64_t>(budget.network_overhead) +
      budget.reserved_envelope_bytes;
  const std::size_t envelope =
      std::holds_alternative<takion::PsnEnvelope>(config.envelope) ? 4 : 0;
  constexpr takion::PacketType control_type{
      static_cast<std::uint8_t>(takion::PacketFamily::control)};
  const auto minimum =
      route.calibration.wire_profile.packet_header_size(control_type) +
      takion::control_header_size + takion::HandshakeCookie{}.size();
  if (mtu <= reserved || mtu - reserved < minimum)
    return unexpected(ActiveTakionError::invalid_mtu);
  config.limits.maximum_fragment_size =
      static_cast<std::size_t>(mtu - reserved) + envelope;

  config.bootstrap_version = version;
  if (!std::holds_alternative<takion::NegotiatedTakionProfiles>(
          config.profile_selection))
    config.profile_selection = takion::FixedTakionProfile{version};
  config.crypto_policy = takion::CryptoPolicy::required;
  config.allow_plain_control_until_crypto = true;
  auto client = std::make_unique<takion::Client>(route.transport.release(),
                                                 clock, threading, random,
                                                 events, std::move(config));
  if (!client->start()) return unexpected(ActiveTakionError::start_failed);
  return std::unique_ptr<ActiveTakion>{
      new ActiveTakion(std::move(client), std::move(route))};
}

}  // namespace libchiaking::net::protocol::remote_play
