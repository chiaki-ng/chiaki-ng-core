#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"
#include "src/core/net/transport/rudp/rudp.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/holepunch_accept_rig.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
using holepunch::SelectedAccept;
using HandoffError = internet::holepunch::CtrlRudpHandoffError;

class HolepunchAcceptHandoff : public ::testing::Test {
 protected:
  struct ConsoleRudpPort {
    std::atomic<unsigned> inits{};
    std::atomic<bool> established{};
  };

  static auto ready_wait() -> internet::holepunch::AcceptWait {
    internet::holepunch::AcceptWait wait;
    EXPECT_TRUE(wait.begin(1));
    nat::SessionMessage accept{};
    accept.action = nat::Action::accept;
    accept.req_id = 7;
    wait.on_message(1, accept);
    EXPECT_TRUE(wait.take_ack().has_value());
    EXPECT_TRUE(wait.ack_sent(1));
    return wait;
  }

  static auto rudp_reply(std::uint16_t type, std::vector<std::byte> payload)
      -> std::vector<std::byte> {
    const std::array<const rudp::Frame, 1> frames{
        rudp::Frame{type, std::move(payload)}};
    auto encoded = rudp::FrameCodec::encode(frames);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return encoded.value();
  }

  static void serve_console_rudp(platform::NetworkProvider& network,
                                 platform::SocketEndpoint endpoint,
                                 std::chrono::milliseconds delay,
                                 ConsoleRudpPort& port) {
    test_support::LiveProviders::nap(delay);
    auto socket = network.sockets().create_udp_socket(endpoint.address);
    ASSERT_TRUE(socket);
    ASSERT_TRUE(socket.value()->bind(endpoint));
    auto poller = network.pollers().create_poller();
    ASSERT_TRUE(poller);
    std::array<std::byte, 2048> buffer{};
    const auto deadline = test_support::LiveProviders::steady_us() + 5'000'000;
    while (!port.established && test_support::LiveProviders::steady_us() < deadline) {
      const std::array<platform::PollRequest, 1> requests{
          {{socket.value().get(), 1, platform::PollEvent::readable}}};
      auto ready = poller.value()->wait(requests, std::chrono::milliseconds{50},
                                        platform::StopToken{});
      ASSERT_TRUE(ready);
      if (ready.value().ready.empty()) continue;
      auto received = socket.value()->receive_from(buffer);
      if (!received) continue;
      auto frames = rudp::FrameCodec::decode(
          std::span<const std::byte>{buffer.data(), received.value().size});
      if (!frames || frames.value().empty()) continue;
      std::vector<std::byte> reply;
      if (frames.value().front().type == 0x8030) {
        ++port.inits;
        reply = rudp_reply(
            0xD000, {std::byte{0x56}, std::byte{0x78}, std::byte{}, std::byte{},
                     std::byte{}, std::byte{}, std::byte{}, std::byte{},
                     std::byte{0xC0}, std::byte{0x0E}});
      } else if (frames.value().front().type == 0x9030) {
        reply = rudp_reply(0xA030, {std::byte{0x56}, std::byte{0x79}});
        port.established = true;
      } else {
        continue;
      }
      EXPECT_TRUE(socket.value()->send_to(reply, received.value().source));
    }
    EXPECT_TRUE(socket.value()->close());
  }

  static auto run_rudp_handshake(datagram::ProviderUdpTransport& transport,
                                 std::uint64_t attempt)
      -> libchiaking::Expected<rudp::HandshakeCompletion,
                               rudp::ConnectError> {
    rudp::ProviderRudpHandshakeChannel channel{transport};
    return rudp::Connector::connect_over_channel(
        channel, rudp::HandshakeParameters{0x1234, 0x01020304},
        test_support::LiveProviders::steady(), platform::StopToken{}, attempt);
  }

  auto closed_endpoint() -> platform::SocketEndpoint {
    auto closed = network_.sockets().create_udp_socket(
        platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(closed);
    if (!closed) return {};
    EXPECT_TRUE(
        closed.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    const auto endpoint = closed.value()->local_endpoint().value();
    EXPECT_TRUE(closed.value()->close());
    return endpoint;
  }

  platform::DefaultNetworkProvider network_{test_support::LiveProviders::threading(),
                                            test_support::LiveProviders::steady()};
  const platform::SocketEndpoint peer_{platform::IpAddress::loopback_ipv4(),
                                       9295};
};

TEST_F(HolepunchAcceptHandoff, RefusesAWaitThatNeverBegan) {
  internet::holepunch::AcceptWait idle;
  auto not_ready = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_, SelectedAccept::selected(network_, peer_), idle);
  ASSERT_FALSE(not_ready);
  EXPECT_EQ(not_ready.error(), HandoffError::accept_not_ready);
}

TEST_F(HolepunchAcceptHandoff, RefusesAWaitWhoseAcknowledgementIsUnsent) {
  internet::holepunch::AcceptWait pending;
  ASSERT_TRUE(pending.begin(2));
  nat::SessionMessage accept{};
  accept.action = nat::Action::accept;
  accept.req_id = 3;
  pending.on_message(2, accept);
  auto before_ack = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_, SelectedAccept::selected(network_, peer_), pending);
  ASSERT_FALSE(before_ack);
  EXPECT_EQ(before_ack.error(), HandoffError::accept_not_ready);
}

TEST_F(HolepunchAcceptHandoff, RefusesAPeerWithoutAnAddress) {
  const auto ready = ready_wait();
  auto invalid = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_,
      SelectedAccept::selected(network_,
                               {platform::IpAddress::any_ipv4(), 9295}),
      ready);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), HandoffError::invalid_peer);
}

TEST_F(HolepunchAcceptHandoff, AdoptsTheSelectedSocketForTheRoute) {
  const auto ready = ready_wait();
  auto receiver = datagram::ProviderUdpTransport::create(
      network_,
      datagram::UdpTransportConfig{{platform::IpAddress::loopback_ipv4(), 0}});
  ASSERT_TRUE(receiver.has_value());
  const auto receiver_endpoint = receiver.value()->local_endpoint();
  ASSERT_TRUE(receiver_endpoint.has_value());
  auto selected = SelectedAccept::selected(network_, receiver_endpoint.value());
  ASSERT_TRUE(selected.socket);
  const auto local = selected.socket->local_endpoint();
  ASSERT_TRUE(local);
  auto route = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_, std::move(selected), ready);
  ASSERT_TRUE(route);
  ASSERT_TRUE(route.value().transport);
  EXPECT_EQ(route.value().local_endpoint, local.value());
  EXPECT_EQ(route.value().peer_endpoint, receiver_endpoint.value());
  auto adopted_local = route.value().transport->local_endpoint();
  ASSERT_TRUE(adopted_local);
  EXPECT_EQ(adopted_local.value(), local.value());
}

TEST_F(HolepunchAcceptHandoff, ReadsNothingWhenTheConsoleRefusesDatagrams) {
  const auto ready = ready_wait();
  auto refused = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_, SelectedAccept::selected(network_, closed_endpoint()), ready);
  ASSERT_TRUE(refused);
  ASSERT_TRUE(refused.value().transport);
  const std::array<std::byte, 2> payload{std::byte{1}, std::byte{2}};
  for (int index = 0; index < 4; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(refused.value().transport->send(payload));
    test_support::LiveProviders::nap(std::chrono::milliseconds{5});
  }
  for (int index = 0; index < 8; ++index) {
    SCOPED_TRACE(index);
    auto received = refused.value().transport->receive();
    ASSERT_TRUE(received);
    EXPECT_FALSE(received.value());
  }
}

TEST_F(HolepunchAcceptHandoff, HandshakesControlAfterQueuedConsoleRefusals) {
  const auto console = closed_endpoint();
  auto socket = network_.sockets().create_udp_socket(
      platform::IpAddress::loopback_ipv4());
  ASSERT_TRUE(socket);
  ASSERT_TRUE(socket.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
  nat::SelectedCandidate selected{std::move(socket.value()), console, 1, false};
  const auto ready = ready_wait();
  auto route = internet::holepunch::Accept::handoff_ctrl_rudp(
      network_, std::move(selected), ready);
  ASSERT_TRUE(route);
  ASSERT_TRUE(route.value().transport);

  ConsoleRudpPort registration_port;
  auto registration_console = test_support::LiveProviders::spawn("registration console", [&] {
    serve_console_rudp(network_, console, std::chrono::milliseconds{0},
                       registration_port);
  });
  test_support::LiveProviders::nap(std::chrono::milliseconds{50});
  auto registered = run_rudp_handshake(*route.value().transport, 1);
  static_cast<void>(registration_console.join());
  EXPECT_TRUE(registered);
  EXPECT_EQ(registration_port.inits.load(), 1U);

  const std::array<std::byte, 9> stale{};
  for (int index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(route.value().transport->send(stale));
    test_support::LiveProviders::nap(std::chrono::milliseconds{5});
  }

  ConsoleRudpPort ctrl_port;
  auto ctrl_console = test_support::LiveProviders::spawn("ctrl console", [&] {
    serve_console_rudp(network_, console, std::chrono::milliseconds{300},
                       ctrl_port);
  });
  const auto started = test_support::LiveProviders::steady_us();
  auto ctrl = run_rudp_handshake(*route.value().transport, 2);
  const auto elapsed = std::chrono::microseconds{
      static_cast<std::int64_t>(test_support::LiveProviders::steady_us() - started)};
  static_cast<void>(ctrl_console.join());
  EXPECT_TRUE(ctrl);
  EXPECT_GE(ctrl_port.inits.load(), 1U);
  EXPECT_GE(elapsed, std::chrono::milliseconds{300});
}

}  // namespace
}  // namespace play_test
