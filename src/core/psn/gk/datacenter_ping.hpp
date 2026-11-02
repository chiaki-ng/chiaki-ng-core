/// @file
/// @brief Names the cloud context's view of the streaming service and platform,
/// and brings in cloud::DatacenterPing, a measured GK datacenter.
#pragma once

#include "src/core/net/protocol/remote_play/stream/datacenter_ping.hpp"
#include "src/core/psn/types.hpp"

namespace libchiaking::cloud {

/// @brief The streaming service a cloud session uses; see
/// psn::StreamingService.
using CloudService = psn::StreamingService;
/// @brief The console generation a cloud game runs on; see
/// psn::StreamingPlatform.
using CloudPlatform = psn::StreamingPlatform;

}  // namespace libchiaking::cloud
