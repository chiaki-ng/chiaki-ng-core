#include "tests/support/stop_states.hpp"
#include "tests/unit/core/net/transport/rudp/rudp_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rudp_test {
namespace {

using libchiaking::net::transport::datagram::TransportError;
using libchiaking::net::transport::rudp::ConnectError;
using libchiaking::net::transport::rudp::Handshake;
using libchiaking::net::transport::rudp::HandshakeRunner;
using Datagram = ScriptedChannel::Datagram;

class RudpHandshakeRunnerRun : public ::testing::Test, protected RudpPackets {
 protected:
  static auto handshake() -> Handshake { return Handshake{1, 0x1234, 0xABCD}; }

  test_support::FakeClock clock{0};
  ScriptedChannel channel{clock};
};

TEST_F(RudpHandshakeRunnerRun, PinsEveryRequestToTheByte) {
  const std::array<std::byte, 4> cookie{std::byte{0xC0}, std::byte{0x00},
                                        std::byte{0x01}, std::byte{0x0E}};
  channel.inbox.push_back(Datagram(16, std::byte{0xFF}));
  channel.inbox.push_back(init_response(0x5678, cookie));
  channel.inbox.push_back(Datagram(16, std::byte{0xFF}));
  channel.inbox.push_back(cookie_response(0x5679));
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().next_local_counter, 0x1238);
  EXPECT_EQ(outcome.value().remote_counter, 0x567A);
  ASSERT_EQ(channel.sent.size(), 4U);
  EXPECT_EQ(channel.sent[0], handshake_request(0x8030, 0x1234, {}));
  EXPECT_EQ(channel.sent[1], handshake_request(0x8030, 0x1235, {}));
  EXPECT_EQ(channel.sent[2], handshake_request(0x9030, 0x1236, cookie));
  EXPECT_EQ(channel.sent[3], handshake_request(0x9030, 0x1237, cookie));
  EXPECT_EQ(clock.now_microseconds(), 0U);
}

TEST_F(RudpHandshakeRunnerRun, ResendsEachSecondOfSilenceThenTimesOut) {
  std::vector<std::uint64_t> sent_at;
  HandshakeRunner runner(handshake(), channel, clock, {});
  channel.on_receive = [&] {
    if (sent_at.size() < channel.sent.size())
      sent_at.push_back(clock.now_microseconds());
  };
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::timeout);
  EXPECT_EQ(sent_at, (std::vector<std::uint64_t>{0, 1'000'000, 2'000'000}));
  ASSERT_GE(channel.sent.size(), 3U);
  EXPECT_EQ(channel.sent[0], handshake_request(0x8030, 0x1234, {}));
  EXPECT_EQ(channel.sent[1], handshake_request(0x8030, 0x1235, {}));
  EXPECT_EQ(channel.sent[2], handshake_request(0x8030, 0x1236, {}));
  EXPECT_EQ(clock.now_microseconds(), 3'000'000U);
}

TEST_F(RudpHandshakeRunnerRun, EstablishesTheSession) {
  channel.inbox.push_back(init_response(
      0x5678, std::array<std::byte, 4>{std::byte{0xC0}, std::byte{0x0},
                                       std::byte{0x1}, std::byte{0xE}}));
  channel.inbox.push_back(cookie_response(0x5679));
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(outcome.value().local_header, 0xABCD);
  EXPECT_EQ(outcome.value().next_local_counter, 0x1236);
  EXPECT_EQ(outcome.value().remote_counter, 0x567A);
  EXPECT_EQ(runner.total_sends(), 2);
  EXPECT_EQ(channel.sent.size(), 2U);
  EXPECT_EQ(clock.now_microseconds(), 0U);
}

TEST_F(RudpHandshakeRunnerRun, TimesOutAfterTheCookieRetries) {
  channel.inbox.push_back(init_response(
      0x5678, std::array<std::byte, 4>{std::byte{0xA}, std::byte{0xB},
                                       std::byte{0xC}, std::byte{0xD}}));
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::timeout);
  EXPECT_EQ(runner.total_sends(), 4);
}

TEST_F(RudpHandshakeRunnerRun, SpendsTriesOnMismatchedPacketsWithoutWaiting) {
  static_assert(libchiaking::net::transport::rudp::rudp_expect_timeout_us ==
                1000000);
  channel.inbox.push_back(Datagram(16, std::byte{0xFF}));
  channel.inbox.push_back(
      encode_one(Frame{0x0001, {std::byte{0}, std::byte{1}}}));
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::timeout);
  EXPECT_EQ(runner.total_sends(), 3);
  EXPECT_EQ(clock.now_microseconds(), 1000000U);
}

TEST_F(RudpHandshakeRunnerRun, AbortsOnAShortDatagram) {
  channel.inbox.push_back(Datagram(8, std::byte{0xC0}));
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::receive_failed);
  EXPECT_EQ(runner.total_sends(), 1);
  EXPECT_EQ(clock.now_microseconds(), 0U);
}

TEST_F(RudpHandshakeRunnerRun, SendsNothingWhenStoppedFirst) {
  auto state = std::make_shared<test_support::StopState>();
  state->stopped = true;
  HandshakeRunner runner(handshake(), channel, clock,
                         libchiaking::platform::StopToken{state});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::stopped);
  EXPECT_EQ(runner.total_sends(), 0);
}

TEST_F(RudpHandshakeRunnerRun, ReportsASendError) {
  channel.send_error = TransportError::io_failure;
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::send_failed);
  EXPECT_EQ(runner.total_sends(), 0);
}

TEST_F(RudpHandshakeRunnerRun, ReportsAShortSend) {
  channel.short_send = true;
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::short_send);
  EXPECT_EQ(runner.total_sends(), 0);
}

TEST_F(RudpHandshakeRunnerRun, ReportsAReceiveError) {
  channel.receive_error = TransportError::closed;
  HandshakeRunner runner(handshake(), channel, clock, {});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::receive_failed);
  EXPECT_EQ(runner.total_sends(), 1);
}

TEST_F(RudpHandshakeRunnerRun, ReportsAStopDuringTheReceive) {
  auto state = std::make_shared<test_support::StopState>();
  channel.on_receive = [state] { state->stopped = true; };
  channel.receive_error = TransportError::closed;
  HandshakeRunner runner(handshake(), channel, clock,
                         libchiaking::platform::StopToken{state});
  const auto outcome = runner.run();
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error(), ConnectError::stopped);
  EXPECT_EQ(runner.total_sends(), 1);
  EXPECT_EQ(channel.sent.size(), 1U);
}

}  // namespace
}  // namespace rudp_test
