#include "tests/unit/core/net/protocol/remote_play/tcp_exchange_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::RegistrationTarget;

class TcpExchangeLoopback : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(peer.open()); }

  void begin(RegistrationTarget target) {
    auto request = TcpExchangeRig::request(target, peer.endpoint.port);
    ASSERT_TRUE(request);
    expected = std::string(request->view());
    ASSERT_TRUE(exchange.begin(peer.endpoint, std::move(*request),
                               remote_play::SessionReply{target}));
    accepted = peer.accept_until(exchange);
    ASSERT_TRUE(accepted);
  }

  auto receive_part(std::vector<std::byte>& received) -> bool {
    std::array<std::byte, 512> buffer{};
    const auto part = accepted->receive(buffer);
    if (!part) {
      EXPECT_EQ(part.error(), platform::NetError::would_block);
      return false;
    }
    received.insert(received.end(), buffer.begin(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
    return part.value() > 0;
  }

  void await_request() {
    std::vector<std::byte> received;
    const auto deadline =
        TcpExchangeRig::deadline_after(std::chrono::seconds{2});
    bool request_received = false;
    while (!request_received && TcpExchangeRig::still_before(deadline)) {
      const auto progress = exchange.pump_once();
      ASSERT_TRUE(progress);
      EXPECT_FALSE(progress.value());
      request_received = receive_part(received);
      TcpExchangeRig::nap();
    }
    ASSERT_TRUE(request_received);
  }

  TcpExchangeLoopbackPeer peer;
  test_support::FakeClock clock{1'000'000};
  remote_play::SessionTcpExchange exchange{peer.network.sockets(), clock};
  std::unique_ptr<platform::TcpSocket> accepted;
  std::string expected;
};

TEST_F(TcpExchangeLoopback, CompletesAnExchangeWithARealSocket) {
  ASSERT_NO_FATAL_FAILURE(begin(RegistrationTarget::ps5_1));
  std::vector<std::byte> received;
  const auto deadline = TcpExchangeRig::deadline_after(std::chrono::seconds{2});
  while (received.size() < expected.size() &&
         TcpExchangeRig::still_before(deadline)) {
    const auto progress = exchange.pump_once();
    ASSERT_TRUE(progress);
    EXPECT_FALSE(progress.value());
    receive_part(received);
    TcpExchangeRig::nap();
  }
  EXPECT_EQ(received, TcpExchangeRig::byte_vector(expected));
  ASSERT_TRUE(TcpExchangeRig::send_all(
      *accepted, "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n"));
  EXPECT_FALSE(exchange.pump_once().value());
  ASSERT_TRUE(TcpExchangeRig::send_all(*accepted, "\r\n"));
  bool done = false;
  while (!done && TcpExchangeRig::still_before(deadline)) {
    const auto progress = exchange.pump_once();
    ASSERT_TRUE(progress);
    done = progress.value();
    TcpExchangeRig::nap();
  }
  ASSERT_TRUE(done);
  const auto result = exchange.take();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value());
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(result.value()->nonce()[i], static_cast<std::byte>(i))
        << "index " << i;
  EXPECT_FALSE(exchange.take());
  EXPECT_TRUE(exchange.cancel());
  EXPECT_TRUE(accepted->close());
  EXPECT_TRUE(peer.listener->close());
}

TEST_F(TcpExchangeLoopback, TimesOutFiveSecondsAfterTheRequestIsDelivered) {
  ASSERT_NO_FATAL_FAILURE(begin(RegistrationTarget::ps4_9));
  ASSERT_NO_FATAL_FAILURE(await_request());
  clock.now_us += 4'999'999;
  const auto pending = exchange.pump_once();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending.value());
  clock.now_us += 1;
  const auto expired = exchange.pump_once();
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error(), remote_play::ConsoleExchangeError::timeout);
  EXPECT_TRUE(exchange.cancel());
  EXPECT_FALSE(exchange.take());
  EXPECT_TRUE(accepted->close());
  EXPECT_TRUE(peer.listener->close());
}

struct TcpExchangeBrokenReply {
  std::string wire;
  bool close_peer;
  remote_play::ConsoleExchangeError expected;
};

class TcpExchangeLoopbackFailure
    : public TcpExchangeLoopback,
      public ::testing::WithParamInterface<TcpExchangeBrokenReply> {};

TEST_P(TcpExchangeLoopbackFailure, ReportsWhatWasWrongWithTheReply) {
  const auto& reply = GetParam();
  ASSERT_NO_FATAL_FAILURE(begin(RegistrationTarget::ps5_1));
  ASSERT_NO_FATAL_FAILURE(await_request());
  ASSERT_TRUE(TcpExchangeRig::send_all(*accepted, reply.wire));
  if (reply.close_peer) {
    EXPECT_TRUE(accepted->close());
  }
  const auto deadline = TcpExchangeRig::deadline_after(std::chrono::seconds{2});
  bool failed = false;
  while (!failed && TcpExchangeRig::still_before(deadline)) {
    const auto progress = exchange.pump_once();
    if (!progress) {
      EXPECT_EQ(progress.error(), reply.expected);
      failed = true;
    } else {
      EXPECT_FALSE(progress.value());
    }
    TcpExchangeRig::nap();
  }
  EXPECT_TRUE(failed);
  EXPECT_TRUE(exchange.cancel());
  if (!reply.close_peer) {
    EXPECT_TRUE(accepted->close());
  }
  EXPECT_TRUE(peer.listener->close());
}

INSTANTIATE_TEST_SUITE_P(
    Replies, TcpExchangeLoopbackFailure,
    ::testing::Values(
        TcpExchangeBrokenReply{"HTTP/1.1 200 OK\r\nRP-Nonce: AAE", true,
                               remote_play::ConsoleExchangeError::truncated},
        TcpExchangeBrokenReply{std::string(512, 'x'), false,
                               remote_play::ConsoleExchangeError::too_large},
        TcpExchangeBrokenReply{
            "not HTTP\r\n\r\n", false,
            remote_play::ConsoleExchangeError::invalid_response}),
    [](const auto& tested) {
      switch (tested.param.expected) {
        case remote_play::ConsoleExchangeError::truncated:
          return std::string{"Truncated"};
        case remote_play::ConsoleExchangeError::too_large:
          return std::string{"TooLarge"};
        default:
          return std::string{"InvalidResponse"};
      }
    });

class TcpExchangeRefused : public ::testing::Test {
 protected:
  auto closed_loopback_endpoint() -> std::optional<platform::SocketEndpoint> {
    auto created = network.sockets().create_tcp_listener(
        platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(created);
    if (!created) return std::nullopt;
    EXPECT_TRUE(
        created.value()->bind({platform::IpAddress::loopback_ipv4(), 0}));
    const auto local = created.value()->local_endpoint();
    EXPECT_TRUE(local);
    if (!local) return std::nullopt;
    EXPECT_TRUE(created.value()->close());
    return local.value();
  }

  platform::DefaultNetworkProvider network{TcpExchangeRig::threading(),
                                           TcpExchangeRig::steady()};
  test_support::FakeClock clock{1'000'000};
};

TEST_F(TcpExchangeRefused, ReportsARefusedConnectDistinctly) {
  const auto endpoint = closed_loopback_endpoint();
  ASSERT_TRUE(endpoint);
  remote_play::SessionTcpExchange exchange(network.sockets(), clock);
  auto request =
      TcpExchangeRig::request(RegistrationTarget::ps4_9, endpoint->port);
  ASSERT_TRUE(request);
  const auto begun =
      exchange.begin(*endpoint, std::move(*request),
                     remote_play::SessionReply{RegistrationTarget::ps4_9});
  if (!begun) {
    EXPECT_EQ(begun.error(),
              remote_play::ConsoleExchangeError::connection_refused);
    EXPECT_TRUE(exchange.failed_while_connecting());
    return;
  }
  const auto deadline = TcpExchangeRig::deadline_after(std::chrono::seconds{2});
  while (TcpExchangeRig::still_before(deadline)) {
    const auto progress = exchange.pump_once();
    if (!progress) {
      EXPECT_EQ(progress.error(),
                remote_play::ConsoleExchangeError::connection_refused);
      EXPECT_TRUE(exchange.failed_while_connecting());
      return;
    }
    EXPECT_FALSE(progress.value());
    TcpExchangeRig::nap();
  }
  ADD_FAILURE() << "the refused connect was never reported";
}

}  // namespace
}  // namespace remote_play_test
