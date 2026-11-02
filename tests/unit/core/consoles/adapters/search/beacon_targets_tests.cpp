#include "src/core/consoles/model/console/wake_credential.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::Generation;
using libchiaking::consoles::WakeCredential;
using libchiaking::platform::InterfaceInfo;
using libchiaking::platform::IpAddress;
using libchiaking::platform::NetError;
using libchiaking::platform::SocketEndpoint;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;
using Addresses = std::vector<IpAddress>;

static_assert(Beacon::ipv4_limited_broadcast ==
              IpAddress::ipv4({255, 255, 255, 255}));
static_assert(Beacon::host_name_limit == 255);
static_assert(Beacon::target_limit == 64);

class BeaconInterfaceBroadcasts : public ::testing::Test {
 protected:
  static auto interface(std::string name, std::uint32_t index,
                        IpAddress address, IpAddress broadcast,
                        bool has_broadcast, bool is_loopback) -> InterfaceInfo {
    InterfaceInfo info;
    info.name = std::move(name);
    info.index = index;
    info.address = address;
    info.broadcast = broadcast;
    info.has_broadcast = has_broadcast;
    info.loopback = is_loopback;
    return info;
  }

  BeaconInterfaceBroadcasts() {
    network.memory.interface_list = {
        interface("eth0", 1, SearchRig::ip(192, 168, 1, 10), local, true,
                  false),
        interface("lo", 2, IpAddress::loopback_ipv4(),
                  SearchRig::ip(127, 255, 255, 255), true, true),
        interface("eth1", 3, SearchRig::ip(10, 0, 0, 10),
                  SearchRig::ip(10, 0, 0, 255), false, false),
        interface("bad6", 4, IpAddress::loopback_ipv6(), local, true, false),
        interface("bad-broadcast", 5, SearchRig::ip(10, 1, 0, 10),
                  IpAddress::loopback_ipv6(), true, false),
        interface("unspecified", 6, SearchRig::ip(10, 2, 0, 10),
                  IpAddress::any_ipv4(), true, false),
    };
  }

  auto searched(Beacon::Settings chosen)
      -> std::pair<libchiaking::Expected<void, Beacon::Failure>, Addresses> {
    SearchSink sink;
    chosen.first_bind_port = 0;
    chosen.last_bind_port = 0;
    Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                  std::move(chosen)};
    const auto started = beacon.start();
    EXPECT_TRUE(started);
    network.sent.clear();
    const auto sent = beacon.send_search();
    return std::pair{sent, SearchRig::destinations(network.sent)};
  }

  const IpAddress global = SearchRig::ip(255, 255, 255, 255);
  const IpAddress local = SearchRig::ip(192, 168, 1, 255);
  ScriptedNetwork network;
  Beacon::Settings settings;
};

TEST_F(BeaconInterfaceBroadcasts, GoToTheGlobalAndEachUsableInterface) {
  const auto selected = searched(settings);
  EXPECT_TRUE(selected.first);
  EXPECT_EQ(selected.second, (Addresses{global, local}));
  EXPECT_EQ(network.interface_calls, 1U);
}

TEST_F(BeaconInterfaceBroadcasts, AreReplacedByExplicitTargets) {
  static_cast<void>(searched(settings));
  settings.targets = {SearchRig::ip(203, 0, 113, 5)};
  const auto explicit_targets = searched(settings);
  EXPECT_TRUE(explicit_targets.first);
  EXPECT_EQ(explicit_targets.second, settings.targets);
  EXPECT_EQ(network.interface_calls, 1U);
}

TEST_F(BeaconInterfaceBroadcasts, CanBeLeftOutForTheGlobalBroadcastAlone) {
  static_cast<void>(searched(settings));
  settings.include_interface_broadcasts = false;
  const auto global_only = searched(settings);
  EXPECT_TRUE(global_only.first);
  EXPECT_EQ(global_only.second, (Addresses{global}));
  EXPECT_EQ(network.interface_calls, 1U);
}

TEST_F(BeaconInterfaceBroadcasts, BecomeAllNodesOnIpv6) {
  static_cast<void>(searched(settings));
  settings.include_interface_broadcasts = false;
  settings.bind_address = IpAddress::loopback_ipv6();
  const auto ipv6_auto = searched(settings);
  const auto all_nodes =
      IpAddress::ipv6({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  EXPECT_TRUE(ipv6_auto.first);
  EXPECT_EQ(ipv6_auto.second, (Addresses{all_nodes}));
  EXPECT_EQ(network.interface_calls, 1U);
}

TEST_F(BeaconInterfaceBroadcasts, FailTheSearchWhenInterfacesCannotBeListed) {
  static_cast<void>(searched(settings));
  settings.bind_address = IpAddress::any_ipv4();
  settings.include_interface_broadcasts = true;
  network.interfaces_fail = true;
  const auto failed = searched(settings);
  ASSERT_FALSE(failed.first);
  EXPECT_EQ(failed.first.error(), Beacon::Failure::network);
  EXPECT_TRUE(failed.second.empty());
  EXPECT_EQ(network.interface_calls, 2U);
}

class BeaconTargets : public ::testing::Test {
 protected:
  ScriptedNetwork network;
  SearchSink sink;
};

TEST_F(BeaconTargets, ADirectedSearchReusesTheSocketAndIgnoresTheTargets) {
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  auto settings = SearchRig::on_loopback();
  settings.targets = {SearchRig::ip(192, 0, 2, 10)};
  settings.ps4_destination_port = ps4.port();
  settings.ps5_destination_port = ps5.port();
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                settings};
  const auto early = beacon.send_search_to(IpAddress::loopback_ipv4());
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), Beacon::ProbeFailure::not_started);
  ASSERT_TRUE(beacon.start());
  const auto bound = beacon.local_endpoint().value();
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    ASSERT_TRUE(beacon.send_search_to(IpAddress::loopback_ipv4())) << attempt;
    EXPECT_TRUE(SearchRig::contains(ps4.receive(), "00020020")) << attempt;
    EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010")) << attempt;
    const auto current = beacon.local_endpoint().value();
    EXPECT_EQ(current.address, bound.address) << attempt;
    EXPECT_EQ(current.port, bound.port) << attempt;
  }
  EXPECT_EQ(network.sockets_created, 1U);
  EXPECT_EQ(SearchRig::destinations(network.sent),
            (Addresses{IpAddress::loopback_ipv4()}));
  const auto wrong_family = beacon.send_search_to(IpAddress::loopback_ipv6());
  ASSERT_FALSE(wrong_family);
  EXPECT_EQ(wrong_family.error(),
            Beacon::ProbeFailure::address_family_mismatch);
  ASSERT_TRUE(beacon.stop());
  const auto stopped = beacon.send_search_to(IpAddress::loopback_ipv4());
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error(), Beacon::ProbeFailure::not_started);
}

TEST_F(BeaconTargets, ASearchContinuesPastAFailedSend) {
  network.unreachable = [](const SocketEndpoint& destination) {
    return destination.address == IpAddress::ipv4({192, 0, 2, 1});
  };
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  auto settings = SearchRig::loopback(ps4, ps5);
  settings.targets = {SearchRig::ip(192, 0, 2, 1), IpAddress::loopback_ipv4()};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                settings};
  ASSERT_TRUE(beacon.start());
  EXPECT_TRUE(beacon.send_search());
  EXPECT_TRUE(SearchRig::contains(ps4.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010"));
}

TEST_F(BeaconTargets, ASearchFailsWhenEveryTargetIsUnreachable) {
  network.unreachable = [](const SocketEndpoint& destination) {
    return destination.address == IpAddress::ipv4({192, 0, 2, 1});
  };
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  auto unreachable = SearchRig::loopback(ps4, ps5);
  unreachable.targets = {SearchRig::ip(192, 0, 2, 1)};
  Beacon failing{network, SearchRig::threading(), SearchRig::clock(), sink,
                 unreachable};
  ASSERT_TRUE(failing.start());
  const auto failed = failing.send_search();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), Beacon::Failure::network);
}

TEST_F(BeaconTargets, RefusedSendsFollowTheReferenceResults) {
  const auto unreachable_v6 = IpAddress::ipv6(
      {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
  for (const bool ipv6 : {false, true}) {
    ScriptedNetwork refusing;
    refusing.unreachable = [](const SocketEndpoint&) { return true; };
    auto settings = SearchRig::on_loopback();
    settings.bind_address =
        ipv6 ? IpAddress::loopback_ipv6() : IpAddress::loopback_ipv4();
    settings.include_interface_broadcasts = false;
    settings.targets = {ipv6 ? unreachable_v6 : SearchRig::ip(192, 0, 2, 1)};
    Beacon beacon(refusing, SearchRig::threading(), SearchRig::clock(), sink,
                  settings);
    ASSERT_TRUE(beacon.start()) << ipv6;
    const auto searched = beacon.send_search();
    const auto woken = beacon.send_wakeup(settings.targets.front(),
                                          Generation::ps5, WakeCredential{1});
    EXPECT_EQ(refusing.refused_sends.size(), 3U) << ipv6;
    if (ipv6) {
      EXPECT_TRUE(searched);
      EXPECT_TRUE(woken);
    } else {
      ASSERT_FALSE(searched);
      EXPECT_EQ(searched.error(), Beacon::Failure::network);
      ASSERT_FALSE(woken);
      EXPECT_EQ(woken.error(), Beacon::Failure::network);
    }
  }
}

TEST_F(BeaconTargets, AHostNameIsResolvedOnEachSearchUntilFound) {
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  network.dns_failures = 1;
  network.dns_answer =
      Addresses{SearchRig::ip(192, 0, 2, 7), IpAddress::loopback_ipv6(),
                IpAddress::loopback_ipv4()};
  auto settings = SearchRig::on_loopback();
  settings.target_hosts = {"console.example.test"};
  settings.ps4_destination_port = ps4.port();
  settings.ps5_destination_port = ps5.port();
  Beacon beacon(network, SearchRig::threading(), SearchRig::clock(), sink,
                settings);
  ASSERT_TRUE(beacon.start());
  const auto unresolved = beacon.send_search();
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Beacon::Failure::network);
  ASSERT_EQ(network.dns_queries.size(), 1U);
  EXPECT_EQ(network.dns_queries.front(), "console.example.test");
  EXPECT_FALSE(ps4.try_receive());
  ASSERT_TRUE(beacon.send_search());
  EXPECT_EQ(network.dns_queries.size(), 2U);
  EXPECT_TRUE(SearchRig::contains(ps4.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010"));
  ASSERT_TRUE(beacon.send_search());
  EXPECT_EQ(network.dns_queries.size(), 2U);
  EXPECT_TRUE(SearchRig::contains(ps4.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010"));
  EXPECT_EQ(network.interface_calls, 0U);
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, AHostNameOfTheWrongFamilyIsResolvedAgainAndNeverSent) {
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  auto settings = SearchRig::on_loopback();
  settings.target_hosts = {"console.example.test"};
  settings.ps4_destination_port = ps4.port();
  settings.ps5_destination_port = ps5.port();
  ScriptedNetwork other;
  other.dns_answer = Addresses{IpAddress::loopback_ipv6()};
  other.unreachable = [](const SocketEndpoint&) { return true; };
  Beacon beacon(other, SearchRig::threading(), SearchRig::clock(), sink,
                settings);
  ASSERT_TRUE(beacon.start());
  const auto searched = beacon.send_search();
  ASSERT_FALSE(searched);
  EXPECT_EQ(searched.error(), Beacon::Failure::network);
  EXPECT_EQ(beacon.send_search().error(), Beacon::Failure::network);
  EXPECT_EQ(other.dns_queries.size(), 2U);
  EXPECT_TRUE(other.refused_sends.empty());
  EXPECT_TRUE(other.sent.empty());
}

TEST_F(BeaconTargets, ADirectedProbeReportsEachDestination) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  const auto target = SearchRig::ip(198, 51, 100, 7);
  const auto early = beacon.send_search_to(target);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), Beacon::ProbeFailure::not_started);
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_search_to(target));
  ASSERT_EQ(sink.probes.size(), 2U);
  EXPECT_EQ(sink.probes[0].first, (SocketEndpoint{target, Beacon::ps4_port}));
  EXPECT_FALSE(sink.probes[0].second);
  EXPECT_EQ(sink.probes[1].first, (SocketEndpoint{target, Beacon::ps5_port}));
  EXPECT_FALSE(sink.probes[1].second);
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, ADirectedProbeReportsPermissionDenial) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  const auto target = SearchRig::ip(198, 51, 100, 7);
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_search_to(target));
  network.send_error = NetError::permission_denied;
  const auto denied = beacon.send_search_to(target);
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error(), Beacon::ProbeFailure::permission_denied);
  ASSERT_EQ(sink.probes.size(), 3U);
  EXPECT_EQ(sink.probes[2].second, Beacon::ProbeFailure::permission_denied);
  const auto searched = beacon.send_search();
  ASSERT_FALSE(searched);
  EXPECT_EQ(searched.error(), Beacon::Failure::network);
  network.send_error = NetError::network_unreachable;
  const auto unreachable = beacon.send_search_to(target);
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error(), Beacon::ProbeFailure::network);
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, ADirectedProbeStopsAtADenialPartWay) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  const auto target = SearchRig::ip(198, 51, 100, 7);
  ASSERT_TRUE(beacon.start());
  network.on_send = [&] {
    network.on_send = [&] { network.send_error = NetError::permission_denied; };
  };
  const auto second_denied = beacon.send_search_to(target);
  ASSERT_FALSE(second_denied);
  EXPECT_EQ(second_denied.error(), Beacon::ProbeFailure::permission_denied);
  EXPECT_EQ(network.sent.size(), 2U);
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, ANetworkThatCannotBroadcastGetsOnlyUnicast) {
  network.unicast_only = true;
  Beacon::Settings settings;
  settings.first_bind_port = 0;
  settings.last_bind_port = 0;
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                settings};
  ASSERT_TRUE(beacon.start());
  EXPECT_TRUE(network.options.empty());
  ASSERT_TRUE(beacon.send_search());
  EXPECT_TRUE(network.sent.empty());
  EXPECT_EQ(network.interface_calls, 0U);
  ASSERT_TRUE(beacon.send_search_to(SearchRig::ip(203, 0, 113, 5)));
  EXPECT_EQ(SearchRig::destinations(network.sent),
            (Addresses{SearchRig::ip(203, 0, 113, 5)}));
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, ANetworkThatCannotBroadcastStillSearchesItsTargets) {
  network.unicast_only = true;
  Beacon::Settings settings;
  settings.first_bind_port = 0;
  settings.last_bind_port = 0;
  settings.targets = {SearchRig::ip(198, 51, 100, 7)};
  Beacon targeted{network, SearchRig::threading(), SearchRig::clock(), sink,
                  settings};
  ASSERT_TRUE(targeted.start());
  ASSERT_TRUE(targeted.send_search());
  EXPECT_EQ(SearchRig::destinations(network.sent),
            (Addresses{SearchRig::ip(198, 51, 100, 7)}));
  EXPECT_TRUE(network.options.empty());
  EXPECT_TRUE(targeted.stop());
}

TEST_F(BeaconTargets, AHostNameTargetIsResolved) {
  InMemoryNetwork memory{SearchRig::threading(), SearchRig::clock()};
  FakeConsole ps4{memory};
  FakeConsole ps5{memory};
  auto settings = SearchRig::loopback(ps4, ps5);
  settings.targets.clear();
  settings.target_hosts = {"localhost"};
  Beacon beacon{memory, SearchRig::threading(), SearchRig::clock(), sink,
                settings};
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_search());
  EXPECT_TRUE(SearchRig::starts_with(ps4.receive(), "SRCH * HTTP/1.1"));
  EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010"));
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconTargets, AnUnresolvableHostNameTargetFailsTheSearch) {
  InMemoryNetwork memory{SearchRig::threading(), SearchRig::clock()};
  FakeConsole ps4{memory};
  FakeConsole ps5{memory};
  auto unresolvable = SearchRig::loopback(ps4, ps5);
  unresolvable.targets.clear();
  unresolvable.target_hosts = {"libchiaki-test.invalid"};
  Beacon lost{memory, SearchRig::threading(), SearchRig::clock(), sink,
              unresolvable};
  ASSERT_TRUE(lost.start());
  const auto searched = lost.send_search();
  ASSERT_FALSE(searched);
  EXPECT_EQ(searched.error(), Beacon::Failure::network);
}

class BeaconTargetLimits : public ::testing::Test {
 protected:
  auto refused(const Beacon::Settings& settings) -> testing::AssertionResult {
    if (Beacon::accepts(settings))
      return testing::AssertionFailure() << "the settings were accepted";
    Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                  settings};
    const auto started = beacon.start();
    if (started) return testing::AssertionFailure() << "the beacon started";
    if (started.error() != Beacon::Failure::invalid_argument)
      return testing::AssertionFailure()
             << "the start failed with "
             << testing::PrintToString(started.error());
    if (beacon.local_endpoint())
      return testing::AssertionFailure() << "a socket was bound";
    if (!network.dns_queries.empty())
      return testing::AssertionFailure()
             << network.dns_queries.size() << " names were resolved";
    if (network.sockets_created != 0)
      return testing::AssertionFailure()
             << network.sockets_created << " sockets were created";
    return testing::AssertionSuccess();
  }

  auto settings() const -> Beacon::Settings {
    return SearchRig::loopback(ps4, ps5);
  }

  ScriptedNetwork network;
  FakeConsole ps4{network.memory};
  FakeConsole ps5{network.memory};
  SearchSink sink;
};

TEST_F(BeaconTargetLimits, RefuseAHostNameOverTheLimit) {
  auto lengthy = settings();
  lengthy.target_hosts = {std::string(256, 'a')};
  EXPECT_TRUE(refused(lengthy));
}

TEST_F(BeaconTargetLimits, RefuseAnEmptyHostName) {
  auto empty = settings();
  empty.target_hosts = {std::string{}};
  EXPECT_TRUE(refused(empty));
}

TEST_F(BeaconTargetLimits, RefuseAHostNameWithAnEmbeddedNul) {
  auto embedded = settings();
  embedded.target_hosts = {std::string{"local\0host", 10}};
  EXPECT_TRUE(refused(embedded));
}

TEST_F(BeaconTargetLimits, CountAddressesAndHostNamesTogether) {
  auto crowded = settings();
  crowded.targets.assign(60, IpAddress::loopback_ipv4());
  crowded.target_hosts.assign(5, "localhost");
  EXPECT_TRUE(refused(crowded));
}

TEST_F(BeaconTargetLimits, RefuseTooManyAddresses) {
  auto flooded = settings();
  flooded.targets.assign(65, IpAddress::loopback_ipv4());
  EXPECT_TRUE(refused(flooded));
}

TEST_F(BeaconTargetLimits, AcceptAFullBudget) {
  auto full = settings();
  full.targets.assign(32, IpAddress::loopback_ipv4());
  full.target_hosts.assign(32, std::string(255, 'a'));
  EXPECT_TRUE(Beacon::accepts(full));
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                full};
  EXPECT_TRUE(beacon.start());
  EXPECT_TRUE(beacon.stop());
}

}  // namespace
}  // namespace consoles_test
