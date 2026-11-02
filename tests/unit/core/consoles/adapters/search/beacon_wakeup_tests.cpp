#include "src/core/consoles/model/console/wake_credential.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::Generation;
using libchiaking::consoles::WakeCredential;
using libchiaking::platform::IpAddress;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

class BeaconWakeup : public ::testing::Test {
 protected:
  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  SearchSink sink;
};

TEST_F(BeaconWakeup, SpeaksTheDialectOfTheChosenConsole) {
  FakeConsole peer{network};
  auto settings = SearchRig::on_loopback();
  settings.ps5_destination_port = peer.port();
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                settings};
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_wakeup(IpAddress::loopback_ipv4(), Generation::ps5,
                                 WakeCredential{123456}));
  const auto packet = peer.receive();
  EXPECT_TRUE(SearchRig::starts_with(packet, "WAKEUP * HTTP/1.1"));
  EXPECT_TRUE(SearchRig::contains(packet, "user-credential:123456"));
  EXPECT_TRUE(SearchRig::contains(packet, "00030010"));
}

TEST_F(BeaconWakeup, IsRefusedBeforeTheBeaconStarts) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  const auto unstarted =
      beacon.send_wakeup("127.0.0.1", Generation::ps5, WakeCredential{1});
  ASSERT_FALSE(unstarted);
  EXPECT_EQ(unstarted.error(), Beacon::Failure::invalid_state);
}

TEST_F(BeaconWakeup, ReachesAConsoleByHostName) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_wakeup("127.0.0.1", Generation::ps5,
                                 WakeCredential{0x3a5f9c1e}));
  const auto wakeup = ps5.receive();
  EXPECT_TRUE(SearchRig::starts_with(wakeup, "WAKEUP * HTTP/1.1"));
  EXPECT_TRUE(SearchRig::contains(wakeup, "user-credential:979344414"));
  ASSERT_TRUE(
      beacon.send_wakeup("localhost", Generation::ps4, WakeCredential{7}));
  EXPECT_TRUE(SearchRig::contains(ps4.receive(), "user-credential:7"));
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconWakeup, AnUnresolvableHostNameIsANetworkFailure) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  const auto unresolved = beacon.send_wakeup(
      "unresolvable.invalid", Generation::ps5, WakeCredential{1});
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Beacon::Failure::network);
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconWakeup, AnInvalidHostNameIsAnInvalidArgument) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  for (const auto& invalid :
       {std::string{}, std::string(256, 'a'), std::string{"a\0b", 3}}) {
    const auto refused =
        beacon.send_wakeup(invalid, Generation::ps5, WakeCredential{1});
    ASSERT_FALSE(refused) << invalid.size();
    EXPECT_EQ(refused.error(), Beacon::Failure::invalid_argument)
        << invalid.size();
  }
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconWakeup, AOneShotWakeReachesAnAddress) {
  ASSERT_TRUE(Beacon::wake_host(network, SearchRig::threading(),
                                SearchRig::clock(), "127.0.0.1", ps4.port(),
                                Generation::ps4, WakeCredential{42}));
  const auto one_shot = ps4.receive();
  EXPECT_TRUE(SearchRig::starts_with(one_shot, "WAKEUP * HTTP/1.1"));
  EXPECT_TRUE(SearchRig::contains(one_shot, "00020020"));
}

TEST_F(BeaconWakeup, AOneShotWakeToAnUnresolvableNameIsANetworkFailure) {
  const auto unresolved = Beacon::wake_host(
      network, SearchRig::threading(), SearchRig::clock(),
      "unresolvable.invalid", 0, Generation::ps4, WakeCredential{42});
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Beacon::Failure::network);
}

TEST_F(BeaconWakeup, AOneShotWakeReachesAHostName) {
  ASSERT_TRUE(Beacon::wake_host(network, SearchRig::threading(),
                                SearchRig::clock(), "localhost", ps4.port(),
                                Generation::ps4, WakeCredential{43}));
  const auto named = ps4.receive();
  EXPECT_TRUE(SearchRig::starts_with(named, "WAKEUP * HTTP/1.1"));
  EXPECT_TRUE(SearchRig::contains(named, "user-credential:43"));
}

TEST_F(BeaconWakeup, AOneShotWakeToAnEmptyNameIsAnInvalidArgument) {
  const auto empty =
      Beacon::wake_host(network, SearchRig::threading(), SearchRig::clock(), "",
                        ps4.port(), Generation::ps4, WakeCredential{43});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), Beacon::Failure::invalid_argument);
}

}  // namespace
}  // namespace consoles_test
