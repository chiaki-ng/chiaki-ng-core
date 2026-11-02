#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/holepunch_probe_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using holepunch_probe::CountingUdp;
using holepunch_probe::QuietUdp;

class HolepunchProbeGroupPlan : public ::testing::Test {
 protected:
  HolepunchProbeGroupPlan() {
    sockets_.ipv4 = std::make_unique<QuietUdp>(network_);
    sockets_.ipv6 = std::make_unique<QuietUdp>(network_);
  }

  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(), test_support::LiveProviders::steady()};
  nat::BoundSockets sockets_;
  const pf::SocketEndpoint a_{pf::IpAddress::ipv4({10, 0, 0, 1}), 1000};
  const pf::SocketEndpoint b_{
      pf::IpAddress::ipv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}),
      2000};
  const pf::SocketEndpoint c_{pf::IpAddress::ipv4({10, 0, 0, 3}), 1001};
  const std::vector<nat::ResolvedCandidateEndpoint> resolved_{
      {5, a_}, {6, b_}, {7, c_}, {8, a_}};
  const std::vector<nat::ResolvedCandidateEndpoint> only_v6_{{9, b_}};
};

TEST_F(HolepunchProbeGroupPlan, GroupsPeersBySocketFamily) {
  auto plan_result = nat::ProbeGroupPlan::make(sockets_, resolved_);
  ASSERT_TRUE(plan_result.has_value());
  auto plan = std::move(plan_result.value());
  auto groups = plan.groups();
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0].socket, sockets_.ipv4.get());
  ASSERT_EQ(groups[0].peers.size(), 3U);
  EXPECT_EQ(groups[0].peers[0], a_);
  EXPECT_EQ(groups[0].peers[1], c_);
  EXPECT_EQ(groups[0].peers[2], a_);
  EXPECT_EQ(groups[1].socket, sockets_.ipv6.get());
  ASSERT_EQ(groups[1].peers.size(), 1U);
  EXPECT_EQ(groups[1].peers[0], b_);
  EXPECT_EQ(plan.source_index({0, a_}), 5U);
  EXPECT_EQ(plan.source_index({0, c_}), 7U);
  EXPECT_EQ(plan.source_index({1, b_}), 6U);
  EXPECT_FALSE(plan.source_index({0, b_}).has_value());
  EXPECT_FALSE(plan.source_index({2, a_}).has_value());
}

TEST_F(HolepunchProbeGroupPlan, MakesOneGroupForOnlyIpv6Peers) {
  auto v6_plan = nat::ProbeGroupPlan::make(sockets_, only_v6_);
  ASSERT_TRUE(v6_plan.has_value());
  auto v6_groups = v6_plan.value().groups();
  ASSERT_EQ(v6_groups.size(), 1U);
  EXPECT_EQ(v6_groups[0].socket, sockets_.ipv6.get());
  EXPECT_EQ(v6_plan.value().source_index({0, b_}), 9U);
  EXPECT_FALSE(v6_plan.value().source_index({1, b_}).has_value());
}

TEST_F(HolepunchProbeGroupPlan, DropsIpv6PeersWithoutAnIpv6Socket) {
  sockets_.ipv6.reset();
  auto fallback = nat::ProbeGroupPlan::make(sockets_, resolved_);
  ASSERT_TRUE(fallback.has_value());
  auto fallback_groups = fallback.value().groups();
  ASSERT_EQ(fallback_groups.size(), 1U);
  EXPECT_EQ(fallback_groups[0].peers.size(), 3U);
  EXPECT_EQ(fallback.value().source_index({0, c_}), 7U);
  auto unusable = nat::ProbeGroupPlan::make(sockets_, only_v6_);
  EXPECT_FALSE(unusable.has_value());
  auto empty = nat::ProbeGroupPlan::make(sockets_, {});
  EXPECT_FALSE(empty.has_value());
}

TEST_F(HolepunchProbeGroupPlan, SurvivesItsSocketsMovingOwner) {
  nat::BoundSockets owner;
  owner.ipv4 = std::make_unique<QuietUdp>(network_);
  owner.ipv6 = std::make_unique<QuietUdp>(network_);
  auto* ipv4_before = owner.ipv4.get();
  auto* ipv6_before = owner.ipv6.get();
  auto moved_plan = nat::ProbeGroupPlan::make(owner, resolved_);
  ASSERT_TRUE(moved_plan.has_value());
  nat::BoundSockets moved_owner = std::move(owner);
  EXPECT_FALSE(owner.ipv4);
  EXPECT_FALSE(owner.ipv6);
  EXPECT_EQ(moved_owner.ipv4.get(), ipv4_before);
  EXPECT_EQ(moved_owner.ipv6.get(), ipv6_before);
  auto moved_groups = moved_plan.value().groups();
  ASSERT_EQ(moved_groups.size(), 2U);
  EXPECT_EQ(moved_groups[0].socket, ipv4_before);
  EXPECT_EQ(moved_groups[1].socket, ipv6_before);
  EXPECT_EQ(moved_plan.value().source_index({0, a_}), 5U);
}

class HolepunchProbeSelectSocket : public ::testing::Test {
 protected:
  auto sockets(unsigned& v4_destroyed, unsigned& v6_destroyed)
      -> nat::BoundSockets {
    nat::BoundSockets out;
    out.ipv4 = std::make_unique<CountingUdp>(network_, v4_destroyed);
    out.ipv6 = std::make_unique<CountingUdp>(network_, v6_destroyed);
    return out;
  }

  pf::DefaultNetworkProvider network_{test_support::LiveProviders::threading(), test_support::LiveProviders::steady()};
  const pf::SocketEndpoint v4_peer_{pf::IpAddress::ipv4({10, 0, 0, 1}), 1000};
  const pf::SocketEndpoint v6_peer_{pf::IpAddress::loopback_ipv6(), 2000};
  const std::vector<nat::ResolvedCandidateEndpoint> resolved_{
      {5, v4_peer_}, {6, v6_peer_}, {8, v4_peer_}};
};

TEST_F(HolepunchProbeSelectSocket, KeepsOnlyTheWinningNatSocket) {
  unsigned v4{};
  unsigned v6{};
  unsigned extra{};
  auto pair = sockets(v4, v6);
  for (int i = 0; i < 3; ++i)
    pair.probes.push_back(std::make_unique<CountingUdp>(network_, extra));
  auto* winner = pair.probes[1].get();
  std::vector<nat::Candidate> candidates(9);
  candidates[5].type = nat::CandidateType::stun;
  candidates[8].type = nat::CandidateType::local;
  auto plan = nat::ProbeGroupPlan::make(pair, resolved_, candidates);
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan.value().groups().size(), 5U);
  auto selected =
      nat::Probe::select_socket(std::move(pair), plan.value(), {3, v4_peer_});
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.value().socket.get(), winner);
  EXPECT_EQ(selected.value().candidate_index, 5U);
  EXPECT_EQ(v4, 1U);
  EXPECT_EQ(v6, 1U);
  EXPECT_EQ(extra, 2U);
  selected.value().socket.reset();
  EXPECT_EQ(extra, 3U);
}

TEST_F(HolepunchProbeSelectSocket, KeepsTheIpv4SocketForAnIpv4Winner) {
  unsigned v4_destroyed{};
  unsigned v6_destroyed{};
  auto pair = sockets(v4_destroyed, v6_destroyed);
  auto* winner = pair.ipv4.get();
  auto plan = nat::ProbeGroupPlan::make(pair, resolved_);
  ASSERT_TRUE(plan.has_value());
  auto selected =
      nat::Probe::select_socket(std::move(pair), plan.value(), {0, v4_peer_});
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected.value().socket.get(), winner);
  EXPECT_EQ(selected.value().peer, v4_peer_);
  EXPECT_EQ(selected.value().candidate_index, 5U);
  EXPECT_EQ(v4_destroyed, 0U);
  EXPECT_EQ(v6_destroyed, 1U);
  selected.value().socket.reset();
  EXPECT_EQ(v4_destroyed, 1U);
  EXPECT_EQ(v6_destroyed, 1U);
}

TEST_F(HolepunchProbeSelectSocket, KeepsTheIpv6SocketForAnIpv6Winner) {
  unsigned v4_destroyed{};
  unsigned v6_destroyed{};
  auto pair = sockets(v4_destroyed, v6_destroyed);
  auto* winner = pair.ipv6.get();
  auto plan = nat::ProbeGroupPlan::make(pair, resolved_);
  ASSERT_TRUE(plan.has_value());
  auto selected =
      nat::Probe::select_socket(std::move(pair), plan.value(), {1, v6_peer_});
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected.value().socket.get(), winner);
  EXPECT_EQ(selected.value().candidate_index, 6U);
  EXPECT_EQ(v4_destroyed, 1U);
  EXPECT_EQ(v6_destroyed, 0U);
}

TEST_F(HolepunchProbeSelectSocket, ClosesEverySocketForAGroupOutOfRange) {
  unsigned v4_destroyed{};
  unsigned v6_destroyed{};
  auto pair = sockets(v4_destroyed, v6_destroyed);
  auto plan = nat::ProbeGroupPlan::make(pair, resolved_);
  ASSERT_TRUE(plan.has_value());
  auto invalid =
      nat::Probe::select_socket(std::move(pair), plan.value(), {2, v4_peer_});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), nat::SelectSocketError::group_out_of_range);
  EXPECT_EQ(v4_destroyed, 1U);
  EXPECT_EQ(v6_destroyed, 1U);
}

TEST_F(HolepunchProbeSelectSocket, ClosesEverySocketForAnUnknownPeer) {
  unsigned v4_destroyed{};
  unsigned v6_destroyed{};
  auto pair = sockets(v4_destroyed, v6_destroyed);
  auto plan = nat::ProbeGroupPlan::make(pair, resolved_);
  ASSERT_TRUE(plan.has_value());
  const pf::SocketEndpoint unknown{pf::IpAddress::ipv4({10, 0, 0, 9}), 1000};
  auto invalid =
      nat::Probe::select_socket(std::move(pair), plan.value(), {0, unknown});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), nat::SelectSocketError::unknown_peer);
  EXPECT_EQ(v4_destroyed, 1U);
  EXPECT_EQ(v6_destroyed, 1U);
}

TEST_F(HolepunchProbeSelectSocket, RefusesSocketsThePlanWasNotMadeFor) {
  unsigned old_v4{};
  unsigned old_v6{};
  unsigned new_v4{};
  unsigned new_v6{};
  auto old_pair = sockets(old_v4, old_v6);
  auto plan = nat::ProbeGroupPlan::make(old_pair, resolved_);
  ASSERT_TRUE(plan.has_value());
  auto new_pair = sockets(new_v4, new_v6);
  auto invalid = nat::Probe::select_socket(std::move(new_pair), plan.value(),
                                           {0, v4_peer_});
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), nat::SelectSocketError::socket_mismatch);
  EXPECT_EQ(new_v4, 1U);
  EXPECT_EQ(new_v6, 1U);
  EXPECT_EQ(old_v4, 0U);
  EXPECT_EQ(old_v6, 0U);
}

}  // namespace
}  // namespace play_test
