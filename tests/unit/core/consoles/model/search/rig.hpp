#pragma once

#include "src/core/consoles/adapters/search/search_datagram.hpp"
#include "src/core/consoles/model/search/search_plan.hpp"
#include "tests/support/enum_printing.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class PlanRig {
 public:
  using Addresses = std::vector<libchiaking::platform::IpAddress>;
  using Interfaces = std::vector<libchiaking::platform::InterfaceInfo>;
  using Texts = std::vector<std::string>;

  static auto ip(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
      -> libchiaking::platform::IpAddress {
    return libchiaking::platform::IpAddress::ipv4({a, b, c, d});
  }

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
    return result;
  }

  static auto subnet(std::string_view cidr) -> libchiaking::consoles::Subnet {
    return libchiaking::consoles::Subnet::parse(cidr).value();
  }

  static auto ranges(bool unicast, const std::vector<std::string>& cidrs)
      -> libchiaking::Expected<libchiaking::consoles::SearchRanges,
                               libchiaking::consoles::SearchRanges::Invalid> {
    return libchiaking::consoles::SearchRanges::parse(cidrs, unicast);
  }

  static auto plan_of(bool unicast, const std::vector<std::string>& cidrs)
      -> libchiaking::consoles::SearchPlan {
    return libchiaking::consoles::SearchPlan{ranges(unicast, cidrs).value()};
  }

  static auto hardware(unsigned number) -> std::string {
    char text[13]{};
    std::snprintf(text, sizeof text, "%012X", number);
    return text;
  }

  static auto reply(std::string_view id,
                    libchiaking::platform::IpAddress address)
      -> libchiaking::consoles::SearchReply {
    const auto text =
        "HTTP/1.1 200 Ok\nhost-id:" + std::string{id} + "\nhost-type:PS5\n";
    return libchiaking::consoles::SearchDatagram::admit(
               std::as_bytes(std::span{text.data(), text.size()}), address)
        .value();
  }

  static auto texts(const std::vector<libchiaking::consoles::Subnet>& subnets)
      -> Texts {
    Texts result;
    for (const auto& entry : subnets) result.push_back(entry.text());
    return result;
  }
};

}  // namespace consoles_test
