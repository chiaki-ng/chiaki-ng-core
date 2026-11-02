#include "src/core/play/adapters/links/link_readings.hpp"

#include "src/core/net/protocol/takion/profile.hpp"

namespace libchiaking::play {

auto LinkReadings::ecdh_curve() const noexcept
    -> std::optional<platform::EcdhCurve> {
  const auto version = takion_version();
  if (!version) return std::nullopt;
  const auto profile =
      net::protocol::takion::resolve_takion_wire_profile(*version);
  if (!profile) return std::nullopt;
  return profile->ecdh_curve();
}

}  // namespace libchiaking::play
