#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace nat_test {
namespace {

namespace event = libchiaking::net::nat::ice::event;

class IceGathering : public SeededIceAgentRig {};

TEST_P(IceGathering, OffersTheHostCandidateAtOnceWithoutServers) {
  Simulation sim{crypto, seed()};
  sim.run_for(3 * second);
  const auto created = sim.now();
  const auto peer = sim.add_peer(
      TransportRole::host, {public_peer(at(ipv4(203, 0, 113, 10), 5000))}, {});
  EXPECT_EQ(gathered_at(sim, peer), created);
  EXPECT_EQ(gathered(sim, peer).candidates().size(), 1U);
  const auto ice = gathered(sim, peer);
  EXPECT_EQ(ice.ufrag().size(), 16U);
  EXPECT_EQ(ice.password().size(), 48U);
}

TEST_P(IceGathering, WaitsOutTheTimeoutForASilentServer) {
  for (const bool registered : {true, false}) {
    SCOPED_TRACE(registered);
    Simulation sim{crypto, seed()};
    if (registered) sim.network().add_stun_server(dead_server(), false);
    const auto peer = sim.add_peer(
        TransportRole::host, {public_peer(at(ipv4(203, 0, 113, 10), 5000))},
        {dead_server()});
    ASSERT_TRUE(gather_peer(sim, peer));
    EXPECT_EQ(gathered_at(sim, peer),
              libchiaking::net::kakusan::gathering_timeout_us);
    EXPECT_EQ(gathered(sim, peer).candidates().size(), 1U);
  }
}

TEST_P(IceGathering, AddsAServerReflexiveCandidateBehindANat) {
  Simulation sim{crypto, seed()};
  sim.network().add_stun_server(stun_server(), true);
  sim.network().add_stun_server(dead_server(), false);
  const auto nat =
      sim.network().add_nat(NatKind::port_restricted, ipv4(203, 0, 113, 10));
  const auto peer =
      sim.add_peer(TransportRole::host,
                   {PeerSocket{at(ipv4(192, 168, 1, 10), 5000), nat, 0}},
                   {stun_server(), dead_server()});
  ASSERT_TRUE(gather_peer(sim, peer));
  EXPECT_EQ(gathered_at(sim, peer),
            libchiaking::net::kakusan::gathering_timeout_us);
  const auto ice = gathered(sim, peer);
  ASSERT_EQ(ice.candidates().size(), 2U);
  EXPECT_EQ(ice.candidates()[1].type, CandidateType::server_reflexive);
  EXPECT_EQ(ice.candidates()[1].related, at(ipv4(192, 168, 1, 10), 5000));
  EXPECT_EQ(ice.candidates()[1].endpoint.address, ipv4(203, 0, 113, 10));
}

TEST_P(IceGathering, FinishesOnceEveryUsableServerAnswered) {
  Simulation sim{crypto, seed()};
  sim.network().add_stun_server(stun_server(), true);
  const auto nat =
      sim.network().add_nat(NatKind::symmetric, ipv4(203, 0, 113, 10));
  const auto peer =
      sim.add_peer(TransportRole::host,
                   {PeerSocket{at(ipv4(192, 168, 1, 10), 5000), nat, 0}},
                   {stun_server(), at(ipv6(1), 3478)});
  ASSERT_TRUE(gather_peer(sim, peer));
  EXPECT_EQ(gathered_at(sim, peer), 2 * latency_us);
  EXPECT_EQ(gathered(sim, peer).candidates().size(), 2U);
}

TEST_P(IceGathering, OffersAtMostTheMaximumCandidates) {
  Simulation sim{crypto, seed()};
  std::vector<PeerSocket> sockets;
  for (std::uint8_t index = 0; index < 40; ++index)
    sockets.push_back(public_peer(at(ipv4(203, 0, 113, index), 5000)));
  const auto peer = sim.add_peer(TransportRole::host, sockets, {});
  EXPECT_EQ(gathered(sim, peer).candidates().size(),
            libchiaking::net::kakusan::max_candidates);
}

TEST_P(IceGathering, RanksUsableBasesAndChecksFew) {
  Simulation sim{crypto, seed()};
  sim.network().add_stun_server(stun_server(), true);
  const std::vector<Endpoint> servers{stun_server()};
  const auto host = sim.add_peer(
      TransportRole::host,
      {PeerSocket{at(ipv4(203, 0, 113, 10), 5000), std::nullopt, 0},
       PeerSocket{at(ipv4(203, 0, 113, 11), 5001), std::nullopt, 10},
       PeerSocket{at(IpAddress::loopback_ipv4(), 5002), std::nullopt, 50},
       PeerSocket{at(ipv4(169, 254, 1, 1), 5003), std::nullopt, 50},
       PeerSocket{at(ipv4(203, 0, 113, 10), 5000), std::nullopt, 50},
       PeerSocket{at(ipv4(224, 0, 0, 9), 5004), std::nullopt, 50}},
      servers);
  ASSERT_TRUE(gather_peer(sim, host));
  const auto host_ice = gathered(sim, host);
  ASSERT_EQ(host_ice.candidates().size(), 2U);
  EXPECT_EQ(host_ice.candidates()[0].endpoint, at(ipv4(203, 0, 113, 11), 5001));
  EXPECT_GT(host_ice.candidates()[0].priority, host_ice.candidates()[1].priority);
  EXPECT_NE(host_ice.candidates()[0].foundation,
            host_ice.candidates()[1].foundation);

  const auto guest =
      sim.add_peer(TransportRole::guest,
                   {public_peer(at(ipv4(198, 51, 100, 20), 6000)),
                    public_peer(at(ipv4(198, 51, 100, 21), 6001))},
                   servers);
  ASSERT_TRUE(gather_peer(sim, guest));
  const auto guest_ice = gathered(sim, guest);
  ASSERT_EQ(guest_ice.candidates().size(), 2U);
  EXPECT_EQ(guest_ice.candidates()[0].endpoint, at(ipv4(198, 51, 100, 20), 6000));

  EXPECT_TRUE(sim.agent(guest).connect(host_ice, sim.now()));
  EXPECT_TRUE(sim.agent(host).connect(guest_ice, sim.now()));
  const auto start = sim.now();
  ASSERT_TRUE(sim.run_until(start + 10 * second, [&] {
    return count_events<event::Nominated>(sim, host) != 0 &&
           count_events<event::Nominated>(sim, guest) != 0;
  }));
  sim.run_for(2 * second);
  expect_agreement(sim, host, guest);
  const auto path = nominated_path(sim, host);
  EXPECT_EQ(path.socket, Simulation::socket_id(1));
  EXPECT_EQ(path.remote, at(ipv4(198, 51, 100, 20), 6000));
  EXPECT_LE(sim.network().stats(host).checked.size(), 4U);
}

TEST_P(IceGathering, PairsOnlyCandidatesOfOneFamilyOnDualStack) {
  Simulation sim{crypto, seed()};
  sim.network().add_stun_server(stun_server(), true);
  const std::vector<Endpoint> servers{stun_server()};
  const auto host = sim.add_peer(TransportRole::host,
                                 {public_peer(at(ipv4(203, 0, 113, 10), 5000)),
                                  public_peer(at(ipv6(0x10), 5000))},
                                 servers);
  ASSERT_TRUE(gather_peer(sim, host));
  const auto guest =
      sim.add_peer(TransportRole::guest,
                   {public_peer(at(ipv6(0x20), 6000)),
                    public_peer(at(ipv4(198, 51, 100, 20), 6000))},
                   servers);
  ASSERT_TRUE(gather_peer(sim, guest));
  const auto host_ice = gathered(sim, host);
  const auto guest_ice = gathered(sim, guest);
  EXPECT_EQ(host_ice.candidates().size(), 2U);
  EXPECT_EQ(guest_ice.candidates().size(), 2U);
  EXPECT_TRUE(sim.agent(guest).connect(host_ice, sim.now()));
  EXPECT_TRUE(sim.agent(host).connect(guest_ice, sim.now()));
  ASSERT_TRUE(sim.run_until(sim.now() + 10 * second, [&] {
    return count_events<event::Nominated>(sim, host) != 0 &&
           count_events<event::Nominated>(sim, guest) != 0;
  }));
  sim.run_for(2 * second);
  expect_agreement(sim, host, guest);
  EXPECT_LE(sim.network().stats(host).checked.size(), 2U);
  const auto path = nominated_path(sim, host);
  EXPECT_EQ(path.remote.address.family(), path.mapped.address.family());
}

TEST_P(IceGathering, FindsNoPathAcrossFamilies) {
  Simulation sim{crypto, seed()};
  const auto host = sim.add_peer(TransportRole::host,
                                 {public_peer(at(ipv6(0x10), 5000))}, {});
  const auto guest =
      sim.add_peer(TransportRole::guest,
                   {public_peer(at(ipv4(198, 51, 100, 20), 6000))}, {});
  const auto host_ice = gathered(sim, host);
  const auto guest_ice = gathered(sim, guest);
  EXPECT_TRUE(sim.agent(guest).connect(host_ice, sim.now()));
  const auto start = sim.now();
  EXPECT_TRUE(sim.agent(host).connect(guest_ice, start));
  sim.run_for(libchiaking::net::nat::ice::connectivity_timeout_us + second);
  EXPECT_EQ(failure(sim, host), Error::no_path);
  EXPECT_EQ(failed_at(sim, host),
            start + libchiaking::net::nat::ice::connectivity_timeout_us);
  EXPECT_EQ(sim.network().stats(host).check_requests, 0U);
  EXPECT_EQ(sim.network().stats(guest).check_requests, 0U);
  EXPECT_FALSE(failure(sim, guest));
}

INSTANTIATE_TEST_SUITE_P(Seeds, IceGathering,
                         ::testing::Range<std::uint64_t>(1, 7));

}  // namespace
}  // namespace nat_test
