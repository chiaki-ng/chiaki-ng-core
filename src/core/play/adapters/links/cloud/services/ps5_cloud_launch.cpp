#include "src/core/play/adapters/links/cloud/services/ps5_cloud_launch.hpp"

namespace libchiaking::play {

Ps5CloudLaunch::Ps5CloudLaunch(const catalog::ClearedLaunch& cleared)
    : entitlement_(cleared.identifier.text()) {}

}  // namespace libchiaking::play
