#pragma once

#include "src/core/net/protocol/takion/versions.hpp"

namespace play_test {

class TakionDefaults {
 public:
  static constexpr auto consoles() -> bool {
    using namespace libchiaking::net::protocol::takion;
    constexpr auto override_version = takion_build_version_override;
    return takion_version_ps4 == 9 && takion_version_ps5 == 20 &&
           (override_version == 0 || override_version == 21 ||
            override_version == 23 || override_version == 25);
  }

  static constexpr auto cloud() -> bool {
    using namespace libchiaking::net::protocol::takion;
    return takion_build_version_override == 0 &&
           takion_version_legacy_cloud_stream == 12 &&
           takion_version_cloud_stream == 23;
  }
};

}  // namespace play_test
