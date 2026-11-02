#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::SearchDatagram;
using libchiaking::consoles::SearchReply;
using libchiaking::platform::IpAddress;
using libchiaking::platform::SocketOption;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

static_assert(SearchDatagram::reply_limit == 4096);
static_assert(SearchDatagram::receive_limit == 4097);

class BeaconSocket : public ::testing::Test {
 protected:
  static constexpr std::string_view console_reply =
      "HTTP/1.1 200 OK\n"
      "host-request-port:9295\n"
      "host-type:PS5\n"
      "host-id:001122334455\n";
  static constexpr std::uint16_t free_port = 20000;

  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  SearchSink sink;
};

TEST_F(BeaconSocket, SendsBothSearchesAndDeliversTheReply) {
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  EXPECT_FALSE(beacon.start());
  ASSERT_TRUE(beacon.send_search());
  EXPECT_TRUE(SearchRig::contains(ps4.receive(), "00020020"));
  EXPECT_TRUE(SearchRig::contains(ps5.receive(), "00030010"));
  const auto destination = beacon.local_endpoint();
  ASSERT_TRUE(destination);
  ps5.send_to(destination.value().port, console_reply);
  for (std::size_t attempt = 0; attempt < 1000 && sink.hosts.empty(); ++attempt)
    ASSERT_TRUE(beacon.pump_once());
  ASSERT_EQ(sink.hosts.size(), 1U);
  EXPECT_EQ(sink.hosts.front().hardware_id().text(), "001122334455");
  EXPECT_EQ(sink.hosts.front().request_port(), 9295);
}

TEST_F(BeaconSocket, StopsTwiceAndSearchesNoMore) {
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  EXPECT_TRUE(beacon.stop());
  EXPECT_TRUE(beacon.stop());
  EXPECT_FALSE(beacon.send_search());
}

TEST_F(BeaconSocket, ReceivesOnlyWhileStarted) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  EXPECT_FALSE(beacon.receive_once());
  ASSERT_TRUE(beacon.start());
  const auto empty = beacon.receive_once();
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty.value());
  ASSERT_TRUE(beacon.stop());
  EXPECT_FALSE(beacon.receive_once());
}

TEST_F(BeaconSocket, CountsAMalformedDatagramAsProgress) {
  FakeConsole peer{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  ASSERT_TRUE(beacon.start());
  constexpr std::string_view malformed = "not a discovery response";
  for (const auto packet : {malformed, console_reply})
    peer.send_to(beacon.local_endpoint().value().port, packet);
  std::size_t consumed = 0;
  for (std::size_t attempt = 0; attempt < 100 && consumed < 2; ++attempt) {
    beacon.wait(std::chrono::milliseconds{10});
    const auto received = beacon.receive_once();
    ASSERT_TRUE(received);
    if (received.value()) ++consumed;
  }
  EXPECT_EQ(consumed, 2U);
  ASSERT_EQ(sink.hosts.size(), 1U);
  EXPECT_EQ(sink.hosts.front().hardware_id().text(), "001122334455");
  const auto empty = beacon.receive_once();
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty.value());
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconSocket, RefusesAnOversizedDatagramWhole) {
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::on_loopback()};
  ASSERT_TRUE(beacon.start());
  const auto destination = beacon.local_endpoint();
  ASSERT_TRUE(destination);
  auto created =
      network.sockets().create_udp_socket(IpAddress::loopback_ipv4());
  ASSERT_TRUE(created);
  ASSERT_TRUE(created.value()->bind({IpAddress::loopback_ipv4(), 0}));
  std::string response{console_reply};
  response +=
      "x-padding:" + std::string(SearchDatagram::reply_limit, 'A') + "\n";
  const auto bytes = std::as_bytes(std::span{response.data(), response.size()});
  const auto sent = created.value()->send_to(bytes, destination.value());
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent.value(), bytes.size());
  const auto received = beacon.receive_once();
  ASSERT_TRUE(received);
  EXPECT_TRUE(received.value());
  EXPECT_TRUE(sink.hosts.empty());
  std::string longest{console_reply};
  longest += "x-padding:";
  longest +=
      std::string(SearchDatagram::reply_limit - longest.size() - 1, 'A') + "\n";
  EXPECT_EQ(longest.size(), SearchDatagram::reply_limit);
  const auto fitting = std::as_bytes(std::span{longest.data(), longest.size()});
  ASSERT_TRUE(created.value()->send_to(fitting, destination.value()));
  ASSERT_TRUE(beacon.pump_once());
  EXPECT_EQ(sink.hosts.size(), 1U);
}

TEST_F(BeaconSocket, BindMovesPastAPortError) {
  ScriptedNetwork scripted;
  scripted.refuse_binds_through = free_port;
  auto settings = SearchRig::on_loopback();
  settings.first_bind_port = static_cast<std::uint16_t>(free_port - 1);
  settings.last_bind_port = static_cast<std::uint16_t>(free_port + 1);
  Beacon beacon(scripted, SearchRig::threading(), SearchRig::clock(), sink,
                settings);
  ASSERT_TRUE(beacon.start());
  EXPECT_EQ(scripted.bind_attempts,
            (std::vector<std::uint16_t>{
                static_cast<std::uint16_t>(free_port - 1), free_port,
                static_cast<std::uint16_t>(free_port + 1)}));
  EXPECT_EQ(scripted.sockets_created, 1U);
  const auto bound = beacon.local_endpoint();
  ASSERT_TRUE(bound);
  EXPECT_EQ(bound.value().port, free_port + 1);
}

TEST_F(BeaconSocket, BindFallsBackToAnyPortWhenTheRangeIsRefused) {
  ScriptedNetwork scripted;
  scripted.refuse_binds_through = 0xffff;
  auto settings = SearchRig::on_loopback();
  settings.first_bind_port = 9303;
  settings.last_bind_port = 9304;
  Beacon beacon(scripted, SearchRig::threading(), SearchRig::clock(), sink,
                settings);
  ASSERT_TRUE(beacon.start());
  EXPECT_EQ(scripted.bind_attempts,
            (std::vector<std::uint16_t>{9303, 9304, 0}));
  const auto bound = beacon.local_endpoint();
  ASSERT_TRUE(bound);
  EXPECT_NE(bound.value().port, 0);
}

TEST_F(BeaconSocket, BindSkipsPortsAlreadyTaken) {
  FakeConsole first{network, {IpAddress::loopback_ipv4(), 9303}};
  FakeConsole second{network, {IpAddress::loopback_ipv4(), 9304}};
  Beacon::Settings settings;
  settings.bind_address = IpAddress::loopback_ipv4();
  Beacon beacon(network, SearchRig::threading(), SearchRig::clock(), sink,
                settings);
  ASSERT_TRUE(beacon.start());
  const auto bound = beacon.local_endpoint();
  ASSERT_TRUE(bound);
  EXPECT_EQ(bound.value().port, 9305);
}

TEST_F(BeaconSocket, ARefusedBroadcastOptionIsNotFatal) {
  for (const bool ipv6 : {false, true}) {
    const auto bind =
        ipv6 ? IpAddress::loopback_ipv6() : IpAddress::loopback_ipv4();
    ScriptedNetwork scripted;
    scripted.refuse_broadcast = true;
    auto settings = SearchRig::on_loopback();
    settings.bind_address = bind;
    Beacon beacon(scripted, SearchRig::threading(), SearchRig::clock(), sink,
                  settings);
    ASSERT_TRUE(beacon.start()) << ipv6;
    EXPECT_EQ(scripted.options,
              (std::vector<SocketOption>{SocketOption::broadcast}))
        << ipv6;
    EXPECT_TRUE(beacon.local_endpoint()) << ipv6;
    EXPECT_TRUE(beacon.pump_once()) << ipv6;
    EXPECT_TRUE(beacon.stop()) << ipv6;
  }
}

TEST_F(BeaconSocket, AWaitEndsOnAWake) {
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  EXPECT_TRUE(SearchRig::wake_interrupts_wait(beacon));
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconSocket, AnIdleWaitLastsItsTimeout) {
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  const auto idle = SearchRig::now();
  beacon.wait(std::chrono::milliseconds{60});
  EXPECT_GE(SearchRig::now() - idle, std::chrono::milliseconds{50});
  EXPECT_TRUE(beacon.stop());
}

TEST_F(BeaconSocket, AWaitEndsWhenAReplyArrives) {
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
  ASSERT_TRUE(beacon.start());
  ASSERT_TRUE(beacon.send_search());
  static_cast<void>(ps4.receive());
  static_cast<void>(ps5.receive());
  auto reply = libchiaking::platform::Thread::start(
                   SearchRig::threading(), "test-reply",
                   libchiaking::platform::ThreadRole::unspecified,
                   [&ps5] {
                     SearchRig::pause(std::chrono::milliseconds{40});
                     ps5.reply(
                         "HTTP/1.1 200 "
                         "Ok\nhost-id:001122334455\nhost-type:PS5\nhost-"
                         "request-port:997\n");
                   })
                   .value();
  const auto started = SearchRig::now();
  beacon.wait(std::chrono::milliseconds{5000});
  const auto elapsed = SearchRig::now() - started;
  static_cast<void>(reply.join());
  EXPECT_TRUE(SearchRig::within_wake_window(elapsed));
  EXPECT_TRUE(sink.hosts.empty());
  ASSERT_TRUE(beacon.pump_once());
  ASSERT_EQ(sink.hosts.size(), 1U);
  EXPECT_EQ(sink.hosts.front().hardware_id().text(), "001122334455");
  EXPECT_EQ(sink.hosts.front().request_port(), 997);
  const auto ready = SearchRig::now();
  beacon.wait(std::chrono::milliseconds{0});
  EXPECT_LT(SearchRig::now() - ready, std::chrono::seconds{1});
  EXPECT_TRUE(beacon.stop());
}

}  // namespace
}  // namespace consoles_test
