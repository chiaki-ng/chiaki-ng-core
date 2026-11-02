#include "src/core/net/protocol/takion/client/limits.hpp"

#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/numeric/checked.hpp"

#include <algorithm>
#include <limits>

namespace libchiaking::net::protocol::takion::client_detail {

std::optional<std::size_t> ClientLimits::active_profile_control_capacity(
    const TakionClientLimits& limits, const WireProfile& profile,
    const PacketEnvelope& envelope) noexcept {
  constexpr PacketType control_type{
      static_cast<std::uint8_t>(PacketFamily::control)};
  const auto datagram_budget =
      limits.maximum_control_datagram_size != 0
          ? std::min(limits.maximum_fragment_size,
                     limits.maximum_control_datagram_size)
          : limits.maximum_fragment_size;
  auto capacity = numeric::checked_sub(datagram_budget, packet_type_size);
  if (capacity) {
    const auto extended = profile.uses_extended_header(control_type)
                              ? extended_header_size
                              : std::size_t{0};
    capacity = numeric::checked_sub(*capacity, extended);
  }
  if (capacity && std::holds_alternative<PsnEnvelope>(envelope))
    capacity = numeric::checked_sub(*capacity, psn_envelope_size);
  if (capacity) capacity = numeric::checked_sub(*capacity, control_header_size);
  if (!capacity || *capacity == 0) return std::nullopt;
  return std::min(*capacity, static_cast<std::size_t>(
                                 std::numeric_limits<std::uint16_t>::max() -
                                 control_length_bias));
}

std::optional<std::size_t> ClientLimits::reliable_control_capacity(
    const ReliableChannelConfig& config) noexcept {
  if (config.maximum_message_size == 0 || config.maximum_fragment_count == 0)
    return std::nullopt;

  const auto first = numeric::checked_sub(config.maximum_fragment_size,
                                          first_fragment_overhead);
  const auto continuation = numeric::checked_sub(
      config.maximum_fragment_size, continuation_fragment_overhead);
  if (!first || !continuation) return std::nullopt;

  const auto remaining_fragments =
      numeric::checked_sub(config.maximum_fragment_count, std::size_t{1});
  if (!remaining_fragments) return std::nullopt;
  const auto remaining_capacity =
      numeric::checked_mul(*remaining_fragments, *continuation);
  if (!remaining_capacity) return std::nullopt;
  const auto aggregate = numeric::checked_add(*first, *remaining_capacity);
  if (!aggregate) return std::nullopt;
  return std::min(config.maximum_message_size, *aggregate);
}

std::optional<std::size_t> ClientLimits::effective_control_message_limit(
    std::size_t configured_limit, std::size_t reliable_limit,
    std::size_t protobuf_backend_limit) noexcept {
  if (configured_limit == 0 || reliable_limit == 0 ||
      protobuf_backend_limit == 0) {
    return std::nullopt;
  }
  return std::min(configured_limit,
                  std::min(reliable_limit, protobuf_backend_limit));
}

bool ClientLimits::valid_limits(const TakionClientLimits& limits) noexcept {
  return limits.mailbox_capacity != 0 &&
         limits.maximum_commands_per_pump != 0 &&
         limits.maximum_datagrams_per_pump != 0 &&
         limits.maximum_message_size != 0 &&
         limits.maximum_fragment_size >= first_fragment_overhead &&
         limits.maximum_fragment_count != 0 &&
         limits.retransmission_window_size != 0 &&
         limits.receive_reorder_size != 0 &&
         limits.retransmission_timeout_us != 0 &&
         limits.minimum_retransmission_timeout_us != 0 &&
         limits.minimum_retransmission_timeout_us <=
             limits.maximum_retransmission_timeout_us &&
         limits.unsent_datagram_capacity != 0 &&
         limits.unsent_retry_interval_us != 0;
}

bool ClientLimits::control_path_is_valid(const ClientConfig& config,
                                         std::uint32_t version) {
  const auto resolved = resolve_takion_wire_profile(version);
  if (!resolved) return false;
  const auto active = active_profile_control_capacity(config.limits, *resolved,
                                                      config.envelope);
  if (!active) return false;
  const ReliableChannelConfig reliable_config{
      {},
      {},
      config.limits.maximum_message_size,
      *active,
      config.limits.maximum_fragment_count,
      config.limits.retransmission_window_size,
      config.limits.receive_reorder_size,
      config.limits.retransmission_timeout_us,
      config.limits.maximum_retries,
      config.limits.minimum_retransmission_timeout_us,
      config.limits.maximum_retransmission_timeout_us,
  };
  const auto reliable = reliable_control_capacity(reliable_config);
  if (!reliable) return false;
  return effective_control_message_limit(
             config.limits.maximum_message_size, *reliable,
             ControlCodec{}.effective_message_size_limit())
      .has_value();
}

bool ClientLimits::validate_client_config(const ClientConfig& config) {
  const auto bootstrap = resolve_takion_wire_profile(config.bootstrap_version);
  if (!bootstrap) return false;
  if (!valid_limits(config.limits)) return false;
  if (!control_path_is_valid(config, config.bootstrap_version)) return false;
  if (const auto* fixed =
          std::get_if<FixedTakionProfile>(&config.profile_selection)) {
    return control_path_is_valid(config, fixed->version);
  }
  const auto& versions =
      std::get<NegotiatedTakionProfiles>(config.profile_selection)
          .supported_versions;
  if (versions.empty()) return false;
  if (std::any_of(versions.begin(), versions.end(), [](std::uint32_t version) {
        return !is_implemented_takion_version(version);
      }))
    return false;
  return std::all_of(versions.begin(), versions.end(),
                     [&](std::uint32_t version) {
                       return control_path_is_valid(config, version);
                     });
}

}  // namespace libchiaking::net::protocol::takion::client_detail
