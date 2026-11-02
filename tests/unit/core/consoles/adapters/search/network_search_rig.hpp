#pragma once

#include "src/core/consoles/adapters/search/network_search.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class NetworkSearchRig {
 public:
  static constexpr std::string_view living =
      "HTTP/1.1 200 Ok\nhost-id:AABBCCDDEEFF\nhost-type:PS5\n"
      "host-name:Living\nhost-request-port:997\n";

  static auto interface_at(libchiaking::platform::IpAddress address,
                           libchiaking::platform::IpAddress broadcast)
      -> libchiaking::platform::InterfaceInfo {
    libchiaking::platform::InterfaceInfo result;
    result.name = "eth0";
    result.index = 1;
    result.address = address;
    result.broadcast = broadcast;
    result.has_broadcast = true;
    result.operational = true;
    result.lan_adapter = true;
    return result;
  }

  static auto routed_interface() -> libchiaking::platform::InterfaceInfo {
    auto routed = interface_at(SearchRig::ip(192, 0, 2, 10),
                               libchiaking::platform::IpAddress::any_ipv4());
    routed.has_broadcast = false;
    return routed;
  }

  static auto ranges(bool unicast = false,
                     const std::vector<std::string>& cidrs = {})
      -> libchiaking::consoles::SearchRanges {
    return libchiaking::consoles::SearchRanges::parse(cidrs, unicast).value();
  }

  static auto on_loopback() -> libchiaking::consoles::Beacon::Settings {
    auto settings = SearchRig::on_loopback();
    settings.targets = {SearchRig::ip(203, 0, 113, 77)};
    return settings;
  }

  static auto progress_of(const libchiaking::consoles::NetworkSearch& search)
      -> libchiaking::consoles::SearchPlan::Progress {
    return search.progress().value();
  }

  static auto pump_until_revision(libchiaking::consoles::NetworkSearch& search,
                                  std::uint64_t expected)
      -> testing::AssertionResult {
    for (int attempt = 0; attempt < 2000 && search.revision() < expected;
         ++attempt)
      if (!search.pump_once())
        return testing::AssertionFailure() << "pump " << attempt << " failed";
    if (search.revision() != expected)
      return testing::AssertionFailure()
             << "the revision is " << search.revision() << ", not " << expected;
    return testing::AssertionSuccess();
  }

  static auto pump_quietly(libchiaking::consoles::NetworkSearch& search)
      -> testing::AssertionResult {
    for (int attempt = 0; attempt < 200; ++attempt)
      if (!search.pump_once())
        return testing::AssertionFailure() << "pump " << attempt << " failed";
    return testing::AssertionSuccess();
  }
};

}  // namespace consoles_test
