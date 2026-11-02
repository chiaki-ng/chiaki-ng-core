#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/stun/mapping_estimate.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

namespace nat_test {
namespace {

namespace pf = libchiaking::platform;
namespace stun = libchiaking::net::nat::stun;
using stun::AllocationDiscovery;

class StunAllocationEstimate : public ::testing::Test {
 protected:
  using Four = std::array<pf::SocketEndpoint, 4>;

  static auto ep(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
                 std::uint16_t port) -> pf::SocketEndpoint {
    return {pf::IpAddress::ipv4({a, b, c, d}), port};
  }

  static auto projects(std::span<const pf::SocketEndpoint> observations,
                       pf::SocketEndpoint projected, std::int32_t increment,
                       bool random) -> ::testing::AssertionResult {
    return matches(AllocationDiscovery::estimate(observations), projected,
                   increment, random);
  }

  static auto projects_divergent(const Four& items, std::int32_t increment,
                                 bool random) -> ::testing::AssertionResult {
    return matches(AllocationDiscovery::estimate_four_divergent(items),
                   items[3], increment, random);
  }

  static auto projects_same_first(const Four& items, std::int32_t increment,
                                  bool random) -> ::testing::AssertionResult {
    return projects(items, items[3], increment, random);
  }

  static auto matches(
      const libchiaking::Expected<stun::StunAllocationEstimate,
                                  stun::StunAllocationError>& result,
      pf::SocketEndpoint projected, std::int32_t increment, bool random)
      -> ::testing::AssertionResult {
    if (!result) return ::testing::AssertionFailure() << "no estimate";
    if (!(result.value().projected == projected))
      return ::testing::AssertionFailure()
             << "projected port " << result.value().projected.port
             << ", expected " << projected.port;
    if (result.value().increment != increment)
      return ::testing::AssertionFailure()
             << "increment " << result.value().increment << ", expected "
             << increment;
    if (result.value().random_allocation != random)
      return ::testing::AssertionFailure()
             << "random allocation " << result.value().random_allocation
             << ", expected " << random;
    return ::testing::AssertionSuccess();
  }

  const pf::IpAddress a = pf::IpAddress::ipv4({192, 0, 2, 1});
  const pf::IpAddress b = pf::IpAddress::ipv4({203, 0, 113, 9});
  const pf::IpAddress c = pf::IpAddress::ipv4({198, 51, 100, 4});
  const pf::IpAddress d = pf::IpAddress::ipv4({198, 51, 100, 5});
};

TEST_F(StunAllocationEstimate, UsesASingleMappingAsIs) {
  const pf::SocketEndpoint single[] = {ep(192, 0, 2, 1, 40000)};
  EXPECT_TRUE(projects(single, single[0], 0, false));
}

TEST_F(StunAllocationEstimate, ProjectsTwoStepsOnFromTwoMappingsOfOneAddress) {
  const pf::SocketEndpoint positive[] = {ep(198, 51, 100, 7, 5000),
                                         ep(198, 51, 100, 7, 5004)};
  EXPECT_TRUE(projects(positive, ep(198, 51, 100, 7, 5012), 4, false));
  const pf::SocketEndpoint negative[] = {ep(198, 51, 100, 7, 5000),
                                         ep(198, 51, 100, 7, 4998)};
  const auto down = AllocationDiscovery::estimate(negative);
  ASSERT_TRUE(down);
  EXPECT_EQ(down.value().projected, ep(198, 51, 100, 7, 4994));
  EXPECT_EQ(down.value().increment, -2);
  const pf::SocketEndpoint wrap[] = {ep(198, 51, 100, 7, 65530),
                                     ep(198, 51, 100, 7, 65535)};
  const auto wrapped = AllocationDiscovery::estimate(wrap);
  ASSERT_TRUE(wrapped);
  EXPECT_EQ(wrapped.value().projected, ep(198, 51, 100, 7, 9));
  EXPECT_EQ(wrapped.value().increment, 5);
}

TEST_F(StunAllocationEstimate,
       KeepsTheSecondOfTwoMappingsOfDifferentAddresses) {
  const pf::SocketEndpoint moved[] = {ep(198, 51, 100, 7, 5000),
                                      ep(203, 0, 113, 9, 5004)};
  const auto changed = AllocationDiscovery::estimate(moved);
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed.value().projected, moved[1]);
  EXPECT_EQ(changed.value().increment, 0);
}

TEST_F(StunAllocationEstimate, StepsFromTheEarlierMappingTheThirdShares) {
  const pf::SocketEndpoint c_matches_a[] = {
      ep(192, 0, 2, 1, 1000), ep(203, 0, 113, 9, 5000), ep(192, 0, 2, 1, 1010)};
  EXPECT_TRUE(projects(c_matches_a, ep(192, 0, 2, 1, 1015), 5, false));
  const pf::SocketEndpoint c_matches_b[] = {ep(192, 0, 2, 1, 1000),
                                            ep(203, 0, 113, 9, 5000),
                                            ep(203, 0, 113, 9, 5007)};
  EXPECT_TRUE(projects(c_matches_b, ep(203, 0, 113, 9, 5014), 7, false));
  const pf::SocketEndpoint c_matches_none[] = {ep(192, 0, 2, 1, 1000),
                                               ep(203, 0, 113, 9, 5000),
                                               ep(198, 51, 100, 4, 6000)};
  EXPECT_TRUE(projects(c_matches_none, c_matches_none[2], 0, false));
  const pf::SocketEndpoint same_ab_new_c[] = {ep(192, 0, 2, 1, 4000),
                                              ep(192, 0, 2, 1, 4004),
                                              ep(198, 51, 100, 4, 1000)};
  EXPECT_TRUE(projects(same_ab_new_c, ep(198, 51, 100, 4, 1004), 4, false));
}

TEST_F(StunAllocationEstimate, FlagsUnevenStepsOfOneAddressAsRandom) {
  const pf::SocketEndpoint random_case[] = {
      ep(192, 0, 2, 1, 5000), ep(192, 0, 2, 1, 5004), ep(192, 0, 2, 1, 5011)};
  EXPECT_TRUE(projects(random_case, ep(192, 0, 2, 1, 5015), 4, true));
  const pf::SocketEndpoint zero_then_delta[] = {
      ep(192, 0, 2, 1, 5000), ep(192, 0, 2, 1, 5000), ep(192, 0, 2, 1, 5003)};
  EXPECT_TRUE(projects(zero_then_delta, ep(192, 0, 2, 1, 5006), 3, true));
}

TEST_F(StunAllocationEstimate, ProjectsSteadyStepsAcrossTheWrap) {
  const pf::SocketEndpoint steady[] = {
      ep(192, 0, 2, 1, 5000), ep(192, 0, 2, 1, 5002), ep(192, 0, 2, 1, 5004)};
  EXPECT_TRUE(projects(steady, ep(192, 0, 2, 1, 5006), 2, false));
  const pf::SocketEndpoint wrap3[] = {ep(192, 0, 2, 1, 65529),
                                      ep(192, 0, 2, 1, 65532),
                                      ep(192, 0, 2, 1, 65535)};
  EXPECT_TRUE(projects(wrap3, ep(192, 0, 2, 1, 2), 3, false));
  const pf::SocketEndpoint down3[] = {ep(192, 0, 2, 1, 5), ep(192, 0, 2, 1, 3),
                                      ep(192, 0, 2, 1, 1)};
  EXPECT_TRUE(projects(down3, ep(192, 0, 2, 1, 65535), -2, false));
}

TEST_F(StunAllocationEstimate, KeepsTheFourthOfFourMappings) {
  const pf::SocketEndpoint four[] = {ep(192, 0, 2, 1, 1), ep(192, 0, 2, 1, 2),
                                     ep(192, 0, 2, 1, 3), ep(192, 0, 2, 1, 4)};
  EXPECT_TRUE(projects(four, four[3], 1, false));
}

TEST_F(StunAllocationEstimate, RefusesUnusableObservations) {
  EXPECT_FALSE(
      AllocationDiscovery::estimate(std::span<const pf::SocketEndpoint>{}));
  const pf::SocketEndpoint zero_port[] = {ep(192, 0, 2, 1, 0)};
  EXPECT_FALSE(AllocationDiscovery::estimate(zero_port));
  const pf::SocketEndpoint unspecified[] = {{pf::IpAddress::any_ipv4(), 3478}};
  EXPECT_FALSE(AllocationDiscovery::estimate(unspecified));
  const pf::SocketEndpoint ipv6[] = {{pf::IpAddress::loopback_ipv6(), 3478}};
  EXPECT_FALSE(AllocationDiscovery::estimate(ipv6));
  const pf::SocketEndpoint five[] = {ep(192, 0, 2, 1, 1), ep(192, 0, 2, 1, 2),
                                     ep(192, 0, 2, 1, 3), ep(192, 0, 2, 1, 4),
                                     ep(192, 0, 2, 1, 5)};
  EXPECT_FALSE(AllocationDiscovery::estimate(five));
}

TEST_F(StunAllocationEstimate, ReadsFourDivergentMappings) {
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {c, 6000}, {d, 6020}}},
                                 0, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {c, 6000}, {a, 1004}}},
                                 1, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {a, 1002}, {d, 6000}}},
                                 1, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {c, 6000}, {b, 5008}}},
                                 1002, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {b, 5002}, {d, 6000}}},
                                 2001, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {a, 1002}, {a, 1010}}},
                                 8, true));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {b, 5002}, {b, 5010}}},
                                 8, true));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {c, 6000}, {c, 5990}}},
                                 10, false));
  EXPECT_TRUE(projects_divergent({{{a, 1000}, {b, 5000}, {a, 1004}, {a, 1004}}},
                                 2, true));
}

TEST_F(StunAllocationEstimate, ReadsFourMappingsWhoseFirstTwoShareAnAddress) {
  EXPECT_TRUE(projects_same_first(
      {{{a, 1000}, {a, 1004}, {b, 6000}, {a, 1012}}}, 4, false));
  EXPECT_TRUE(projects_same_first(
      {{{a, 1000}, {a, 1004}, {b, 6000}, {a, 1016}}}, 4, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 5000}, {a, 5000}, {b, 6000}, {a, 5006}}}, 3, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 1000}, {a, 1004}, {b, 6000}, {b, 6010}}}, 4, false));
  EXPECT_TRUE(projects_same_first(
      {{{a, 4000}, {a, 4004}, {a, 4008}, {b, 9000}}}, 4, false));
  EXPECT_TRUE(projects_same_first(
      {{{a, 4000}, {a, 4004}, {a, 4011}, {b, 9000}}}, 4, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 4000}, {a, 4000}, {a, 4003}, {b, 9000}}}, 3, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 5000}, {a, 5002}, {a, 5005}, {a, 5007}}}, 2, false));
  EXPECT_TRUE(projects_same_first(
      {{{a, 5000}, {a, 5005}, {a, 5008}, {a, 5011}}}, 3, false));
  EXPECT_TRUE(projects_same_first(
      {{{a, 5000}, {a, 5001}, {a, 5003}, {a, 5006}}}, 1, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 5000}, {a, 5000}, {a, 5003}, {a, 5007}}}, 3, true));
  EXPECT_TRUE(projects_same_first(
      {{{a, 65529}, {a, 65532}, {a, 65535}, {a, 2}}}, 3, false));
}

TEST_F(StunAllocationEstimate, AgreesWithTheDivergentEstimateForDivergentFour) {
  const Four divergent{{{a, 1000}, {b, 5000}, {c, 6000}, {d, 6020}}};
  const auto via_main = AllocationDiscovery::estimate(divergent);
  const auto via_divergent =
      AllocationDiscovery::estimate_four_divergent(divergent);
  ASSERT_TRUE(via_main);
  ASSERT_TRUE(via_divergent);
  EXPECT_EQ(via_main.value().projected, via_divergent.value().projected);
  EXPECT_EQ(via_main.value().increment, via_divergent.value().increment);
}

TEST_F(StunAllocationEstimate, RefusesDivergentInputThatDoesNotDiverge) {
  const Four same_first{{{a, 1000}, {a, 1002}, {c, 3000}, {d, 3004}}};
  EXPECT_FALSE(AllocationDiscovery::estimate_four_divergent(same_first));
  const Four zero_port_four{{{a, 1}, {b, 2}, {c, 0}, {d, 4}}};
  EXPECT_FALSE(AllocationDiscovery::estimate_four_divergent(zero_port_four));
}

}  // namespace
}  // namespace nat_test
