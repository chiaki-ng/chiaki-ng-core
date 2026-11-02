/// @file
/// @brief Declares ServerSlot, the streaming server a cloud launch reserved.
#pragma once

#include "src/core/psn/gk/client.hpp"
#include "src/core/psn/gk/measurement.hpp"
#include "src/core/psn/types.hpp"

#include <string>
#include <vector>

namespace libchiaking::play {

/// @brief A reserved cloud streaming server and how it was reached. Plain
/// data returned by a successful launch.
struct ServerSlot {
  /// @brief The datacenter the server is in, with its measurement.
  psn::gk::Ping datacenter;
  /// @brief The rows shown to the player for the datacenter choice.
  std::vector<psn::gk::Ping> picker_rows;
  /// @brief PS's allocation: the server's address, handshake key, launch
  /// specification and session id.
  psn::gk::Allocation allocation;
  /// @brief The service that reserved it.
  psn::StreamingService service{psn::StreamingService::legacy_cloud_stream};
  /// @brief The platform the title streams as.
  psn::StreamingPlatform platform{psn::StreamingPlatform::ps4};
  /// @brief The entitlement launched.
  std::string entitlement_id{};
};

}  // namespace libchiaking::play
