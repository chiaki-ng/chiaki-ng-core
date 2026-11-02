/// @file
/// @brief Declares ClientLimits, the size arithmetic that checks a Takion
/// client's configuration can carry control messages at every version it may
/// use.
#pragma once

#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"

#include <cstddef>
#include <optional>

namespace libchiaking::net::protocol::takion::client_detail {

/// @brief Size budgets of a Takion client's control path. Stateless; safe from
/// any thread.
class ClientLimits {
 public:
  /// @brief Computes the largest DATA chunk body one control datagram can
  /// carry.
  ///
  /// @param limits The client's limits; the smaller of the fragment size and
  ///     the control datagram size (when set) is the budget.
  /// @param profile The version's layout.
  /// @param envelope The sending framing.
  /// @return The budget less the type byte, any extended header, any 4-byte
  ///     envelope and the 16 bytes of control headers, capped at 65531; nothing
  ///     when nothing is left.
  [[nodiscard]] static std::optional<std::size_t>
  active_profile_control_capacity(const TakionClientLimits& limits,
                                  const WireProfile& profile,
                                  const PacketEnvelope& envelope) noexcept;

  /// @brief Computes the largest message the reliable channel can fragment.
  ///
  /// @param config The channel configuration.
  /// @return The smaller of the message limit and one first fragment plus the
  ///     remaining fragments' capacity; nothing when a size is zero or too
  ///     small.
  [[nodiscard]] static std::optional<std::size_t> reliable_control_capacity(
      const ReliableChannelConfig& config) noexcept;

  /// @brief Picks the control message limit in force.
  ///
  /// @param configured_limit The configured limit.
  /// @param reliable_limit What the reliable channel can carry.
  /// @param protobuf_backend_limit What protobuf can parse.
  /// @return The smallest; nothing when any is zero.
  [[nodiscard]] static std::optional<std::size_t>
  effective_control_message_limit(std::size_t configured_limit,
                                  std::size_t reliable_limit,
                                  std::size_t protobuf_backend_limit) noexcept;

  /// @brief Checks the limits themselves.
  ///
  /// @param limits The limits.
  /// @return Whether every count and timeout is non-zero, the fragment size at
  ///     least 9 and the timeout clamps ordered.
  static bool valid_limits(const TakionClientLimits& limits) noexcept;

  /// @brief Checks a whole configuration.
  ///
  /// @param config The configuration.
  /// @return Whether the bootstrap version and every fixed or offered version
  /// is
  ///     implemented, the limits are valid, and each version's control path can
  ///     carry a message.
  static bool validate_client_config(const ClientConfig& config);

 private:
  /// @brief Checks one version's control path.
  ///
  /// @param config The configuration.
  /// @param version The version.
  /// @return Whether it is implemented and a control message fits.
  static bool control_path_is_valid(const ClientConfig& config,
                                    std::uint32_t version);
};

}  // namespace libchiaking::net::protocol::takion::client_detail
