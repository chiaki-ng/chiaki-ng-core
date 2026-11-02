#include "src/core/consoles/adapters/pairing/pairing_knock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace consoles_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::consoles::Generation;
using libchiaking::consoles::PairingKnock;
using libchiaking::testing::InMemoryNetwork;

class PairingKnockDoor : public ::testing::Test {
 protected:
  using Heard =
      std::pair<std::array<std::byte, 256>, platform::UdpReceiveResult>;

  auto now() const -> std::chrono::microseconds {
    return std::chrono::microseconds{steady.now_microseconds()};
  }

  void pause(std::chrono::nanoseconds duration) {
    platform::Sleeper{test_support::process_threading(), steady}.sleep_for(
        duration);
  }

  auto peer() -> std::unique_ptr<platform::UdpSocket> {
    auto created = network.sockets()
                       .create_udp_socket(platform::IpAddress::loopback_ipv4())
                       .value();
    EXPECT_TRUE(created->bind({platform::IpAddress::loopback_ipv4(), 0}));
    return created;
  }

  auto receive(platform::UdpSocket& socket) -> Heard {
    auto poller = network.pollers().create_poller().value();
    const std::array<platform::PollRequest, 1> requests{
        {{&socket, 1, platform::PollEvent::readable}}};
    const auto ready =
        poller->wait(requests, std::chrono::seconds{1}, platform::StopToken{});
    EXPECT_TRUE(ready);
    if (ready) {
      EXPECT_FALSE(ready.value().ready.empty());
    }
    std::array<std::byte, 256> buffer{};
    const auto received = socket.receive_from(buffer).value();
    return {buffer, received};
  }

  static void send(platform::UdpSocket& socket,
                   platform::SocketEndpoint destination,
                   std::string_view text) {
    const auto bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(text.data()), text.size()};
    const auto sent = socket.send_to(bytes, destination);
    ASSERT_TRUE(sent);
    EXPECT_EQ(sent.value(), bytes.size());
  }

  auto wait_for_match() -> std::optional<platform::SocketEndpoint> {
    const auto deadline = now() + std::chrono::seconds{2};
    while (now() < deadline) {
      auto result = client.pump_once();
      EXPECT_TRUE(result);
      if (!result) return std::nullopt;
      if (result.value()) return result.value();
      pause(std::chrono::milliseconds{1});
    }
    return std::nullopt;
  }

  platform::SteadyClock steady;
  InMemoryNetwork network{test_support::process_threading(), steady};
  libchiaking::testing::ManualClock clock;
  PairingKnock client{network, clock};
};

class PairingKnockZeroLengthReply : public PairingKnockDoor,
                                    public ::testing::WithParamInterface<bool> {
};

TEST_F(PairingKnockDoor, SendsTheExactKnockFromABoundPort) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.generation = Generation::ps5;
  config.destination = endpoint.value();
  ASSERT_TRUE(client.start(config));
  EXPECT_FALSE(client.start(config));
  const auto [request, received] = receive(*console);
  EXPECT_EQ(received.size, 5U);
  EXPECT_EQ(request[0], std::byte{'S'});
  EXPECT_EQ(request[1], std::byte{'R'});
  EXPECT_EQ(request[2], std::byte{'C'});
  EXPECT_EQ(request[3], std::byte{'3'});
  EXPECT_EQ(request[4], std::byte{0});
  EXPECT_NE(received.source.port, 0);
}

TEST_F(PairingKnockDoor, MatchesOnlyTheRightAnswerFromTheConsole) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.generation = Generation::ps5;
  config.destination = endpoint.value();
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*console);
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES2"));
  const auto found = client.pump_once();
  ASSERT_TRUE(found);
  EXPECT_FALSE(found.value());
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES3suffix"));
  const auto match = wait_for_match();
  ASSERT_TRUE(match);
  EXPECT_EQ(*match, endpoint.value());
  EXPECT_FALSE(client.pump_once());
  EXPECT_TRUE(client.cancel());
}

TEST_F(PairingKnockDoor, IgnoresAnAnswerFromAnotherPortWhenUnicast) {
  auto console = peer();
  auto other = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.generation = Generation::ps4;
  config.destination = endpoint.value();
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*console);
  EXPECT_EQ(request[3], std::byte{'2'});
  ASSERT_NO_FATAL_FAILURE(send(*other, received.source, "RES2"));
  const auto found = client.pump_once();
  ASSERT_TRUE(found);
  EXPECT_FALSE(found.value());
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES2"));
  const auto match = wait_for_match();
  ASSERT_TRUE(match);
  EXPECT_EQ(*match, endpoint.value());
}

TEST_F(PairingKnockDoor, AcceptsAnAnswerFromAnySourceWhenBroadcast) {
  auto destination = peer();
  auto answerer = peer();
  const auto destination_endpoint = destination->local_endpoint();
  const auto answerer_endpoint = answerer->local_endpoint();
  ASSERT_TRUE(destination_endpoint);
  ASSERT_TRUE(answerer_endpoint);
  PairingKnock::Door config;
  config.generation = Generation::ps4;
  config.destination = destination_endpoint.value();
  config.broadcast = true;
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*destination);
  EXPECT_EQ(request[3], std::byte{'2'});
  ASSERT_NO_FATAL_FAILURE(send(*answerer, received.source, "RES2"));
  const auto match = wait_for_match();
  ASSERT_TRUE(match);
  EXPECT_EQ(*match, answerer_endpoint.value());
}

TEST_F(PairingKnockDoor, TimesOutAfterThreeSeconds) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.destination = endpoint.value();
  ASSERT_TRUE(client.start(config));
  static_cast<void>(receive(*console));
  clock.now_us += 2'999'999;
  const auto waiting = client.pump_once();
  ASSERT_TRUE(waiting);
  EXPECT_FALSE(waiting.value());
  ++clock.now_us;
  const auto timed_out = client.pump_once();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), PairingKnock::Error::timeout);
  EXPECT_FALSE(client.pump_once());
  EXPECT_TRUE(client.cancel());
}

TEST_F(PairingKnockDoor, RestartsAfterACancelAndTakesAQueuedAnswerLate) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.destination = endpoint.value();
  ASSERT_TRUE(client.start(config));
  static_cast<void>(receive(*console));
  clock.now_us += 3'000'000;
  EXPECT_FALSE(client.pump_once());
  ASSERT_TRUE(client.cancel());
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*console);
  EXPECT_EQ(request[3], std::byte{'3'});
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES3"));
  pause(std::chrono::milliseconds{1});
  clock.now_us += 3'000'000;
  const auto queued = wait_for_match();
  ASSERT_TRUE(queued);
  EXPECT_EQ(*queued, endpoint.value());
  EXPECT_FALSE(client.pump_once());
}

TEST_F(PairingKnockDoor, SurfacesAnUnreachableConsoleWhenUnicast) {
  platform::SocketEndpoint closed_endpoint;
  {
    auto closed = peer();
    const auto endpoint = closed->local_endpoint();
    ASSERT_TRUE(endpoint);
    closed_endpoint = endpoint.value();
    ASSERT_TRUE(closed->close());
  }
  PairingKnock::Door config;
  config.destination = closed_endpoint;
  ASSERT_TRUE(client.start(config));
  std::optional<PairingKnock::Error> error;
  const auto deadline = now() + std::chrono::seconds{1};
  while (!error && now() < deadline) {
    const auto result = client.pump_once();
    if (!result)
      error = result.error();
    else
      pause(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(error, PairingKnock::Error::network_failure);
}

TEST_F(PairingKnockDoor, FallsBackToTheNextDestinationWhenUnicast) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.destination = {platform::IpAddress::ipv6({0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 1})
                            .with_scope_id(4'000'000'000U),
                        endpoint.value().port};
  config.fallbacks = {endpoint.value()};
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*console);
  EXPECT_EQ(request[3], std::byte{'3'});
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES3"));
  const auto match = wait_for_match();
  ASSERT_TRUE(match);
  EXPECT_EQ(*match, endpoint.value());
}

TEST_P(PairingKnockZeroLengthReply, FailsTheSearch) {
  auto console = peer();
  const auto endpoint = console->local_endpoint();
  ASSERT_TRUE(endpoint);
  PairingKnock::Door config;
  config.generation = Generation::ps5;
  config.destination = endpoint.value();
  config.broadcast = GetParam();
  ASSERT_TRUE(client.start(config));
  const auto [request, received] = receive(*console);
  EXPECT_EQ(received.size, 5U);
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, ""));
  ASSERT_NO_FATAL_FAILURE(send(*console, received.source, "RES3"));
  const auto deadline = now() + std::chrono::seconds{2};
  std::optional<PairingKnock::Error> failure;
  while (!failure && now() < deadline) {
    const auto result = client.pump_once();
    if (!result) {
      failure = result.error();
      break;
    }
    EXPECT_FALSE(result.value());
    pause(std::chrono::milliseconds{1});
  }
  EXPECT_EQ(failure, PairingKnock::Error::network_failure);
  EXPECT_FALSE(client.pump_once());
  EXPECT_EQ(client.pollable(), nullptr);
}

INSTANTIATE_TEST_SUITE_P(UnicastAndBroadcast, PairingKnockZeroLengthReply,
                         ::testing::Bool());

}  // namespace
}  // namespace consoles_test
