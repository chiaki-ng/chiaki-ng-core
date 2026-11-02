#include "src/core/play/model/nat_mappings.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

namespace play_test {
namespace {

using libchiaking::platform::IpAddress;
using libchiaking::platform::SocketEndpoint;
using libchiaking::play::NatMappings;
using libchiaking::play::NatType;
using libchiaking::play::NatTypeFailure;

auto address(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
    -> IpAddress {
  return IpAddress::ipv4({a, b, c, d});
}

TEST(NatMappings, CallsAPublicAddressTypeOne) {
  const std::array local{address(192, 168, 1, 2), address(203, 0, 113, 9)};
  const SocketEndpoint mapped{address(203, 0, 113, 9), 50000};
  const auto type =
      NatMappings{mapped, std::nullopt, {local.begin(), local.end()}}.type();
  ASSERT_TRUE(type);
  EXPECT_EQ(type.value(), NatType::type1);
}

TEST(NatMappings, CallsOneMappingForBothServersTypeTwo) {
  const std::array local{address(192, 168, 1, 2)};
  const SocketEndpoint mapped{address(198, 51, 100, 7), 50000};
  const auto type =
      NatMappings{mapped, mapped, {local.begin(), local.end()}}.type();
  ASSERT_TRUE(type);
  EXPECT_EQ(type.value(), NatType::type2);
}

TEST(NatMappings, CallsAMappingPerServerTypeThree) {
  const std::array local{address(192, 168, 1, 2)};
  const SocketEndpoint first{address(198, 51, 100, 7), 50000};
  const SocketEndpoint second{address(198, 51, 100, 7), 50013};
  const auto type =
      NatMappings{first, second, {local.begin(), local.end()}}.type();
  ASSERT_TRUE(type);
  EXPECT_EQ(type.value(), NatType::type3);
}

TEST(NatMappings, IsIncompleteBehindARouterWithoutASecondAnswer) {
  const std::array local{address(192, 168, 1, 2)};
  const SocketEndpoint mapped{address(198, 51, 100, 7), 50000};
  const auto type =
      NatMappings{mapped, std::nullopt, {local.begin(), local.end()}}.type();
  ASSERT_FALSE(type);
  EXPECT_EQ(type.error().reason, NatTypeFailure::Reason::incomplete);
}

}  // namespace
}  // namespace play_test
