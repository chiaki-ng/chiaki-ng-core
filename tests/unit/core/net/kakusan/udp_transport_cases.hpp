#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/kakusan/secure_datagrams.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/support/net/ice_network.hpp"
#include "tests/support/net/simulated_network.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace net_kakusan_test {

using test_support::net::Addresses;
namespace {

namespace kakusan = libchiaking::net::kakusan;
namespace platform = libchiaking::platform;
namespace transport_event = libchiaking::net::kakusan::transport_event;

class PathRig : public ::testing::Test {
 public:
  using Bytes = std::vector<std::byte>;

  static constexpr std::uint64_t ms = 1000;
  static constexpr std::uint64_t second = 1'000'000;
  static constexpr std::uint64_t latency_us = 20 * ms;
  static constexpr std::size_t max_record = 1163;

  static auto steady() -> const platform::SteadyClock& {
    static const platform::SteadyClock clock;
    return clock;
  }

  static auto stun_server() -> platform::SocketEndpoint {
    return Addresses::at(Addresses::ipv4(192, 0, 2, 200), 3478);
  }

  static auto pattern(std::size_t size, std::uint8_t seed) -> Bytes {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index)
      bytes[index] =
          static_cast<std::byte>((index * 29U + seed * 7U + 1U) & 0xffU);
    return bytes;
  }

  static auto media(std::size_t size, std::uint8_t seed) -> Bytes {
    auto bytes = pattern(size, seed);
    const std::string_view prefix = kakusan::media_prefix;
    for (std::size_t index = 0; index < prefix.size() && index < size; ++index)
      bytes[index] = static_cast<std::byte>(prefix[index]);
    return bytes;
  }

  static auto text(std::string_view value) -> Bytes {
    const auto bytes = std::as_bytes(std::span{value});
    return {bytes.begin(), bytes.end()};
  }

  static auto stand_in() -> kakusan::TransportDescription {
    auto credentials = kakusan::IceCredentials::make(
        libchiaking::SecretString{"0123456789abcdef"},
        libchiaking::SecretString{std::string(48, 'c')});
    auto ice = kakusan::IceParameters::make(
        std::move(credentials.value()),
        {kakusan::Candidate{"a", 1,
                            platform::SocketEndpoint{
                                Addresses::ipv4(198, 51, 100, 99), 9000},
                            kakusan::CandidateType::host,
                            {}}},
        0);
    return kakusan::TransportDescription{std::move(ice.value()), {}};
  }

  static auto fails_with(const kakusan::Result<void>& result,
                         kakusan::Error error) -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it succeeded";
    if (result.error() != error)
      return ::testing::AssertionFailure()
             << "it failed with " << ::testing::PrintToString(result.error());
    return ::testing::AssertionSuccess();
  }

 protected:
  test_support::SoftwareCrypto crypto;
  platform::Sleeper sleeper{test_support::process_threading(), steady()};
};

class Recorded {
 public:
  std::uint64_t at_us{};
  kakusan::TransportEvent event;
};

template <class Provider>
class PathPeer {
 public:
  using Bytes = std::vector<std::byte>;

  PathPeer(platform::CryptoProvider& crypto,
           test_support::net::SimulatedNetwork& network,
           std::vector<platform::InterfaceInfo> interfaces,
           std::optional<std::size_t> nat, kakusan::UdpTransportConfig config,
           std::uint64_t seed)
      : machine(network, std::move(interfaces), nat),
        entropy(seed),
        factory(
            kakusan::UdpTransportDependencies{
                machine, crypto, entropy,
                test_support::kakusan::SecureDatagrams<Provider>::factory()},
            std::move(config)) {}

  test_support::net::SimulatedMachine machine;
  test_support::SeededRandom entropy;
  kakusan::UdpTransportFactory factory;
  std::unique_ptr<kakusan::SpokeTransport> transport;
  std::vector<Recorded> events;
  std::optional<kakusan::TransportDescription> local;

  auto endpoint() const -> platform::SocketEndpoint {
    if (!local || local->ice.candidates().empty()) {
      ADD_FAILURE() << "the peer has gathered no candidate";
      return {};
    }
    return local->ice.candidates().front().endpoint;
  }

  template <class Event>
  auto count() const -> std::size_t {
    return static_cast<std::size_t>(
        std::ranges::count_if(events, [](const Recorded& item) {
          return std::holds_alternative<Event>(item.event);
        }));
  }

  template <class Event>
  auto first() const -> const Recorded* {
    const auto found = std::ranges::find_if(events, [](const Recorded& item) {
      return std::holds_alternative<Event>(item.event);
    });
    return found == events.end() ? nullptr : &*found;
  }

  auto failure() const -> std::optional<kakusan::Error> {
    const auto* failed = first<transport_event::Failed>();
    if (failed == nullptr) return std::nullopt;
    return std::get<transport_event::Failed>(failed->event).error;
  }

  auto established() const -> bool {
    return count<transport_event::Established>() != 0;
  }

  auto records() const -> std::vector<Bytes> {
    std::vector<Bytes> found;
    for (const auto& item : events) {
      if (const auto* record =
              std::get_if<transport_event::Record>(&item.event))
        found.emplace_back(record->bytes.begin(), record->bytes.end());
    }
    return found;
  }

  auto datagrams() const -> std::vector<Bytes> {
    std::vector<Bytes> found;
    for (const auto& item : events) {
      if (const auto* datagram =
              std::get_if<transport_event::Datagram>(&item.event))
        found.push_back(datagram->bytes);
    }
    return found;
  }
};

template <class Provider>
class PathWorld {
 public:
  using Peer = PathPeer<Provider>;
  using Bytes = std::vector<std::byte>;

  PathWorld(platform::CryptoProvider& crypto, platform::Sleeper& sleeper,
            std::uint64_t seed)
      : network(crypto, seed), crypto_(crypto), sleeper_(sleeper) {
    network.network().set_latency(PathRig::latency_us, 0);
  }

  test_support::net::SimulatedNetwork network;
  std::vector<std::unique_ptr<Peer>> peers;
  bool pace_handshakes{true};

  auto now() const -> std::uint64_t { return network.now(); }

  auto handshaking() const -> bool {
    return std::ranges::any_of(peers, [](const std::unique_ptr<Peer>& peer) {
      return peer->transport &&
             peer->template count<transport_event::Nominated>() != 0 &&
             !peer->established() && !peer->failure();
    });
  }

  void advance(std::uint64_t to_us) {
    if (pace_handshakes && to_us > now() && handshaking())
      sleeper_.sleep_for(std::chrono::microseconds{
          static_cast<std::chrono::microseconds::rep>(to_us - now())});
    network.advance_to(to_us);
  }

  auto add(kakusan::TransportRole role,
           std::vector<platform::InterfaceInfo> interfaces,
           std::optional<std::size_t> nat = std::nullopt,
           kakusan::UdpTransportConfig config = {}) -> Peer& {
    peers.push_back(
        std::make_unique<Peer>(crypto_, network, std::move(interfaces), nat,
                               std::move(config), 0x7000 + peers.size()));
    auto& peer = *peers.back();
    auto created = peer.factory.create(role);
    EXPECT_TRUE(created);
    if (created) peer.transport = std::move(created).value();
    return peer;
  }

  auto add_public(kakusan::TransportRole role,
                  const platform::IpAddress& address) -> Peer& {
    return add(role, {Addresses::interface_on("en0", address)});
  }

  auto add_behind(kakusan::TransportRole role, test_support::net::NatKind kind,
                  const platform::IpAddress& public_address,
                  const platform::IpAddress& private_address) -> Peer& {
    const auto nat = network.network().add_nat(kind, public_address);
    kakusan::UdpTransportConfig config;
    config.stun_servers.push_back(PathRig::stun_server());
    return add(role, {Addresses::interface_on("en0", private_address)}, nat,
               std::move(config));
  }

  void pump(Peer& peer) {
    if (!peer.transport) return;
    std::vector<kakusan::TransportEvent> events;
    EXPECT_TRUE(peer.transport->pump(now(), events));
    for (auto& event : events) {
      if (const auto* gathered = std::get_if<transport_event::Gathered>(&event))
        peer.local = gathered->local.clone();
      peer.events.push_back(Recorded{now(), std::move(event)});
    }
    const auto deadline = peer.transport->next_deadline_us();
    EXPECT_TRUE(!deadline || *deadline > now() || peer.machine.readable());
  }

  void pump_all() {
    for (auto& peer : peers) pump(*peer);
  }

  template <class Done>
  auto run_until(std::uint64_t limit_us, Done done) -> bool {
    std::size_t spins = 0;
    while (!done()) {
      auto next = network.next_delivery_us();
      for (const auto& peer : peers) {
        if (peer->transport)
          next = libchiaking::timing::Deadline::earliest(
              next, peer->transport->next_deadline_us());
      }
      if (network.backlog() != 0) next = now();
      if (!next || *next > limit_us) {
        advance(limit_us);
        pump_all();
        return done();
      }
      if (*next <= now()) {
        ++spins;
        EXPECT_LT(spins, 10'000U);
        if (spins >= 10'000) return false;
      } else {
        spins = 0;
      }
      advance(*next);
      pump_all();
    }
    return true;
  }

  void run_for(std::uint64_t duration_us) {
    run_until(now() + duration_us, [] { return false; });
  }

  void gather(Peer& peer) {
    ASSERT_TRUE(peer.transport);
    ASSERT_TRUE(peer.transport->gather(now(), std::nullopt));
    ASSERT_TRUE(run_until(now() + 20 * PathRig::second, [&] {
      return peer.local.has_value() || peer.failure().has_value();
    }));
  }

  void connect(Peer& host, Peer& guest,
               std::optional<kakusan::Digest256> guest_pin = std::nullopt) {
    ASSERT_NO_FATAL_FAILURE(gather(host));
    ASSERT_NO_FATAL_FAILURE(gather(guest));
    ASSERT_TRUE(host.local);
    ASSERT_TRUE(guest.local);
    ASSERT_TRUE(guest.transport->connect(*host.local, now()));
    auto remote = guest.local->clone();
    if (guest_pin) remote.certificate_digest = *guest_pin;
    ASSERT_TRUE(host.transport->connect(remote, now()));
  }

  void establish(Peer& host, Peer& guest, std::uint64_t within_us) {
    ASSERT_NO_FATAL_FAILURE(connect(host, guest));
    const auto started = now();
    ASSERT_TRUE(run_until(started + within_us, [&] {
      return host.established() && guest.established();
    }));
  }

  auto deliver(Peer& from, Peer& to, const Bytes& record) -> bool {
    for (std::size_t attempt = 0; attempt < 30; ++attempt) {
      if (!from.transport->send_record(record)) return false;
      const auto received = run_until(now() + 150 * PathRig::ms, [&] {
        const auto records = to.records();
        return std::ranges::find(records, record) != records.end();
      });
      if (received) return true;
    }
    return false;
  }

 private:
  platform::CryptoProvider& crypto_;
  platform::Sleeper& sleeper_;
};

template <class Provider>
class UdpTransport : public PathRig {
 protected:
  using Datagrams = test_support::kakusan::SecureDatagrams<Provider>;
  using World = PathWorld<Provider>;
  using Peer = PathPeer<Provider>;

  auto world(std::uint64_t seed) -> std::unique_ptr<World> {
    return std::make_unique<World>(crypto, sleeper, seed);
  }
};

TYPED_TEST_SUITE_P(UdpTransport);

TYPED_TEST_P(UdpTransport, RefusesTrafficBeforeItConnects) {
  auto world = this->world(1);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  std::array<std::byte, 32> early{};
  EXPECT_TRUE(
      this->fails_with(host.transport->send_record(this->pattern(10, 1)),
                       kakusan::Error::no_path));
  EXPECT_TRUE(
      this->fails_with(host.transport->send_datagram(this->media(10, 1)),
                       kakusan::Error::no_path));
  EXPECT_TRUE(this->fails_with(
      host.transport->export_secret("EXPORTER-test", {}, early),
      kakusan::Error::no_path));
  EXPECT_TRUE(this->fails_with(
      host.transport->connect(this->stand_in(), world->now()),
      kakusan::Error::internal));
}

TYPED_TEST_P(UdpTransport, EstablishesPublicPeersOnTheirOnlyCandidates) {
  auto world = this->world(1);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  EXPECT_EQ(host.local->ice.candidates().size(), 1U);
  EXPECT_EQ(host.endpoint().address, Addresses::ipv4(203, 0, 113, 10));
  ASSERT_FALSE(host.events.empty());
  EXPECT_TRUE(std::holds_alternative<transport_event::Gathered>(
      host.events.front().event));
  for (const auto* peer : {&host, &guest}) {
    EXPECT_EQ(peer->template count<transport_event::Gathered>(), 1U);
    EXPECT_EQ(peer->template count<transport_event::Nominated>(), 1U);
    EXPECT_EQ(peer->template count<transport_event::Established>(), 1U);
    EXPECT_FALSE(peer->failure());
    const auto* nominated = peer->template first<transport_event::Nominated>();
    const auto* established =
        peer->template first<transport_event::Established>();
    ASSERT_NE(nominated, nullptr);
    ASSERT_NE(established, nullptr);
    EXPECT_LE(nominated->at_us, established->at_us);
    const auto deadline = peer->transport->next_deadline_us();
    ASSERT_TRUE(deadline);
    EXPECT_GT(*deadline, world->now());
  }
  EXPECT_NE(host.local->certificate_digest, guest.local->certificate_digest);
  EXPECT_TRUE(
      this->fails_with(host.transport->connect(*guest.local, world->now()),
                       kakusan::Error::internal));
  EXPECT_TRUE(
      this->fails_with(host.transport->gather(world->now(), std::nullopt),
                       kakusan::Error::internal));
}

TYPED_TEST_P(UdpTransport, ExportsTheSameSecretOnBothSidesPerLabel) {
  auto world = this->world(1);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  const auto context = this->text("transcript");
  std::array<std::byte, 32> host_secret{};
  std::array<std::byte, 32> guest_secret{};
  std::array<std::byte, 32> other_secret{};
  EXPECT_TRUE(
      host.transport->export_secret("EXPORTER-test", context, host_secret));
  EXPECT_TRUE(
      guest.transport->export_secret("EXPORTER-test", context, guest_secret));
  EXPECT_TRUE(
      guest.transport->export_secret("EXPORTER-other", context, other_secret));
  EXPECT_EQ(host_secret, guest_secret);
  EXPECT_NE(host_secret, decltype(host_secret){});
  EXPECT_NE(other_secret, guest_secret);
}

TYPED_TEST_P(UdpTransport, CarriesRecordsAndMediaWithinTheirLimits) {
  auto world = this->world(1);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  const auto max_record = this->max_record;
  const std::vector<std::vector<std::byte>> host_records{
      this->pattern(1, 1), this->pattern(500, 2), this->pattern(max_record, 3)};
  for (const auto& record : host_records)
    EXPECT_TRUE(host.transport->send_record(record));
  EXPECT_TRUE(guest.transport->send_record(this->pattern(max_record, 4)));
  EXPECT_TRUE(this->fails_with(
      host.transport->send_record(this->pattern(max_record + 1, 5)),
      kakusan::Error::internal));
  EXPECT_TRUE(this->fails_with(host.transport->send_record({}),
                               kakusan::Error::internal));

  const std::vector<std::vector<std::byte>> host_media{
      this->media(4, 6), this->media(kakusan::max_datagram_size, 7),
      this->media(600, 8)};
  for (const auto& datagram : host_media)
    EXPECT_TRUE(host.transport->send_datagram(datagram));
  EXPECT_TRUE(guest.transport->send_datagram(this->media(100, 9)));
  EXPECT_TRUE(this->fails_with(host.transport->send_datagram(this->media(
                                   kakusan::max_datagram_size + 1, 10)),
                               kakusan::Error::internal));
  EXPECT_TRUE(
      this->fails_with(host.transport->send_datagram(this->pattern(100, 11)),
                       kakusan::Error::internal));
  EXPECT_TRUE(this->fails_with(host.transport->send_datagram({}),
                               kakusan::Error::internal));

  world->run_for(200 * this->ms);
  EXPECT_EQ(guest.records(), host_records);
  EXPECT_EQ(
      host.records(),
      (std::vector<std::vector<std::byte>>{this->pattern(max_record, 4)}));
  EXPECT_EQ(guest.datagrams(), host_media);
  EXPECT_EQ(host.datagrams(),
            (std::vector<std::vector<std::byte>>{this->media(100, 9)}));

  std::vector<std::vector<std::byte>> captured;
  world->network.network().tamper = [&](std::vector<std::byte>& bytes) {
    captured.push_back(bytes);
    bytes.clear();
  };
  const auto first = this->pattern(100, 12);
  const auto second_record = this->pattern(200, 13);
  EXPECT_TRUE(host.transport->send_record(first));
  EXPECT_TRUE(host.transport->send_record(second_record));
  world->network.network().tamper = nullptr;
  ASSERT_EQ(captured.size(), 2U);
  auto combined = captured[0];
  combined.insert(combined.end(), captured[1].begin(), captured[1].end());
  EXPECT_LE(combined.size(), kakusan::max_datagram_size);
  world->network.network().inject(host.endpoint(), guest.endpoint(), combined,
                                  world->now());
  world->run_for(200 * this->ms);
  const auto received = guest.records();
  ASSERT_EQ(received.size(), host_records.size() + 2);
  EXPECT_EQ(received[received.size() - 2], first);
  EXPECT_EQ(received.back(), second_record);
  EXPECT_FALSE(host.failure());
  EXPECT_FALSE(guest.failure());
}

TYPED_TEST_P(UdpTransport, MeetsThroughStunBehindPortRestrictedNats) {
  auto world = this->world(2);
  world->network.network().add_stun_server(this->stun_server(), true);
  auto& host = world->add_behind(
      kakusan::TransportRole::host, test_support::net::NatKind::port_restricted,
      Addresses::ipv4(203, 0, 113, 10), Addresses::ipv4(192, 168, 1, 10));
  auto& guest = world->add_behind(
      kakusan::TransportRole::guest, test_support::net::NatKind::port_restricted,
      Addresses::ipv4(198, 51, 100, 20), Addresses::ipv4(10, 0, 0, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(
      host, guest, libchiaking::net::nat::ice::connectivity_timeout_us));
  for (const auto* peer : {&host, &guest}) {
    EXPECT_TRUE(std::ranges::any_of(
        peer->local->ice.candidates(), [](const kakusan::Candidate& candidate) {
          return candidate.type == kakusan::CandidateType::server_reflexive;
        }));
  }
  EXPECT_TRUE(world->deliver(host, guest, this->pattern(300, 1)));
  EXPECT_TRUE(world->deliver(guest, host, this->pattern(301, 2)));
  EXPECT_TRUE(host.transport->send_datagram(this->media(1200, 3)));
  world->run_for(100 * this->ms);
  EXPECT_EQ(guest.datagrams(),
            (std::vector<std::vector<std::byte>>{this->media(1200, 3)}));
}

TYPED_TEST_P(UdpTransport, FailsAtTheHostDeadlineFromASymmetricNat) {
  auto world = this->world(3);
  world->network.network().add_stun_server(this->stun_server(), true);
  auto& host = world->add_behind(
      kakusan::TransportRole::host, test_support::net::NatKind::symmetric,
      Addresses::ipv4(203, 0, 113, 10), Addresses::ipv4(192, 168, 1, 10));
  auto& guest = world->add_behind(
      kakusan::TransportRole::guest, test_support::net::NatKind::port_restricted,
      Addresses::ipv4(198, 51, 100, 20), Addresses::ipv4(10, 0, 0, 20));
  ASSERT_NO_FATAL_FAILURE(world->connect(host, guest));
  const auto started = world->now();
  world->run_until(started +
                       libchiaking::net::nat::ice::connectivity_timeout_us +
                       this->second,
                   [&] { return host.failure().has_value(); });
  const auto* failed = host.template first<transport_event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->at_us,
            started + libchiaking::net::nat::ice::connectivity_timeout_us);
  EXPECT_EQ(host.failure(), kakusan::Error::no_path);
  EXPECT_EQ(host.template count<transport_event::Failed>(), 1U);
  EXPECT_EQ(host.template count<transport_event::Nominated>(), 0U);
  EXPECT_FALSE(guest.established());
  EXPECT_FALSE(guest.failure());
  EXPECT_FALSE(host.transport->next_deadline_us());
  EXPECT_EQ(host.machine.open_sockets(), 0U);
  EXPECT_TRUE(this->fails_with(host.transport->send_record(this->pattern(4, 1)),
                               kakusan::Error::no_path));
  world->run_for(5 * this->second);
  EXPECT_EQ(host.template count<transport_event::Failed>(), 1U);
}

TYPED_TEST_P(UdpTransport, FailsWithoutDeliveryOnAWrongCertificatePin) {
  auto world = this->world(4);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->gather(host));
  ASSERT_NO_FATAL_FAILURE(world->gather(guest));
  ASSERT_TRUE(host.local);
  ASSERT_TRUE(guest.local);
  auto wrong = guest.local->certificate_digest;
  wrong[0] ^= std::byte{1};
  EXPECT_TRUE(guest.transport->connect(*host.local, world->now()));
  auto remote = guest.local->clone();
  remote.certificate_digest = wrong;
  EXPECT_TRUE(host.transport->connect(remote, world->now()));
  world->run_until(world->now() + 20 * this->second, [&] {
    return host.failure().has_value() && guest.failure().has_value();
  });
  EXPECT_EQ(host.failure(), kakusan::Error::protocol);
  EXPECT_TRUE(guest.failure() == kakusan::Error::protocol ||
              guest.failure() == kakusan::Error::timeout)
      << ::testing::PrintToString(guest.failure());
  for (const auto* peer : {&host, &guest}) {
    EXPECT_EQ(peer->template count<transport_event::Nominated>(), 1U);
    EXPECT_FALSE(peer->established());
    EXPECT_TRUE(peer->records().empty());
    EXPECT_TRUE(peer->datagrams().empty());
    EXPECT_EQ(peer->template count<transport_event::Failed>(), 1U);
    EXPECT_FALSE(peer->transport->next_deadline_us());
  }
}

TYPED_TEST_P(UdpTransport, TimesOutTheSecurityHandshakeAfterNomination) {
  auto world = this->world(5);
  world->pace_handshakes = false;
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  world->network.network().tamper = [](std::vector<std::byte>& bytes) {
    if (!bytes.empty() && std::to_integer<unsigned>(bytes[0]) >= 20 &&
        std::to_integer<unsigned>(bytes[0]) <= 63)
      bytes.clear();
  };
  ASSERT_NO_FATAL_FAILURE(world->connect(host, guest));
  const auto started = world->now();
  world->run_until(started + 40 * this->second, [&] {
    return host.failure().has_value() && guest.failure().has_value();
  });
  EXPECT_EQ(host.failure(), kakusan::Error::timeout);
  EXPECT_EQ(guest.failure(), kakusan::Error::timeout);
  const auto* failed = host.template first<transport_event::Failed>();
  const auto* nominated = host.template first<transport_event::Nominated>();
  ASSERT_NE(failed, nullptr);
  ASSERT_NE(nominated, nullptr);
  EXPECT_EQ(failed->at_us,
            nominated->at_us + kakusan::security_handshake_timeout_us);
  EXPECT_FALSE(host.established());
  EXPECT_FALSE(guest.established());
}

TYPED_TEST_P(UdpTransport, ReportsNoPathWithoutAUsableInterface) {
  auto world = this->world(6);
  auto loopback =
      Addresses::interface_on("lo0", platform::IpAddress::loopback_ipv4());
  loopback.loopback = true;
  auto down = Addresses::interface_on("en1", Addresses::ipv4(192, 0, 2, 9));
  down.operational = false;
  std::vector<platform::InterfaceInfo> interfaces{
      loopback,
      Addresses::interface_on("en0", Addresses::ipv4(169, 254, 3, 4)),
      Addresses::interface_on(
          "en0", platform::IpAddress::ipv6(
                     {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})),
      Addresses::interface_on("en0", Addresses::ipv6(5)),
      Addresses::interface_on("en2", Addresses::ipv4(224, 0, 0, 9)),
      Addresses::interface_on("en3", platform::IpAddress::any_ipv4()),
      down};
  auto& peer = world->add(kakusan::TransportRole::host, interfaces);
  EXPECT_TRUE(peer.transport->gather(world->now(), std::nullopt));
  EXPECT_EQ(peer.transport->next_deadline_us(), world->now());
  world->pump(peer);
  EXPECT_EQ(peer.events.size(), 1U);
  EXPECT_EQ(peer.failure(), kakusan::Error::no_path);
  EXPECT_FALSE(peer.transport->next_deadline_us());
  EXPECT_EQ(peer.machine.open_sockets(), 0U);
  EXPECT_TRUE(this->fails_with(
      peer.transport->connect(this->stand_in(), world->now()),
      kakusan::Error::no_path));
  EXPECT_TRUE(this->fails_with(peer.transport->send_record(this->pattern(4, 1)),
                               kakusan::Error::no_path));
  EXPECT_TRUE(
      this->fails_with(peer.transport->gather(world->now(), std::nullopt),
                       kakusan::Error::internal));
  world->pump(peer);
  EXPECT_EQ(peer.events.size(), 1U);
}

TYPED_TEST_P(UdpTransport, ReportsNoPathWhenInterfacesCannotBeListed) {
  auto world = this->world(7);
  auto& peer = world->add_public(kakusan::TransportRole::guest,
                                 Addresses::ipv4(198, 51, 100, 20));
  peer.machine.fail_enumerate = true;
  EXPECT_TRUE(peer.transport->gather(world->now(), std::nullopt));
  world->pump(peer);
  EXPECT_EQ(peer.events.size(), 1U);
  EXPECT_EQ(peer.failure(), kakusan::Error::no_path);
}

TYPED_TEST_P(UdpTransport, ReportsNoPathWhenNoSocketBinds) {
  auto world = this->world(8);
  auto& peer = world->add_public(kakusan::TransportRole::guest,
                                 Addresses::ipv4(198, 51, 100, 20));
  peer.machine.fail_bind = true;
  EXPECT_TRUE(peer.transport->gather(world->now(), std::nullopt));
  world->pump(peer);
  EXPECT_EQ(peer.events.size(), 1U);
  EXPECT_EQ(peer.failure(), kakusan::Error::no_path);
  EXPECT_EQ(peer.machine.open_sockets(), 0U);
}

class MultiHomedMachine {
 public:
  static auto interfaces() -> std::vector<platform::InterfaceInfo> {
    auto vpn =
        Addresses::interface_on("utun0", Addresses::ipv4(198, 51, 100, 33));
    vpn.lan_adapter = false;
    auto down =
        Addresses::interface_on("en5", Addresses::ipv4(198, 51, 100, 34));
    down.operational = false;
    return {Addresses::interface_on("wl0", Addresses::ipv4(198, 51, 100, 30),
                                    platform::InterfaceMedium::wireless),
            Addresses::interface_on("en0", Addresses::ipv4(198, 51, 100, 31)),
            Addresses::interface_on("ppp0", Addresses::ipv4(198, 51, 100, 32),
                                    platform::InterfaceMedium::unclassified),
            vpn,
            Addresses::interface_on("en0", Addresses::ipv6(0x31)),
            Addresses::interface_on("wl1", Addresses::ipv4(198, 51, 100, 31),
                                    platform::InterfaceMedium::wireless),
            down};
  }

  static auto ordered(const kakusan::TransportDescription& description)
      -> std::vector<platform::IpAddress> {
    std::vector<kakusan::Candidate> candidates{
        description.ice.candidates().begin(),
        description.ice.candidates().end()};
    std::ranges::stable_sort(candidates, [](const kakusan::Candidate& left,
                                            const kakusan::Candidate& right) {
      return left.priority > right.priority;
    });
    std::vector<platform::IpAddress> addresses;
    for (const auto& candidate : candidates) {
      EXPECT_EQ(candidate.type, kakusan::CandidateType::host);
      addresses.push_back(candidate.endpoint.address);
    }
    return addresses;
  }
};

TYPED_TEST_P(UdpTransport, RanksTheInterfacesOfAMultiHomedMachine) {
  auto world = this->world(9);
  kakusan::UdpTransportConfig config;
  config.enable_ipv6 = true;
  auto& host =
      world->add(kakusan::TransportRole::host, MultiHomedMachine::interfaces(),
                 std::nullopt, config);
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(203, 0, 113, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  EXPECT_EQ(
      MultiHomedMachine::ordered(*host.local),
      (std::vector<platform::IpAddress>{
          Addresses::ipv4(198, 51, 100, 31), Addresses::ipv6(0x31),
          Addresses::ipv4(198, 51, 100, 30), Addresses::ipv4(198, 51, 100, 32),
          Addresses::ipv4(198, 51, 100, 33)}));
  EXPECT_EQ(host.machine.open_sockets(), 5U);
  EXPECT_TRUE(world->deliver(guest, host, this->pattern(64, 1)));
  EXPECT_TRUE(world->deliver(host, guest, this->pattern(64, 2)));
}

TYPED_TEST_P(UdpTransport, LeavesIpv6OutUnlessEnabled) {
  auto world = this->world(10);
  auto& host =
      world->add(kakusan::TransportRole::host, MultiHomedMachine::interfaces());
  ASSERT_NO_FATAL_FAILURE(world->gather(host));
  ASSERT_TRUE(host.local);
  EXPECT_EQ(
      MultiHomedMachine::ordered(*host.local),
      (std::vector<platform::IpAddress>{Addresses::ipv4(198, 51, 100, 31),
                                        Addresses::ipv4(198, 51, 100, 30),
                                        Addresses::ipv4(198, 51, 100, 32),
                                        Addresses::ipv4(198, 51, 100, 33)}));
}

TYPED_TEST_P(UdpTransport, GathersOnAtMostTheLocalAddressLimit) {
  auto world = this->world(11);
  std::vector<platform::InterfaceInfo> many;
  for (std::uint8_t index = 0; index < 12; ++index)
    many.push_back(Addresses::interface_on(
        "en" + std::to_string(index),
        Addresses::ipv4(198, 51, 100, static_cast<std::uint8_t>(100 + index))));
  auto& host = world->add(kakusan::TransportRole::host, many);
  ASSERT_NO_FATAL_FAILURE(world->gather(host));
  ASSERT_TRUE(host.local);
  EXPECT_EQ(host.local->ice.candidates().size(), kakusan::max_local_addresses);
  EXPECT_EQ(host.machine.open_sockets(), kakusan::max_local_addresses);
  const auto ordered = MultiHomedMachine::ordered(*host.local);
  ASSERT_FALSE(ordered.empty());
  EXPECT_EQ(ordered.front(), Addresses::ipv4(198, 51, 100, 100));
}

TYPED_TEST_P(UdpTransport, PlaysPingPongOverALossyNetwork) {
  auto world = this->world(12);
  world->network.network().set_loss_permille(100);
  world->network.network().set_latency(this->latency_us, 5 * this->ms);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(
      host, guest, libchiaking::net::nat::ice::connectivity_timeout_us));
  for (std::uint8_t round = 0; round < 20; ++round) {
    EXPECT_TRUE(world->deliver(host, guest, this->pattern(48, round)));
    EXPECT_TRUE(world->deliver(guest, host, this->pattern(49, round)));
  }
  EXPECT_FALSE(host.failure());
  EXPECT_FALSE(guest.failure());
}

TYPED_TEST_P(UdpTransport, FailsWithNoPathWhenConsentExpiresAfterACut) {
  auto world = this->world(13);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  world->run_for(60 * this->second);
  EXPECT_FALSE(host.failure());
  EXPECT_FALSE(guest.failure());

  const auto cut = world->now();
  const auto host_endpoint = host.endpoint();
  const auto guest_endpoint = guest.endpoint();
  world->network.network().set_cut(true);
  world->run_until(cut + 40 * this->second, [&] {
    return host.failure().has_value() && guest.failure().has_value();
  });
  std::array<std::byte, 16> secret{};
  for (auto* peer : {&host, &guest}) {
    EXPECT_EQ(peer->failure(), kakusan::Error::no_path);
    const auto* failed = peer->template first<transport_event::Failed>();
    ASSERT_NE(failed, nullptr);
    EXPECT_LE(failed->at_us,
              cut + libchiaking::net::nat::ice::consent_timeout_us);
    EXPECT_GT(failed->at_us,
              cut + libchiaking::net::nat::ice::consent_timeout_us -
                  libchiaking::net::nat::ice::consent_interval_min_us -
                  libchiaking::net::nat::ice::consent_interval_span_us -
                  2 * this->latency_us);
    EXPECT_TRUE(
        this->fails_with(peer->transport->send_record(this->pattern(8, 1)),
                         kakusan::Error::no_path));
    EXPECT_TRUE(
        this->fails_with(peer->transport->send_datagram(this->media(8, 1)),
                         kakusan::Error::no_path));
    EXPECT_TRUE(this->fails_with(
        peer->transport->export_secret("EXPORTER-test", {}, secret),
        kakusan::Error::no_path));
    EXPECT_FALSE(peer->transport->next_deadline_us());
    EXPECT_EQ(peer->machine.open_sockets(), 0U);
  }
  world->network.network().set_cut(false);
  const auto to_guest = world->network.network().packets_to(guest_endpoint);
  const auto to_host = world->network.network().packets_to(host_endpoint);
  world->run_for(10 * this->second);
  EXPECT_EQ(world->network.network().packets_to(guest_endpoint), to_guest);
  EXPECT_EQ(world->network.network().packets_to(host_endpoint), to_host);
  EXPECT_EQ(host.template count<transport_event::Failed>(), 1U);
}

TYPED_TEST_P(UdpTransport, IgnoresDatagramsOffTheNominatedPair) {
  auto world = this->world(14);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  const auto host_endpoint = host.endpoint();
  const auto guest_endpoint = guest.endpoint();
  const auto attacker = Addresses::at(Addresses::ipv4(192, 0, 2, 66), 7777);
  const auto neighbour =
      Addresses::at(guest_endpoint.address,
                    static_cast<std::uint16_t>(guest_endpoint.port + 1));
  std::vector<std::byte> fake_dtls{
      std::byte{0x17}, std::byte{0xfe}, std::byte{0xfd}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x09}, std::byte{0x00},
      std::byte{0x28}};
  const auto filler = this->pattern(40, 21);
  fake_dtls.insert(fake_dtls.end(), filler.begin(), filler.end());
  auto& network = world->network.network();
  for (const auto& source : {attacker, neighbour}) {
    network.inject(source, host_endpoint, fake_dtls, world->now());
    network.inject(source, host_endpoint, this->media(200, 22), world->now());
  }
  network.inject(guest_endpoint, host_endpoint,
                 this->media(kakusan::max_datagram_size + 1, 23), world->now());
  network.inject(guest_endpoint, host_endpoint, fake_dtls, world->now());
  world->run_for(500 * this->ms);
  EXPECT_TRUE(host.records().empty());
  EXPECT_TRUE(host.datagrams().empty());
  EXPECT_FALSE(host.failure());

  network.inject(guest_endpoint, host_endpoint, this->media(64, 24),
                 world->now());
  world->run_for(100 * this->ms);
  EXPECT_EQ(host.datagrams(),
            (std::vector<std::vector<std::byte>>{this->media(64, 24)}));
  EXPECT_TRUE(world->deliver(guest, host, this->pattern(90, 25)));
  EXPECT_EQ(host.records(),
            (std::vector<std::vector<std::byte>>{this->pattern(90, 25)}));
}

TYPED_TEST_P(UdpTransport, DropsMediaBeforeEstablishment) {
  auto world = this->world(15);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->connect(host, guest));
  auto& network = world->network.network();
  network.inject(guest.endpoint(), host.endpoint(), this->media(80, 1),
                 world->now());
  EXPECT_TRUE(world->run_until(world->now() + 10 * this->second, [&] {
    return host.template count<transport_event::Nominated>() != 0;
  }));
  EXPECT_FALSE(host.established());
  network.inject(guest.endpoint(), host.endpoint(), this->media(80, 2),
                 world->now());
  EXPECT_TRUE(world->run_until(world->now() + 10 * this->second, [&] {
    return host.established() && guest.established();
  }));
  world->run_for(this->second);
  EXPECT_TRUE(host.datagrams().empty());
  network.inject(guest.endpoint(), host.endpoint(), this->media(80, 3),
                 world->now());
  world->run_for(100 * this->ms);
  EXPECT_EQ(host.datagrams(),
            (std::vector<std::vector<std::byte>>{this->media(80, 3)}));
}

TYPED_TEST_P(UdpTransport, ClosesOnceAndTellsThePeer) {
  auto world = this->world(16);
  auto& host = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  auto& guest = world->add_public(kakusan::TransportRole::guest,
                                  Addresses::ipv4(198, 51, 100, 20));
  ASSERT_NO_FATAL_FAILURE(world->establish(host, guest, 10 * this->second));
  auto& network = world->network.network();
  const auto guest_endpoint = guest.endpoint();
  const auto before = network.packets_to(guest_endpoint);
  host.transport->close();
  EXPECT_EQ(network.packets_to(guest_endpoint), before + 1);
  host.transport->close();
  EXPECT_EQ(network.packets_to(guest_endpoint), before + 1);
  EXPECT_FALSE(host.transport->next_deadline_us());
  EXPECT_EQ(host.machine.open_sockets(), 0U);
  std::vector<kakusan::TransportEvent> events;
  EXPECT_TRUE(host.transport->pump(world->now(), events));
  EXPECT_TRUE(events.empty());
  std::array<std::byte, 16> secret{};
  EXPECT_TRUE(this->fails_with(host.transport->send_record(this->pattern(8, 1)),
                               kakusan::Error::no_path));
  EXPECT_TRUE(this->fails_with(host.transport->send_datagram(this->media(8, 1)),
                               kakusan::Error::no_path));
  EXPECT_TRUE(this->fails_with(
      host.transport->export_secret("EXPORTER-test", {}, secret),
      kakusan::Error::no_path));
  EXPECT_TRUE(
      this->fails_with(host.transport->gather(world->now(), std::nullopt),
                       kakusan::Error::internal));
  EXPECT_TRUE(
      this->fails_with(host.transport->connect(*guest.local, world->now()),
                       kakusan::Error::internal));

  const auto closed_at = world->now();
  world->run_until(closed_at + 2 * this->second,
                   [&] { return guest.failure().has_value(); });
  EXPECT_EQ(guest.failure(), kakusan::Error::no_path);
  const auto* failed = guest.template first<transport_event::Failed>();
  ASSERT_NE(failed, nullptr);
  EXPECT_LE(failed->at_us, closed_at + 2 * this->latency_us);
  EXPECT_EQ(guest.machine.open_sockets(), 0U);
}

TYPED_TEST_P(UdpTransport, ClosesAGatheredTransportOnce) {
  auto quiet = this->world(17);
  auto& lonely = quiet->add_public(kakusan::TransportRole::host,
                                   Addresses::ipv4(203, 0, 113, 10));
  ASSERT_NO_FATAL_FAILURE(quiet->gather(lonely));
  EXPECT_EQ(lonely.machine.open_sockets(), 1U);
  lonely.transport->close();
  lonely.transport->close();
  EXPECT_FALSE(lonely.transport->next_deadline_us());
  EXPECT_EQ(lonely.machine.open_sockets(), 0U);
}

TYPED_TEST_P(UdpTransport, FactoryWaitReportsReadabilityAndWakes) {
  auto world = this->world(18);
  auto& peer = world->add_public(kakusan::TransportRole::host,
                                 Addresses::ipv4(203, 0, 113, 10));
  ASSERT_NO_FATAL_FAILURE(world->gather(peer));
  EXPECT_EQ(peer.factory.wait(5 * this->ms), kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.machine.last_requests, 1U);
  EXPECT_EQ(peer.machine.last_timeout, std::chrono::milliseconds{5});
  world->network.network().inject(
      Addresses::at(Addresses::ipv4(192, 0, 2, 66), 7777), peer.endpoint(),
      this->pattern(30, 1), world->now());
  world->network.advance_to(world->now() + this->latency_us);
  EXPECT_EQ(peer.factory.wait(5 * this->ms), kakusan::WaitResult::readable);
  world->pump(peer);
  EXPECT_EQ(peer.factory.wait(5 * this->ms), kakusan::WaitResult::timed_out);
  peer.factory.wake();
  EXPECT_EQ(peer.factory.wait(5 * this->ms), kakusan::WaitResult::woken);
  EXPECT_EQ(peer.factory.wait(5 * this->ms), kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.factory.wait(kakusan::max_wait_us * 10),
            kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.machine.last_timeout,
            std::chrono::microseconds{kakusan::max_wait_us});

  auto second_transport = peer.factory.create(kakusan::TransportRole::guest);
  ASSERT_TRUE(second_transport);
  EXPECT_TRUE(second_transport.value()->gather(world->now(), std::nullopt));
  EXPECT_EQ(peer.factory.wait(0), kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.machine.last_requests, 2U);
  peer.transport.reset();
  EXPECT_EQ(peer.factory.wait(0), kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.machine.last_requests, 1U);
  second_transport.value().reset();
  EXPECT_EQ(peer.factory.wait(0), kakusan::WaitResult::timed_out);
  EXPECT_EQ(peer.machine.last_requests, 0U);
}

TYPED_TEST_P(UdpTransport, LetsATransportOutliveItsFactory) {
  auto world = this->world(18);
  test_support::net::SimulatedMachine machine{
      world->network,
      {Addresses::interface_on("en0", Addresses::ipv4(203, 0, 113, 50))}};
  test_support::SeededRandom entropy{99};
  auto factory = std::make_unique<kakusan::UdpTransportFactory>(
      kakusan::UdpTransportDependencies{machine, this->crypto, entropy,
                                        TestFixture::Datagrams::factory()},
      kakusan::UdpTransportConfig{});
  auto orphan = factory->create(kakusan::TransportRole::guest);
  ASSERT_TRUE(orphan);
  EXPECT_TRUE(orphan.value()->gather(world->now(), std::nullopt));
  factory.reset();
  std::vector<kakusan::TransportEvent> events;
  EXPECT_TRUE(orphan.value()->pump(world->now(), events));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<transport_event::Gathered>(events[0]));
  orphan.value()->close();
  EXPECT_EQ(machine.open_sockets(), 0U);
  orphan.value().reset();
}

TYPED_TEST_P(UdpTransport, FactoryWithoutAPollerCannotWait) {
  auto world = this->world(18);
  test_support::net::SimulatedMachine machine{
      world->network,
      {Addresses::interface_on("en0", Addresses::ipv4(203, 0, 113, 50))}};
  test_support::SeededRandom entropy{99};
  machine.fail_poller = true;
  kakusan::UdpTransportFactory blind{
      kakusan::UdpTransportDependencies{machine, this->crypto, entropy,
                                        TestFixture::Datagrams::factory()},
      kakusan::UdpTransportConfig{}};
  EXPECT_EQ(blind.wait(0), kakusan::WaitResult::unavailable);
  blind.wake();
}

TYPED_TEST_P(UdpTransport, FactoryRefusesTransportsWithoutDatagramSecurity) {
  auto world = this->world(18);
  test_support::net::SimulatedMachine machine{
      world->network,
      {Addresses::interface_on("en0", Addresses::ipv4(203, 0, 113, 50))}};
  test_support::SeededRandom entropy{99};
  kakusan::UdpTransportFactory insecure{
      kakusan::UdpTransportDependencies{machine, this->crypto, entropy, {}},
      kakusan::UdpTransportConfig{}};
  const auto refused = insecure.create(kakusan::TransportRole::host);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), kakusan::Error::internal);
}

TYPED_TEST_P(UdpTransport, FactoryWakeCrossesThreads) {
  auto& threading = test_support::process_threading();
  platform::DefaultNetworkProvider network{threading, this->steady()};
  test_support::SeededRandom entropy{7};
  kakusan::UdpTransportFactory factory{
      kakusan::UdpTransportDependencies{network, this->crypto, entropy,
                                        TestFixture::Datagrams::factory()},
      kakusan::UdpTransportConfig{}};
  EXPECT_EQ(factory.wait(1 * this->ms), kakusan::WaitResult::timed_out);
  const auto waking = [&] {
    platform::Sleeper pause{threading, this->steady()};
    pause.sleep_for(std::chrono::milliseconds{30});
    factory.wake();
  };
  auto started_thread = platform::Thread::start(
      threading, "waker", platform::ThreadRole::unspecified, waking);
  ASSERT_TRUE(started_thread);
  auto waker = std::move(started_thread).value();
  const auto started = this->steady().now_microseconds();
  const auto result = factory.wait(5 * this->second);
  const auto elapsed = std::chrono::microseconds{
      static_cast<std::int64_t>(this->steady().now_microseconds() - started)};
  static_cast<void>(waker.join());
  EXPECT_EQ(result, kakusan::WaitResult::woken);
  EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TYPED_TEST_P(UdpTransport, ResolvesStunServerNames) {
  using Endpoints = std::vector<platform::SocketEndpoint>;
  test_support::net::SimulatedNetwork network{this->crypto, 19};
  test_support::net::SimulatedMachine machine{network, {}};
  machine.add_host("stun.example.org", {Addresses::ipv4(192, 0, 2, 1),
                                        Addresses::ipv4(192, 0, 2, 2)});
  machine.add_host("mapped.example.org",
                   {platform::IpAddress::ipv6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                               0xff, 0xff, 192, 0, 2, 9}),
                    platform::IpAddress::any_ipv4()});
  const auto resolve = [&](std::vector<std::string> names) {
    return kakusan::UdpTransportFactory::resolve_stun_servers(machine.dns(),
                                                              names);
  };
  const auto at = [](std::uint8_t last, std::uint16_t port) {
    return Addresses::at(Addresses::ipv4(192, 0, 2, last), port);
  };
  EXPECT_EQ(resolve({"stun.example.org"}),
            (Endpoints{at(1, 3478), at(2, 3478)}));
  EXPECT_EQ(resolve({"stun.example.org:19302"}),
            (Endpoints{at(1, 19302), at(2, 19302)}));
  const auto queries = machine.dns_queries;
  EXPECT_EQ(resolve({"192.0.2.7"}), (Endpoints{at(7, 3478)}));
  EXPECT_EQ(resolve({"192.0.2.7:5349"}), (Endpoints{at(7, 5349)}));
  EXPECT_EQ(resolve({"[2001:db8::7]:3479"}),
            (Endpoints{Addresses::at(Addresses::ipv6(7), 3479)}));
  EXPECT_EQ(resolve({"[2001:db8::7]"}),
            (Endpoints{Addresses::at(Addresses::ipv6(7), 3478)}));
  EXPECT_EQ(resolve({"2001:db8::7"}),
            (Endpoints{Addresses::at(Addresses::ipv6(7), 3478)}));
  EXPECT_TRUE(resolve({"[192.0.2.7]", "[2001:db8::7", "[2001:db8::7]x", "[]:1",
                       "[nothex]", "2001:db8::zz"})
                  .empty());
  EXPECT_EQ(machine.dns_queries, queries);
  EXPECT_TRUE(resolve({"stun.example.org:", ":3478", "stun.example.org:0",
                       "stun.example.org:65536", "stun.example.org:12a",
                       "stun.example.org:123456", "", std::string(300, 'a')})
                  .empty());
  EXPECT_EQ(machine.dns_queries, queries);
  EXPECT_TRUE(resolve({"unknown.example.org"}).empty());
  EXPECT_EQ(machine.dns_queries, queries + 1);
  EXPECT_EQ(resolve({"mapped.example.org"}), (Endpoints{at(9, 3478)}));
  EXPECT_EQ(resolve({"192.0.2.7", "192.0.2.7:3478", "stun.example.org"}),
            (Endpoints{at(7, 3478), at(1, 3478), at(2, 3478)}));
  EXPECT_EQ(
      resolve({"stun.example.org", "192.0.2.7", "192.0.2.8", "192.0.2.9"}),
      (Endpoints{at(1, 3478), at(2, 3478), at(7, 3478), at(8, 3478)}));
}

REGISTER_TYPED_TEST_SUITE_P(
    UdpTransport, RefusesTrafficBeforeItConnects,
    EstablishesPublicPeersOnTheirOnlyCandidates,
    ExportsTheSameSecretOnBothSidesPerLabel,
    CarriesRecordsAndMediaWithinTheirLimits,
    MeetsThroughStunBehindPortRestrictedNats,
    FailsAtTheHostDeadlineFromASymmetricNat,
    FailsWithoutDeliveryOnAWrongCertificatePin,
    TimesOutTheSecurityHandshakeAfterNomination,
    ReportsNoPathWithoutAUsableInterface,
    ReportsNoPathWhenInterfacesCannotBeListed, ReportsNoPathWhenNoSocketBinds,
    RanksTheInterfacesOfAMultiHomedMachine, LeavesIpv6OutUnlessEnabled,
    GathersOnAtMostTheLocalAddressLimit, PlaysPingPongOverALossyNetwork,
    FailsWithNoPathWhenConsentExpiresAfterACut,
    IgnoresDatagramsOffTheNominatedPair, DropsMediaBeforeEstablishment,
    ClosesOnceAndTellsThePeer, ClosesAGatheredTransportOnce,
    FactoryWaitReportsReadabilityAndWakes, LetsATransportOutliveItsFactory,
    FactoryWithoutAPollerCannotWait,
    FactoryRefusesTransportsWithoutDatagramSecurity, FactoryWakeCrossesThreads,
    ResolvesStunServerNames);

}  // namespace
}  // namespace net_kakusan_test
