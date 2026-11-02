#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace nat_test {
namespace {

namespace event = libchiaking::net::nat::ice::event;
using libchiaking::net::nat::ice::check_floor_us;
using libchiaking::net::nat::ice::check_transmissions;
using libchiaking::net::nat::ice::connectivity_timeout_us;
using libchiaking::net::nat::ice::max_check_requests;
using libchiaking::net::nat::ice::max_pairs;
using libchiaking::net::nat::ice::max_port_guesses;

class IceConnectivity : public SeededIceAgentRig {};

TEST_P(IceConnectivity, PairsPublicHostsDirectly) {
  for (const auto head_start : {std::uint64_t{0}, 20 * second}) {
    SCOPED_TRACE(head_start);
    auto session = prepare(
        seed(), Topology{NatKind::none, NatKind::none, true, head_start});
    EXPECT_FALSE(has_type(session.host_ice, CandidateType::server_reflexive));
    EXPECT_FALSE(has_type(session.guest_ice, CandidateType::server_reflexive));
    EXPECT_EQ(session.host_ice.candidates().size(), 1U);
    expect_nominated(session, 3 * second);
    const auto host_path = nominated_path(*session.sim, session.host);
    EXPECT_EQ(host_path.remote, at(ipv4(198, 51, 100, 20), 6000));
    EXPECT_EQ(host_path.mapped, at(ipv4(203, 0, 113, 10), 5000));
  }
}

TEST_P(IceConnectivity, PunchesThroughPortRestrictedNats) {
  for (const auto head_start : {std::uint64_t{0}, 2 * second, 60 * second}) {
    SCOPED_TRACE(head_start);
    Topology topology{NatKind::port_restricted, NatKind::port_restricted, true,
                      head_start};
    topology.host_wait_us = head_start == 0 ? 0 : 100 * second;
    auto session = prepare(seed(), topology);
    EXPECT_TRUE(has_type(session.host_ice, CandidateType::server_reflexive));
    EXPECT_TRUE(has_type(session.guest_ice, CandidateType::server_reflexive));
    if (head_start == 60 * second) {
      EXPECT_GT(session.guest_requests_before_connect,
                3 * 2 * check_transmissions);
    }
    expect_nominated(session, connectivity_timeout_us - 1);
    const auto host_path = nominated_path(*session.sim, session.host);
    EXPECT_EQ(host_path.remote.address, ipv4(198, 51, 100, 20));
    EXPECT_EQ(host_path.mapped.address, ipv4(203, 0, 113, 10));
  }
}

TEST_P(IceConnectivity, ReachesASymmetricGuestThroughGuessedPorts) {
  Topology topology{NatKind::port_restricted, NatKind::symmetric, true,
                    3 * second};
  topology.guest_guessing = PortGuessing::forced;
  auto session = prepare(seed(), topology);
  EXPECT_EQ(session.guest_ice.port_guesses(), max_port_guesses);
  EXPECT_LT(session.guest_ice.candidates().size(), 4U);
  expect_nominated(session, connectivity_timeout_us - 1);
}

TEST_P(IceConnectivity, ReachesASymmetricHostThroughGuessedPorts) {
  Topology topology{NatKind::symmetric, NatKind::port_restricted, true,
                    3 * second};
  topology.host_guessing = PortGuessing::forced;
  auto session = prepare(seed(), topology);
  EXPECT_EQ(session.host_ice.port_guesses(), max_port_guesses);
  EXPECT_LT(session.host_ice.candidates().size(), 4U);
  expect_nominated(session, connectivity_timeout_us - 1);
}

class IceLossyNetwork : public SeededIceAgentRig {};

TEST_P(IceLossyNetwork, NominatesDespiteLossAndJitter) {
  auto session = prepare(
      seed(), Topology{NatKind::port_restricted, NatKind::port_restricted, true,
                       3 * second, 200, 30'000});
  expect_nominated(session, connectivity_timeout_us - 1);
}

TEST_P(IceConnectivity, SettlesARoleConflictBetweenTwoHosts) {
  Topology topology;
  topology.host_role = TransportRole::host;
  topology.guest_role = TransportRole::host;
  auto session = prepare(seed(), topology);
  auto& sim = *session.sim;
  EXPECT_EQ(sim.agent(session.host).role(), sim.agent(session.guest).role());
  expect_nominated(session, 20 * second);
  EXPECT_NE(sim.agent(session.host).role(), sim.agent(session.guest).role());
}

TEST_P(IceConnectivity, SettlesARoleConflictBetweenTwoGuests) {
  Topology topology;
  topology.host_role = TransportRole::guest;
  topology.guest_role = TransportRole::guest;
  auto session = prepare(seed(), topology);
  auto& sim = *session.sim;
  expect_nominated(session, 20 * second);
  EXPECT_NE(sim.agent(session.host).role(), sim.agent(session.guest).role());
}

class IceCandidateFlood : public SeededIceAgentRig {
 protected:
  static auto fabricated_remote() -> IceParameters {
    std::vector<libchiaking::net::kakusan::Candidate> candidates;
    for (std::uint8_t index = 0;
         index < libchiaking::net::kakusan::max_candidates; ++index)
      candidates.push_back(libchiaking::net::kakusan::Candidate{
          "r" + std::to_string(index),
          static_cast<std::uint32_t>(2'000'000'000U - index),
          at(ipv4(198, 18, 0, static_cast<std::uint8_t>(index + 1)), 9000),
          CandidateType::host,
          {}});
    return with_credentials(std::move(candidates), 0,
                            libchiaking::SecretString{"0123456789abcdef"},
                            libchiaking::SecretString{std::string(48, 'c')});
  }

  static auto many_bases() -> std::vector<PeerSocket> {
    std::vector<PeerSocket> sockets;
    for (std::uint8_t index = 0;
         index < libchiaking::net::kakusan::max_candidates; ++index)
      sockets.push_back(public_peer(at(ipv4(203, 0, 113, 100 + index), 5000)));
    return sockets;
  }
};

TEST_P(IceCandidateFlood, CapsTheChecksOfAControllingAgent) {
  Simulation sim{crypto, seed()};
  const auto peer = sim.add_peer(TransportRole::host, many_bases(), {});
  EXPECT_EQ(gathered(sim, peer).candidates().size(),
            libchiaking::net::kakusan::max_candidates);
  const auto start = sim.now();
  EXPECT_TRUE(sim.agent(peer).connect(fabricated_remote(), start));
  sim.run_for(connectivity_timeout_us + second);
  const auto& stats = sim.network().stats(peer);
  EXPECT_EQ(stats.checked.size(), max_pairs);
  EXPECT_EQ(stats.check_requests, sim.agent(peer).check_requests_sent());
  EXPECT_LE(stats.check_requests, max_check_requests);
  EXPECT_GE(stats.min_gap_us, check_floor_us);
  EXPECT_EQ(failure(sim, peer), Error::no_path);
  EXPECT_EQ(failed_at(sim, peer), start + connectivity_timeout_us);
}

class IceCandidateFloodOnAGuest : public IceCandidateFlood {};

TEST_P(IceCandidateFloodOnAGuest, StopsCheckingAtTheRequestCap) {
  Simulation sim{crypto, seed()};
  const auto peer = sim.add_peer(TransportRole::guest, many_bases(), {});
  EXPECT_TRUE(sim.agent(peer).connect(fabricated_remote(), sim.now()));
  sim.run_until(4000 * second, [&] {
    return sim.network().stats(peer).check_requests >= max_check_requests;
  });
  const auto reached = sim.network().stats(peer).check_requests;
  EXPECT_EQ(reached, max_check_requests);
  sim.run_for(600 * second);
  const auto& stats = sim.network().stats(peer);
  EXPECT_EQ(stats.check_requests, max_check_requests);
  EXPECT_EQ(sim.agent(peer).check_requests_sent(), max_check_requests);
  EXPECT_LE(stats.checked.size(), max_pairs);
  EXPECT_GE(stats.min_gap_us, check_floor_us);
  EXPECT_FALSE(failure(sim, peer));
  sim.agent(peer).close();
  EXPECT_FALSE(sim.agent(peer).next_deadline_us());
}

class IceUnreachable
    : public IceAgentRig,
      public ::testing::TestWithParam<
          std::tuple<std::uint64_t, std::tuple<test_support::net::NatKind,
                                               test_support::net::NatKind>>> {};

TEST_P(IceUnreachable, FailsTheHostAtTheTimeoutWhileTheGuestKeepsTrying) {
  const auto [seed, nats] = GetParam();
  const auto [host_nat, guest_nat] = nats;
  auto session = prepare(seed, Topology{host_nat, guest_nat, true, 3 * second});
  auto& sim = *session.sim;
  sim.run_until(session.connect_us + connectivity_timeout_us + second, [&] {
    return count_events<event::Failed>(sim, session.host) != 0;
  });
  ASSERT_TRUE(failed_at(sim, session.host));
  EXPECT_EQ(failed_at(sim, session.host),
            session.connect_us + connectivity_timeout_us);
  EXPECT_EQ(failure(sim, session.host), Error::no_path);
  EXPECT_FALSE(sim.agent(session.host).next_deadline_us());
  EXPECT_FALSE(sim.agent(session.host).nominated());
  EXPECT_FALSE(sim.agent(session.host).consent());

  const auto before = sim.network().stats(session.guest).check_requests;
  sim.run_for(90 * second);
  EXPECT_EQ(count_events<event::Nominated>(sim, session.guest), 0U);
  EXPECT_FALSE(failure(sim, session.guest));
  EXPECT_TRUE(sim.agent(session.guest).next_deadline_us());
  EXPECT_GT(sim.network().stats(session.guest).check_requests, before);
  sim.agent(session.guest).close();
  EXPECT_FALSE(sim.agent(session.guest).next_deadline_us());
  EXPECT_FALSE(sim.agent(session.guest).take_event());
}

INSTANTIATE_TEST_SUITE_P(Seeds, IceConnectivity,
                         ::testing::Range<std::uint64_t>(1, 7));
INSTANTIATE_TEST_SUITE_P(Seeds, IceLossyNetwork,
                         ::testing::Range<std::uint64_t>(1, 17));
INSTANTIATE_TEST_SUITE_P(Seeds, IceCandidateFlood,
                         ::testing::Range<std::uint64_t>(1, 7));
INSTANTIATE_TEST_SUITE_P(Seeds, IceCandidateFloodOnAGuest,
                         ::testing::Range<std::uint64_t>(1, 3));
INSTANTIATE_TEST_SUITE_P(
    SeedsAndNats, IceUnreachable,
    ::testing::Combine(
        ::testing::Range<std::uint64_t>(1, 7),
        ::testing::Values(
            std::tuple{test_support::net::NatKind::symmetric,
                       test_support::net::NatKind::port_restricted},
            std::tuple{test_support::net::NatKind::port_restricted,
                       test_support::net::NatKind::symmetric},
            std::tuple{test_support::net::NatKind::symmetric,
                       test_support::net::NatKind::symmetric})));

}  // namespace
}  // namespace nat_test
