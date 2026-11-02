/// @file
/// @brief Declares NatType, the host's NAT type, and NatTypeFailure, why it
/// could not be determined.
#pragma once

#include <cstdint>

namespace libchiaking::play {

/// @brief How reachable this host is from the internet, as type 1, 2 or 3.
///
/// @see Play::nat_type
enum class NatType : std::uint8_t {
  /// @brief Type 1: the host has a public address of its own.
  type1 = 1,
  /// @brief Type 2: behind a router that keeps one public mapping for every
  /// destination, so peers can reach it.
  type2 = 2,
  /// @brief Type 3: behind a router that gives each destination its own
  /// mapping, so peers can rarely reach it.
  type3 = 3,
};

/// @brief Why the NAT type could not be determined.
struct NatTypeFailure {
  /// @brief The reason.
  enum class Reason : std::uint8_t {
    /// @brief The stop token fired.
    cancelled,
    /// @brief A UDP socket could not be opened or bound.
    network,
    /// @brief The STUN servers' names did not resolve.
    unresolved,
    /// @brief No STUN server answered, so UDP may be blocked.
    unanswered,
    /// @brief The first server answered but the second, needed to tell
    /// type 2 from type 3, did not.
    incomplete,
  };

  /// @brief The reason.
  Reason reason{Reason::unanswered};
};

}  // namespace libchiaking::play
