#include "src/core/net/protocol/senkusha/validator.hpp"
#include "src/core/net/protocol/takion/limits.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <variant>

namespace libchiaking::net::protocol::senkusha {
template <class Duration>
bool Validator::positive(Duration value) noexcept {
  return value.count() > 0;
}

bool Validator::valid_version(std::uint32_t version) noexcept {
  return version != 0 && takion::is_takion_version_available(
                             version, takion::SessionScope::console);
}

auto Validator::validate_configuration(const Profile& profile,
                                       const Policy& policy)
    -> Expected<void, Error> {
  if (profile.big_response == BigResponsePolicy::fragmented_big_echo) {
    const auto* fixed =
        std::get_if<takion::FixedTakionProfile>(&profile.profile_selection);
    if (!fixed || fixed->version != takion::senkusha_client_takion_version ||
        profile.bootstrap_version != takion::senkusha_client_takion_version ||
        profile.big.client_version != takion::senkusha_client_takion_version ||
        !profile.big.launch_spec.empty() ||
        !profile.big.encrypted_key.empty() || profile.big.session_key.empty() ||
        profile.big.ecdh_pub_key || profile.big.ecdh_sig)
      return unexpected(Error::invalid_configuration);
  } else if (profile.big_response != BigResponsePolicy::typed_bang) {
    return unexpected(Error::unsupported_big_response);
  }
  if (profile.big.client_version == 0 ||
      profile.big.ecdh_pub_key.has_value() != profile.big.ecdh_sig.has_value())
    return unexpected(Error::invalid_configuration);
  if (!valid_version(profile.bootstrap_version))
    return unexpected(Error::unsupported_version);

  if (const auto* fixed =
          std::get_if<takion::FixedTakionProfile>(&profile.profile_selection)) {
    if (!valid_version(fixed->version))
      return unexpected(Error::unsupported_version);
  } else {
    const auto& versions =
        std::get<takion::NegotiatedTakionProfiles>(profile.profile_selection)
            .supported_versions;
    if (versions.empty()) return unexpected(Error::invalid_configuration);
    for (auto version = versions.begin(); version != versions.end();
         ++version) {
      if (!valid_version(*version))
        return unexpected(Error::unsupported_version);
      if (std::ranges::find(std::next(version), versions.end(), *version) !=
          versions.end())
        return unexpected(Error::duplicate_version);
    }
  }

  if (!positive(policy.connect_timeout) || !positive(policy.response_timeout) ||
      policy.probe.count == 0 ||
      policy.probe.count > takion::max_units_in_frame ||
      !positive(policy.probe.timeout) ||
      policy.mtu.attempts_per_candidate == 0 ||
      policy.mtu.timeout_multiplier == 0 ||
      !positive(policy.mtu.timeout_floor) ||
      !positive(policy.mtu.timeout_ceiling) ||
      policy.mtu.timeout_floor > policy.mtu.timeout_ceiling ||
      profile.rtt_adjustment.subtract.count() < 0 ||
      profile.rtt_adjustment.floor.count() < 0) {
    return unexpected(Error::invalid_configuration);
  }

  const auto& mtu = policy.mtu;
  if (mtu.lower_bound == 0 || mtu.lower_bound >= mtu.upper_bound)
    return unexpected(Error::invalid_configuration);
  const auto minimum_probe_size =
      static_cast<std::uint64_t>(mtu.network_overhead) +
      static_cast<std::uint64_t>(mtu_av_header_size) +
      probe_minimum_payload_size;
  if (minimum_probe_size > std::numeric_limits<std::uint32_t>::max())
    return unexpected(Error::arithmetic_overflow);
  if (minimum_probe_size > mtu.lower_bound)
    return unexpected(Error::invalid_configuration);

  const auto tag_end = static_cast<std::uint64_t>(policy.probe.tag_offset) + 4U;
  if (tag_end > std::numeric_limits<std::uint32_t>::max())
    return unexpected(Error::arithmetic_overflow);
  if (tag_end > policy.probe.minimum_payload_size)
    return unexpected(Error::invalid_configuration);

  return {};
}

auto Validator::validate_bang(const Profile& profile, const takion::Bang& bang,
                              std::uint32_t committed_version)
    -> Expected<void, Error> {
  if (!valid_version(committed_version))
    return unexpected(Error::unsupported_version);
  if (profile.bang_validation.require_version_accepted &&
      !bang.version_accepted)
    return unexpected(Error::negotiation_failed);
  if (profile.bang_validation.require_encrypted_key_accepted &&
      !bang.encrypted_key_accepted)
    return unexpected(Error::negotiation_failed);

  if (!bang.protocol_ack && profile.bang_validation.require_protocol_ack)
    return unexpected(Error::malformed_response);
  return {};
}

}  // namespace libchiaking::net::protocol::senkusha
