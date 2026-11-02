#include "libchiaking/detail/play/preferences.hpp"

#include "src/core/net/protocol/takion/profile.hpp"

namespace libchiaking::play {

auto DeveloperTuning::built_protocol_version(
    consoles::Generation generation) noexcept -> std::uint32_t {
  return net::protocol::takion::takion_build_version(
      generation == consoles::Generation::ps5
          ? net::protocol::takion::BuildTarget::ps5
          : net::protocol::takion::BuildTarget::ps4);
}

auto DeveloperTuning::built_protocol_version(
    catalog::ServiceGeneration service) noexcept -> std::uint32_t {
  return net::protocol::takion::takion_build_version(
      service == catalog::ServiceGeneration::ps5_cloud
          ? net::protocol::takion::BuildTarget::cloud_stream
          : net::protocol::takion::BuildTarget::legacy_cloud_stream);
}

auto DeveloperTuning::implemented_protocol_versions(consoles::Generation)
    -> std::vector<std::uint32_t> {
  return net::protocol::takion::takion_versions_available_to(
      net::protocol::takion::SessionScope::console);
}

auto DeveloperTuning::implemented_protocol_versions(
    catalog::ServiceGeneration service) -> std::vector<std::uint32_t> {
  return net::protocol::takion::takion_versions_available_to(
      service == catalog::ServiceGeneration::ps5_cloud
          ? net::protocol::takion::SessionScope::cloud_stream
          : net::protocol::takion::SessionScope::legacy_cloud_stream);
}

auto DeveloperTuning::build_protocol_override() noexcept
    -> std::optional<std::uint32_t> {
  if (net::protocol::takion::takion_build_version_override == 0)
    return std::nullopt;
  return net::protocol::takion::takion_build_version_override;
}

}  // namespace libchiaking::play
