/// @file
/// @brief Declares DatacenterSelector, the port through which the player's
/// datacenter pick reaches a running cloud launch.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <string_view>

namespace libchiaking::play {

/// @brief Something that can pass a datacenter pick on to a cloud link.
///
/// Implemented by the session core and the link driver, which forward the
/// pick down to the link. Called from the frontend's thread while the link
/// runs on the session's.
class DatacenterSelector {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~DatacenterSelector() = default;
  /// @brief Passes the player's datacenter pick to the link.
  ///
  /// @param name An offered datacenter name, or DatacenterName::automatic.
  /// @return Nothing when the link took the pick; the link's StreamError
  ///     otherwise, for example when no cloud launch is waiting for one.
  virtual auto select_datacenter(std::string_view name)
      -> Expected<void, net::protocol::remote_play::StreamError> = 0;
};

}  // namespace libchiaking::play
