#include "tests/unit/core/net/protocol/remote_play/tcp_exchange_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::RegistrationTarget;

class TcpExchangeScripted : public ::testing::Test {
 protected:
  auto begin() -> testing::AssertionResult {
    auto request = TcpExchangeRig::request(RegistrationTarget::ps5_1, 9295);
    if (!request) return testing::AssertionFailure() << "no request";
    expected = std::string(request->view());
    if (!exchange.begin({platform::IpAddress::loopback_ipv4(), 9295},
                        std::move(*request),
                        remote_play::SessionReply{RegistrationTarget::ps5_1}))
      return testing::AssertionFailure() << "the exchange did not begin";
    return testing::AssertionSuccess();
  }

  TcpExchangeSocketScript script;
  TcpExchangeScriptedSockets sockets{script};
  test_support::FakeClock clock{1'000'000};
  remote_play::SessionTcpExchange exchange{sockets, clock};
  std::string expected;
};

TEST_F(TcpExchangeScripted, ADroppedConnectionReturnsToTheConnectDeadline) {
  script.connects.push_back(
      libchiaking::unexpected(platform::NetError::would_block));
  script.completions.push_back({});
  script.sends.push_back(
      libchiaking::unexpected(platform::NetError::not_connected));
  ASSERT_TRUE(begin());
  clock.now_us = 2'000'000;
  auto progress = exchange.pump_once();
  ASSERT_TRUE(progress);
  EXPECT_FALSE(progress.value());
  EXPECT_TRUE(script.written.empty());
  clock.now_us = 5'999'999;
  progress = exchange.pump_once();
  ASSERT_TRUE(progress);
  EXPECT_FALSE(progress.value());
  clock.now_us = 6'000'000;
  progress = exchange.pump_once();
  ASSERT_FALSE(progress);
  EXPECT_EQ(progress.error(), remote_play::ConsoleExchangeError::timeout);
  EXPECT_TRUE(exchange.failed_while_connecting());
  const auto failure = exchange.last_failure_progress();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->request_bytes_sent, 0U);
  EXPECT_FALSE(failure->retried_header_timeout);
  EXPECT_EQ(script.closes, 1U);
  EXPECT_EQ(script.created, 1U);
}

TEST_F(TcpExchangeScripted, SendsTheRequestInOrderAcrossShortWrites) {
  script.sends.push_back(std::size_t{5});
  script.sends.push_back(
      libchiaking::unexpected(platform::NetError::would_block));
  script.sends.push_back(std::size_t{7});
  script.replies.push_back(
      "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(begin());
  auto progress = exchange.pump_once();
  ASSERT_TRUE(progress);
  EXPECT_FALSE(progress.value());
  EXPECT_EQ(script.written.size(), 5U);
  clock.now_us += 4'999'999;
  progress = exchange.pump_once();
  ASSERT_TRUE(progress);
  EXPECT_TRUE(progress.value());
  EXPECT_EQ(script.written, TcpExchangeRig::byte_vector(expected));
  EXPECT_EQ(script.closes, 1U);
  const auto result = exchange.take();
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.value());
  EXPECT_FALSE(exchange.last_failure_progress());
}

}  // namespace
}  // namespace remote_play_test
