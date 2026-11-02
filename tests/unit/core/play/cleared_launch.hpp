#pragma once

#include "tests/support/play/scripted_clearance.hpp"

#include "src/core/play/adapters/links/cloud/cloud_link.hpp"

#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"

#include <string_view>

namespace play_test {

using test_support::play::ScriptedClearance;
using test_support::play::Title;

class ClearedLaunch {
 public:
  explicit ClearedLaunch(libchiaking::cloud::CloudService service =
                             libchiaking::cloud::CloudService::cloud_stream,
                         std::string_view entitlement = "synthetic-entitlement")
      : entitlement_(entitlement), clearance(title_for(service, entitlement)) {}

  explicit ClearedLaunch(const Title& title)
      : entitlement_(title.product), clearance(title) {}

  ClearedLaunch(const ClearedLaunch&) = delete;
  auto operator=(const ClearedLaunch&) -> ClearedLaunch& = delete;

  static auto title_for(libchiaking::cloud::CloudService service,
                        std::string_view entitlement) -> Title {
    if (service == libchiaking::cloud::CloudService::legacy_cloud_stream)
      return Title{
          .product = entitlement,
          .service = libchiaking::catalog::ServiceGeneration::old_service,
          .platform = libchiaking::catalog::Platform::ps4,
          .held_as = entitlement};
    return Title{.product = entitlement};
  }

  void arm(libchiaking::play::CloudLinkConfiguration& configuration) const {
    configuration.product = Title::product_id(entitlement_);
  }

  auto seams() noexcept -> libchiaking::play::CloudLaunchSeams {
    return libchiaking::play::CloudLaunchSeams{.clearance = &clearance,
                                               .waiter = waiter};
  }

 private:
  std::string_view entitlement_;

 public:
  ScriptedClearance clearance;
  libchiaking::net::http::PollWaiter* waiter{};
};

}  // namespace play_test
