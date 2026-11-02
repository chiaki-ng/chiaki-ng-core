#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace nat_test {
namespace {

namespace event = libchiaking::net::nat::ice::event;
namespace stun = libchiaking::net::nat::stun;
using libchiaking::net::nat::ice::consent_interval_min_us;
using libchiaking::net::nat::ice::consent_interval_span_us;
using libchiaking::net::nat::ice::consent_timeout_us;

class IceConsent : public SeededIceAgentRig {};

TEST_P(IceConsent, IsRefreshedAtItsIntervalThenLostWhenThePathIsCut) {
  auto session =
      prepare(seed(), Topology{NatKind::port_restricted, NatKind::none, true});
  auto& sim = *session.sim;
  ASSERT_NO_FATAL_FAILURE(expect_nominated(session, 10 * second));
  const auto before = sim.network().stats(session.host).check_requests;
  sim.run_for(300 * second);
  EXPECT_FALSE(failure(sim, session.host));
  EXPECT_FALSE(failure(sim, session.guest));
  EXPECT_TRUE(sim.agent(session.host).consent());
  EXPECT_TRUE(sim.agent(session.guest).consent());
  const auto consent_requests =
      sim.network().stats(session.host).check_requests - before;
  EXPECT_GE(consent_requests, 300U / 6);
  EXPECT_LE(consent_requests, 300U / 4 + 1);
  EXPECT_TRUE(sim.network().stats(session.host).min_gap_us >=
                  consent_interval_min_us ||
              before > 0);

  const auto cut = sim.now();
  sim.network().set_cut(true);
  sim.run_until(cut + 40 * second, [&] {
    return failure(sim, session.host) && failure(sim, session.guest);
  });
  for (const auto peer : {session.host, session.guest}) {
    SCOPED_TRACE(peer);
    const auto* record = find_event<event::Failed>(sim, peer);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(std::get<event::Failed>(record->event).error, Error::no_path);
    EXPECT_GT(record->at_us, cut + consent_timeout_us -
                                 consent_interval_min_us -
                                 consent_interval_span_us - 2 * latency_us);
    EXPECT_LE(record->at_us, cut + consent_timeout_us);
    EXPECT_FALSE(sim.agent(peer).consent());
    EXPECT_TRUE(sim.agent(peer).nominated());
    EXPECT_FALSE(sim.agent(peer).next_deadline_us());
    EXPECT_EQ(count_events<event::Failed>(sim, peer), 1U);
  }

  sim.network().set_cut(false);
  const auto host_endpoint = nominated_path(sim, session.guest).remote;
  const auto guest_endpoint = nominated_path(sim, session.host).remote;
  stun::Message check;
  check.kind = stun::MessageClass::request;
  check.transaction = stun::TransactionId{std::byte{7}, std::byte{7}};
  check.username = session.host_ice.ufrag().clone();
  *check.username += ':';
  *check.username += session.guest_ice.ufrag();
  check.priority = 1;
  check.ice_controlled = 5;
  auto encoded = check.encode(crypto,
                              libchiaking::net::kakusan::Hash::bytes_of(
                                  session.host_ice.password().view()),
                              stun::Fingerprint::present);
  ASSERT_TRUE(encoded);
  const auto answered = sim.network().packets_to(guest_endpoint);
  sim.network().inject(guest_endpoint, host_endpoint, encoded.value(),
                       sim.now());
  sim.run_for(second);
  EXPECT_EQ(sim.network().packets_to(guest_endpoint), answered + 1);
  EXPECT_EQ(count_events<event::Failed>(sim, session.host), 1U);
}

class IceMappingRefresh : public SeededIceAgentRig {
 protected:
  struct Refreshed {
    Simulation sim;
    std::size_t nat{};
    std::size_t peer{};
  };

  auto behind_nat(std::uint64_t binding_timeout_us)
      -> std::unique_ptr<Refreshed> {
    auto made = std::make_unique<Refreshed>(Simulation{crypto, seed()});
    auto& sim = made->sim;
    sim.network().set_latency(latency_us, 0);
    sim.network().set_binding_timeout(binding_timeout_us);
    sim.network().add_stun_server(stun_server(), true);
    made->nat =
        sim.network().add_nat(NatKind::port_restricted, ipv4(203, 0, 113, 10));
    made->peer = sim.add_peer(
        TransportRole::host,
        {PeerSocket{at(ipv4(192, 168, 1, 10), 5000), made->nat, 0}},
        {stun_server()});
    EXPECT_TRUE(gather_peer(sim, made->peer));
    return made;
  }
};

TEST_P(IceMappingRefresh, KeepsAnUnchangedMappingAlive) {
  auto made =
      behind_nat(libchiaking::net::kakusan::mapping_refresh_us + 5 * second);
  auto& sim = made->sim;
  const auto peer = made->peer;
  const auto gathered_time = gathered_at(sim, peer);
  ASSERT_TRUE(gathered_time);
  sim.run_for(5 * second);
  sim.run_until(*gathered_time + 61 * second,
                [&] { return failure(sim, peer).has_value(); });
  EXPECT_FALSE(failure(sim, peer));
  EXPECT_TRUE(sim.agent(peer).next_deadline_us());
}

TEST_P(IceMappingRefresh, FailsAtTheRefreshWhenTheMappingMoved) {
  auto made =
      behind_nat(libchiaking::net::kakusan::mapping_refresh_us + 5 * second);
  auto& sim = made->sim;
  const auto peer = made->peer;
  const auto gathered_time = gathered_at(sim, peer);
  ASSERT_TRUE(gathered_time);
  sim.run_for(5 * second);
  sim.network().rebind(made->nat);
  sim.run_until(*gathered_time + 61 * second,
                [&] { return failure(sim, peer).has_value(); });
  const auto* record = find_event<event::Failed>(sim, peer);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(std::get<event::Failed>(record->event).error, Error::no_path);
  EXPECT_GE(record->at_us,
            *gathered_time + libchiaking::net::kakusan::mapping_refresh_us);
  EXPECT_LE(record->at_us, *gathered_time +
                               libchiaking::net::kakusan::mapping_refresh_us +
                               4 * latency_us);
  EXPECT_FALSE(sim.agent(peer).next_deadline_us());
}

TEST_P(IceMappingRefresh, FailsWhenTheBindingExpiresBeforeTheRefresh) {
  auto made =
      behind_nat(libchiaking::net::kakusan::mapping_refresh_us - 5 * second);
  auto& sim = made->sim;
  sim.run_for(libchiaking::net::kakusan::mapping_refresh_us + second);
  EXPECT_EQ(failure(sim, made->peer), Error::no_path);
}

class SwitchableEntropy final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  explicit SwitchableEntropy(std::uint64_t seed) noexcept : source_(seed) {}

  void fail_after(std::size_t draws) noexcept { remaining_ = draws; }

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    if (remaining_) {
      if (*remaining_ == 0) {
        std::fill(output.begin(), output.end(), std::byte{0x55});
        return libchiaking::unexpected(
            libchiaking::platform::PlatformError::system_failure);
      }
      --*remaining_;
    }
    return source_.fill_bytes(output);
  }

 private:
  test_support::SeededRandom source_;
  std::optional<std::size_t> remaining_;
};

class IceConsentEntropy : public IceAgentRig,
                          public ::testing::TestWithParam<std::size_t> {};

TEST_P(IceConsentEntropy, StopsTheAgentWhenAConsentCheckCannotBeDrawn) {
  const auto successful_draws = GetParam();
  SwitchableEntropy host_entropy{91};
  SwitchableEntropy guest_entropy{92};
  const auto host_endpoint = at(ipv4(192, 0, 2, 10), 50000);
  const auto guest_endpoint = at(ipv4(192, 0, 2, 20), 50001);
  AgentConfig host_config;
  host_config.bases.push_back(Base{1, host_endpoint, 0});
  AgentConfig guest_config;
  guest_config.role = TransportRole::guest;
  guest_config.bases.push_back(Base{2, guest_endpoint, 0});
  auto host = Agent::create(crypto, host_entropy, std::move(host_config), 0);
  auto guest = Agent::create(crypto, guest_entropy, std::move(guest_config), 0);
  ASSERT_TRUE(host);
  ASSERT_TRUE(guest);
  auto host_local = host.value()->take_event();
  auto guest_local = guest.value()->take_event();
  ASSERT_TRUE(host_local);
  ASSERT_TRUE(guest_local);
  EXPECT_TRUE(
      host.value()->connect(std::get<event::Gathered>(*guest_local).local, 0));
  EXPECT_TRUE(
      guest.value()->connect(std::get<event::Gathered>(*host_local).local, 0));
  for (std::uint64_t now = 0; now < 10 * second; now += latency_us) {
    std::vector<Outgoing> outgoing;
    host.value()->poll(now, outgoing);
    guest.value()->poll(now, outgoing);
    for (std::size_t round = 0; !outgoing.empty() && round < 8; ++round) {
      std::vector<Outgoing> replies;
      for (const auto& sent : outgoing) {
        if (sent.socket == 1)
          guest.value()->receive(2, host_endpoint, sent.bytes, now, replies);
        else
          host.value()->receive(1, guest_endpoint, sent.bytes, now, replies);
      }
      outgoing = std::move(replies);
    }
    EXPECT_TRUE(outgoing.empty());
    if (host.value()->nominated() && guest.value()->nominated()) break;
  }
  ASSERT_TRUE(host.value()->nominated());
  ASSERT_TRUE(guest.value()->nominated());
  while (const auto seen = host.value()->take_event())
    EXPECT_FALSE(std::holds_alternative<event::Failed>(*seen));
  host_entropy.fail_after(successful_draws);
  const auto deadline = host.value()->next_deadline_us();
  ASSERT_TRUE(deadline);
  std::vector<Outgoing> outgoing;
  host.value()->poll(*deadline, outgoing);
  EXPECT_TRUE(outgoing.empty());
  const auto failed = host.value()->take_event();
  ASSERT_TRUE(failed);
  ASSERT_TRUE(std::holds_alternative<event::Failed>(*failed));
  EXPECT_EQ(std::get<event::Failed>(*failed).error, Error::crypto_unavailable);
  EXPECT_FALSE(host.value()->next_deadline_us());
  EXPECT_FALSE(host.value()->take_event());
}

INSTANTIATE_TEST_SUITE_P(Seeds, IceConsent,
                         ::testing::Range<std::uint64_t>(1, 7));
INSTANTIATE_TEST_SUITE_P(Seeds, IceMappingRefresh,
                         ::testing::Range<std::uint64_t>(1, 7));
INSTANTIATE_TEST_SUITE_P(SuccessfulDraws, IceConsentEntropy,
                         ::testing::Values<std::size_t>(0, 1));

}  // namespace
}  // namespace nat_test
