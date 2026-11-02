#include "tests/support/bytes.hpp"
#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nat_test {
namespace {

namespace stun = libchiaking::net::nat::stun;

class IceCredentials : public SeededIceAgentRig {};

TEST_P(IceCredentials, RejectChecksSignedWithTheWrongPassword) {
  auto session = prepare_with(seed(), [](const IceParameters& ice) {
    auto password = ice.password().clone();
    password[0] = password[0] == 'a' ? 'b' : 'a';
    return with_credentials(ice, ice.ufrag().clone(), std::move(password));
  });
  expect_no_path(session);
}

TEST_P(IceCredentials, RejectChecksForTheWrongUsername) {
  auto session = prepare_with(seed(), [](const IceParameters& ice) {
    auto ufrag = ice.ufrag().clone();
    ufrag[3] = ufrag[3] == 'a' ? 'b' : 'a';
    return with_credentials(ice, std::move(ufrag), ice.password().clone());
  });
  expect_no_path(session);
}

TEST_P(IceCredentials, RejectChecksWhoseIntegrityWasTamperedWith) {
  Session session;
  session.sim = std::make_unique<Simulation>(crypto, seed());
  auto& sim = *session.sim;
  sim.network().tamper = [](std::vector<std::byte>& bytes) {
    const auto decoded = stun::ReceivedMessage::decode(bytes);
    if (!decoded || !decoded.value().integrity_offset) return;
    bytes[*decoded.value().integrity_offset + 4 + 7] ^= std::byte{0x40};
    if (decoded.value().fingerprinted) {
      bytes.resize(bytes.size() - 8);
      stun::Message::append_fingerprint(bytes);
    }
  };
  session.host = sim.add_peer(
      TransportRole::host, {public_peer(at(ipv4(203, 0, 113, 10), 5000))}, {});
  session.guest =
      sim.add_peer(TransportRole::guest,
                   {public_peer(at(ipv4(198, 51, 100, 20), 6000))}, {});
  session.host_ice = gathered(sim, session.host);
  session.guest_ice = gathered(sim, session.guest);
  EXPECT_TRUE(sim.agent(session.guest).connect(session.host_ice, sim.now()));
  session.connect_us = sim.now();
  EXPECT_TRUE(sim.agent(session.host).connect(session.guest_ice, sim.now()));
  expect_no_path(session);
  EXPECT_GT(sim.network().stats(session.host).check_requests, 0U);
}

class IceForgeries : public SeededIceAgentRig {
 protected:
  void inject_forgeries(Simulation& sim, const IceParameters& host_ice,
                        const IceParameters& guest_ice,
                        const Endpoint& target) {
    const auto username = std::string{host_ice.ufrag().view()} + ":" +
                          std::string{guest_ice.ufrag().view()};
    auto& network = sim.network();
    network.inject(attacker(), target,
                   forged_request(username, std::string(48, '0'), true),
                   sim.now());
    network.inject(attacker(), target,
                   forged_request(username, host_ice.password().view(), false),
                   sim.now());
    network.inject(attacker(), target,
                   forged_request(std::string{host_ice.ufrag().view()} + ":" +
                                      std::string(16, 'f'),
                                  std::string(48, 'f'), true),
                   sim.now());
    auto stripped = forged_request(username, host_ice.password().view(), true);
    ASSERT_GE(stripped.size(), 8U);
    stripped.resize(stripped.size() - 8);
    stripped[3] =
        static_cast<std::byte>(std::to_integer<std::uint8_t>(stripped[3]) - 8);
    network.inject(attacker(), target, stripped, sim.now());

    stun::Message response;
    response.kind = stun::MessageClass::success;
    response.transaction = stun::TransactionId{std::byte{9}};
    response.xor_mapped = attacker();
    auto encoded = response.encode(
        crypto,
        libchiaking::net::kakusan::Hash::bytes_of(guest_ice.password().view()),
        stun::Fingerprint::present);
    ASSERT_TRUE(encoded);
    network.inject(attacker(), target, encoded.value(), sim.now());

    auto rcpm = test_support::Bytes::of("RCPM");
    rcpm.resize(96, std::byte{0x11});
    network.inject(attacker(), target, rcpm, sim.now());
    std::vector<std::byte> record(48, std::byte{0x17});
    record[0] = std::byte{23};
    network.inject(attacker(), target, record, sim.now());
  }
};

TEST_P(IceForgeries, AreNeitherAnsweredNorAllowedToChangeThePath) {
  Session session;
  session.sim = std::make_unique<Simulation>(crypto, seed());
  auto& sim = *session.sim;
  const auto target = at(ipv4(203, 0, 113, 10), 5000);
  session.host = sim.add_peer(TransportRole::host, {public_peer(target)}, {});
  session.guest =
      sim.add_peer(TransportRole::guest,
                   {public_peer(at(ipv4(198, 51, 100, 20), 6000))}, {});
  session.host_ice = gathered(sim, session.host);
  session.guest_ice = gathered(sim, session.guest);

  ASSERT_NO_FATAL_FAILURE(
      inject_forgeries(sim, session.host_ice, session.guest_ice, target));
  sim.run_for(second);
  EXPECT_EQ(sim.network().packets_to(attacker()), 0U);
  EXPECT_EQ(sim.events(session.host).size(), 1U);

  EXPECT_TRUE(sim.agent(session.guest).connect(session.host_ice, sim.now()));
  session.connect_us = sim.now();
  EXPECT_TRUE(sim.agent(session.host).connect(session.guest_ice, sim.now()));
  ASSERT_NO_FATAL_FAILURE(
      inject_forgeries(sim, session.host_ice, session.guest_ice, target));
  ASSERT_NO_FATAL_FAILURE(expect_nominated(session, 5 * second));
  const auto path = nominated_path(sim, session.host);
  ASSERT_NO_FATAL_FAILURE(
      inject_forgeries(sim, session.host_ice, session.guest_ice, target));
  sim.run_for(10 * second);
  EXPECT_EQ(sim.network().packets_to(attacker()), 0U);
  EXPECT_EQ(sim.events(session.host).size(), 2U);
  ASSERT_TRUE(sim.agent(session.host).nominated());
  EXPECT_TRUE(same_path(*sim.agent(session.host).nominated(), path));
  EXPECT_TRUE(sim.agent(session.host).consent());
}

INSTANTIATE_TEST_SUITE_P(Seeds, IceCredentials,
                         ::testing::Range<std::uint64_t>(1, 7));
INSTANTIATE_TEST_SUITE_P(Seeds, IceForgeries,
                         ::testing::Range<std::uint64_t>(1, 7));

}  // namespace
}  // namespace nat_test
