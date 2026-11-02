#pragma once

#include "src/core/net/nat/ice/agent.hpp"
#include "src/core/net/nat/stun/message.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/net/ice_network.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nat_test {

using test_support::net::Addresses;

class IceAgentRig {
 protected:
  using Agent = libchiaking::net::nat::ice::Agent;
  using AgentConfig = libchiaking::net::nat::ice::AgentConfig;
  using Base = libchiaking::net::nat::ice::Base;
  using Error = libchiaking::net::nat::ice::Error;
  using IceParameters = libchiaking::net::kakusan::IceParameters;
  using CandidateType = libchiaking::net::kakusan::CandidateType;
  using NatKind = test_support::net::NatKind;
  using Outgoing = libchiaking::net::nat::ice::Outgoing;
  using Path = libchiaking::net::nat::ice::Path;
  using PeerSocket = test_support::net::PeerSocket;
  using PortGuessing = libchiaking::net::nat::ice::PortGuessing;
  using Recorded = test_support::net::Recorded;
  using Role = libchiaking::net::nat::ice::Role;
  using Simulation = test_support::net::Simulation;
  using TransportRole = libchiaking::net::kakusan::TransportRole;
  using Endpoint = libchiaking::platform::SocketEndpoint;
  using IpAddress = libchiaking::platform::IpAddress;

  struct Topology {
    NatKind host_nat{NatKind::none};
    NatKind guest_nat{NatKind::none};
    bool stun{true};
    std::uint64_t head_start_us{};
    std::uint64_t loss_permille{};
    std::uint64_t jitter_us{};
    TransportRole host_role{TransportRole::host};
    TransportRole guest_role{TransportRole::guest};
    std::uint64_t host_wait_us{};
    PortGuessing host_guessing{PortGuessing::automatic};
    PortGuessing guest_guessing{PortGuessing::automatic};
  };

  struct Session {
    std::unique_ptr<Simulation> sim;
    std::size_t host{};
    std::size_t guest{};
    std::uint64_t connect_us{};
    IceParameters host_ice{placeholder()};
    IceParameters guest_ice{placeholder()};
    std::size_t guest_requests_before_connect{};
  };

  static constexpr std::uint64_t second = 1'000'000;
  static constexpr std::uint64_t latency_us = 20'000;

  static auto ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                   std::uint8_t d) -> IpAddress {
    return Addresses::ipv4(a, b, c, d);
  }

  static auto ipv6(std::uint16_t last) -> IpAddress {
    return Addresses::ipv6(last);
  }

  static auto at(const IpAddress& address, std::uint16_t port) -> Endpoint {
    return Addresses::at(address, port);
  }

  static auto stun_server() -> Endpoint { return at(ipv4(192, 0, 2, 1), 3478); }

  static auto dead_server() -> Endpoint { return at(ipv4(192, 0, 2, 2), 3478); }

  static auto attacker() -> Endpoint {
    return at(ipv4(198, 51, 100, 66), 7777);
  }

  template <class T>
  static auto count_events(const Simulation& sim, std::size_t peer)
      -> std::size_t {
    std::size_t count = 0;
    for (const auto& record : sim.events(peer))
      count += std::holds_alternative<T>(record.event) ? 1U : 0U;
    return count;
  }

  template <class T>
  static auto find_event(const Simulation& sim, std::size_t peer)
      -> const Recorded* {
    for (const auto& record : sim.events(peer)) {
      if (std::holds_alternative<T>(record.event)) return &record;
    }
    return nullptr;
  }

  static auto gathered(const Simulation& sim, std::size_t peer)
      -> IceParameters {
    namespace event = libchiaking::net::nat::ice::event;
    EXPECT_EQ(count_events<event::Gathered>(sim, peer), 1U);
    const auto* record = find_event<event::Gathered>(sim, peer);
    if (record == nullptr) return placeholder();
    return std::get<event::Gathered>(record->event).local.clone();
  }

  static auto nominated_path(const Simulation& sim, std::size_t peer) -> Path {
    namespace event = libchiaking::net::nat::ice::event;
    EXPECT_EQ(count_events<event::Nominated>(sim, peer), 1U);
    const auto* record = find_event<event::Nominated>(sim, peer);
    if (record == nullptr) return {};
    return std::get<event::Nominated>(record->event).path;
  }

  [[nodiscard]] static auto failure(const Simulation& sim, std::size_t peer)
      -> std::optional<Error> {
    namespace event = libchiaking::net::nat::ice::event;
    const auto* record = find_event<event::Failed>(sim, peer);
    if (record == nullptr) return std::nullopt;
    return std::get<event::Failed>(record->event).error;
  }

  static auto failed_at(const Simulation& sim, std::size_t peer)
      -> std::optional<std::uint64_t> {
    namespace event = libchiaking::net::nat::ice::event;
    const auto* record = find_event<event::Failed>(sim, peer);
    if (record == nullptr) return std::nullopt;
    return record->at_us;
  }

  static auto gathered_at(const Simulation& sim, std::size_t peer)
      -> std::optional<std::uint64_t> {
    namespace event = libchiaking::net::nat::ice::event;
    const auto* record = find_event<event::Gathered>(sim, peer);
    if (record == nullptr) return std::nullopt;
    return record->at_us;
  }

  static auto same_path(const Path& left, const Path& right) -> bool {
    return left.socket == right.socket && left.mapped == right.mapped &&
           left.remote == right.remote;
  }

  static auto has_type(const IceParameters& ice, CandidateType type) -> bool {
    for (const auto& candidate : ice.candidates()) {
      if (candidate.type == type) return true;
    }
    return false;
  }

  static auto side(Simulation& sim, NatKind nat,
                   const IpAddress& public_address,
                   const IpAddress& private_address, std::uint16_t port)
      -> PeerSocket {
    if (nat == NatKind::none)
      return PeerSocket{at(public_address, port), std::nullopt, 0};
    const auto index = sim.network().add_nat(nat, public_address);
    return PeerSocket{at(private_address, port), index, 0};
  }

  static auto public_peer(const Endpoint& endpoint) -> PeerSocket {
    return PeerSocket{endpoint, std::nullopt, 0};
  }

  static auto gather_peer(Simulation& sim, std::size_t peer)
      -> ::testing::AssertionResult {
    namespace event = libchiaking::net::nat::ice::event;
    const auto done = sim.run_until(
        sim.now() + libchiaking::net::kakusan::gathering_timeout_us + second,
        [&] {
          return count_events<event::Gathered>(sim, peer) != 0 ||
                 count_events<event::Failed>(sim, peer) != 0;
        });
    if (!done)
      return ::testing::AssertionFailure()
             << "peer " << peer << " did not finish gathering";
    return ::testing::AssertionSuccess();
  }

  auto prepare(std::uint64_t seed, const Topology& topology) -> Session {
    Session session;
    session.sim = std::make_unique<Simulation>(crypto, seed);
    auto& sim = *session.sim;
    sim.network().set_latency(latency_us, topology.jitter_us);
    std::vector<Endpoint> servers;
    if (topology.stun) {
      sim.network().add_stun_server(stun_server(), true);
      servers.push_back(stun_server());
    }
    const auto host_socket = side(sim, topology.host_nat, ipv4(203, 0, 113, 10),
                                  ipv4(192, 168, 1, 10), 5000);
    const auto guest_socket =
        side(sim, topology.guest_nat, ipv4(198, 51, 100, 20),
             ipv4(10, 0, 0, 20), 6000);
    session.host = sim.add_peer(topology.host_role, {host_socket}, servers,
                                topology.host_guessing);
    EXPECT_TRUE(gather_peer(sim, session.host));
    session.host_ice = gathered(sim, session.host);
    sim.run_for(topology.host_wait_us);
    session.guest = sim.add_peer(topology.guest_role, {guest_socket}, servers,
                                 topology.guest_guessing);
    EXPECT_TRUE(gather_peer(sim, session.guest));
    session.guest_ice = gathered(sim, session.guest);
    sim.network().set_loss_permille(topology.loss_permille);
    EXPECT_TRUE(sim.agent(session.guest).connect(session.host_ice, sim.now()));
    sim.run_for(topology.head_start_us);
    session.guest_requests_before_connect =
        sim.network().stats(session.guest).check_requests;
    session.connect_us = sim.now();
    EXPECT_TRUE(sim.agent(session.host).connect(session.guest_ice, sim.now()));
    return session;
  }

  auto prepare_public(std::uint64_t seed) -> Session {
    Session fresh;
    fresh.sim = std::make_unique<Simulation>(crypto, seed);
    auto& next = *fresh.sim;
    fresh.host =
        next.add_peer(TransportRole::host,
                      {public_peer(at(ipv4(203, 0, 113, 10), 5000))}, {});
    fresh.guest =
        next.add_peer(TransportRole::guest,
                      {public_peer(at(ipv4(198, 51, 100, 20), 6000))}, {});
    fresh.host_ice = gathered(next, fresh.host);
    fresh.guest_ice = gathered(next, fresh.guest);
    return fresh;
  }

  auto prepare_with(std::uint64_t seed,
                    const std::function<IceParameters(const IceParameters&)>&
                        corrupt)
      -> Session {
    Session fresh;
    fresh.sim = std::make_unique<Simulation>(crypto, seed);
    auto& next = *fresh.sim;
    next.network().set_latency(latency_us, 0);
    fresh.host =
        next.add_peer(TransportRole::host,
                      {public_peer(at(ipv4(203, 0, 113, 10), 5000))}, {});
    fresh.guest =
        next.add_peer(TransportRole::guest,
                      {public_peer(at(ipv4(198, 51, 100, 20), 6000))}, {});
    fresh.host_ice = gathered(next, fresh.host);
    fresh.guest_ice = gathered(next, fresh.guest);
    EXPECT_TRUE(next.agent(fresh.guest).connect(fresh.host_ice, next.now()));
    auto damaged = corrupt(fresh.guest_ice);
    fresh.connect_us = next.now();
    EXPECT_TRUE(next.agent(fresh.host).connect(damaged, next.now()));
    return fresh;
  }

  static auto run_to_nomination(Session& session, std::uint64_t limit_us)
      -> bool {
    namespace event = libchiaking::net::nat::ice::event;
    auto& sim = *session.sim;
    return sim.run_until(limit_us, [&] {
      return count_events<event::Nominated>(sim, session.host) != 0 &&
             count_events<event::Nominated>(sim, session.guest) != 0;
    });
  }

  static void expect_agreement(Simulation& sim, std::size_t left,
                               std::size_t right) {
    const auto left_path = nominated_path(sim, left);
    const auto right_path = nominated_path(sim, right);
    EXPECT_EQ(left_path.mapped, right_path.remote);
    EXPECT_EQ(left_path.remote, right_path.mapped);
    ASSERT_TRUE(sim.agent(left).nominated());
    EXPECT_TRUE(same_path(*sim.agent(left).nominated(), left_path));
    ASSERT_TRUE(sim.agent(right).nominated());
    EXPECT_TRUE(same_path(*sim.agent(right).nominated(), right_path));
    EXPECT_TRUE(sim.agent(left).consent());
    EXPECT_TRUE(sim.agent(right).consent());
  }

  static void expect_nominated(Session& session, std::uint64_t within_us) {
    auto& sim = *session.sim;
    ASSERT_TRUE(run_to_nomination(session, session.connect_us + within_us));
    sim.run_for(2 * second);
    expect_agreement(sim, session.host, session.guest);
    EXPECT_FALSE(failure(sim, session.host));
    EXPECT_FALSE(failure(sim, session.guest));
  }

  static void expect_no_path(Session& session) {
    namespace event = libchiaking::net::nat::ice::event;
    auto& sim = *session.sim;
    sim.run_until(
        session.connect_us +
            libchiaking::net::nat::ice::connectivity_timeout_us + second,
        [&] { return count_events<event::Failed>(sim, session.host) != 0; });
    EXPECT_EQ(failure(sim, session.host), Error::no_path);
    sim.run_for(10 * second);
    EXPECT_EQ(count_events<event::Nominated>(sim, session.host), 0U);
    EXPECT_EQ(count_events<event::Nominated>(sim, session.guest), 0U);
    EXPECT_FALSE(failure(sim, session.guest));
  }

  auto forged_request(const std::string& username, std::string_view password,
                      bool integrity) -> std::vector<std::byte> {
    namespace stun = libchiaking::net::nat::stun;
    stun::Message message;
    message.kind = stun::MessageClass::request;
    message.transaction =
        stun::TransactionId{std::byte{1}, std::byte{2}, std::byte{3}};
    message.username = libchiaking::SecretString{username};
    message.priority = 0x6e0000ffU;
    message.ice_controlling = 1;
    message.use_candidate = true;
    const auto key = libchiaking::net::kakusan::Hash::bytes_of(password);
    const auto used = integrity ? key : std::span<const std::byte>{};
    auto encoded = message.encode(crypto, used, stun::Fingerprint::present);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::move(encoded.value());
  }

  static auto remote_with_one_candidate(const Endpoint& endpoint,
                                        std::uint32_t priority)
      -> IceParameters {
    return with_credentials(
        {libchiaking::net::kakusan::Candidate{
            "a", priority, endpoint, CandidateType::host, {}}},
        0, libchiaking::SecretString{"0123456789abcdef"},
        libchiaking::SecretString{std::string(48, 'c')});
  }

  static auto with_credentials(
      std::vector<libchiaking::net::kakusan::Candidate> candidates,
      std::size_t port_guesses, libchiaking::SecretString ufrag,
      libchiaking::SecretString password) -> IceParameters {
    auto credentials = libchiaking::net::nat::ice::Credentials::make(
        std::move(ufrag), std::move(password));
    EXPECT_TRUE(credentials);
    if (!credentials) return placeholder();
    auto made = IceParameters::make(std::move(credentials.value()),
                                    std::move(candidates), port_guesses);
    EXPECT_TRUE(made);
    if (!made) return placeholder();
    return std::move(made.value());
  }

  static auto with_credentials(const IceParameters& ice,
                               libchiaking::SecretString ufrag,
                               libchiaking::SecretString password)
      -> IceParameters {
    return with_credentials({ice.candidates().begin(), ice.candidates().end()},
                            ice.port_guesses(), std::move(ufrag),
                            std::move(password));
  }

  static auto placeholder() -> IceParameters {
    auto credentials = libchiaking::net::nat::ice::Credentials::make(
        libchiaking::SecretString{"0123456789abcdef"},
        libchiaking::SecretString{std::string(48, 'c')});
    return IceParameters::make(
               std::move(credentials.value()),
               {libchiaking::net::kakusan::Candidate{
                   "p", 1, Endpoint{ipv4(203, 0, 113, 99), 9},
                   CandidateType::host, {}}},
               0)
        .value();
  }

  test_support::SoftwareCrypto crypto;
};

class SeededIceAgentRig : public IceAgentRig,
                          public ::testing::TestWithParam<std::uint64_t> {
 protected:
  auto seed() const -> std::uint64_t { return GetParam(); }
};

}  // namespace nat_test
