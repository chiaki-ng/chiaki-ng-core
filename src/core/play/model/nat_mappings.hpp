/// @file
/// @brief Declares NatMappings, the public mappings two STUN servers saw for
/// one socket, and the NAT type they show.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/nat_type.hpp"
#include "libchiaking/platform/net/types.hpp"

#include <optional>
#include <vector>

namespace libchiaking::play {

/// @brief What two STUN servers saw of one socket, with the host's own
/// addresses, which together decide the NAT type.
///
/// A mapping equal to one of the host's own addresses is type 1; the same
/// mapping from both servers is type 2; different mappings are type 3. Plain
/// data.
struct NatMappings {
  /// @brief The mapping the first server saw.
  platform::SocketEndpoint first{};
  /// @brief The mapping the second server saw, or nothing when it did not
  /// answer.
  std::optional<platform::SocketEndpoint> second{};
  /// @brief The host's own IPv4 addresses.
  std::vector<platform::IpAddress> local{};

  /// @brief Decides the NAT type the mappings show.
  ///
  /// @return Type 1 when the first mapping's address is the host's own, type
  ///     2 when both servers saw the same mapping, type 3 otherwise.
  /// @retval NatTypeFailure::Reason::incomplete The host is behind a router
  ///     and the second server did not answer.
  [[nodiscard]] auto type() const -> Expected<NatType, NatTypeFailure>;
};

}  // namespace libchiaking::play
