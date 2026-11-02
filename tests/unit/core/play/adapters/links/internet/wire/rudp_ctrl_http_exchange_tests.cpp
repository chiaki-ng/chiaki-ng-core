#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;
using rudp_ctrl::CtrlFrames;
using rudp_ctrl::ExchangeChannel;
using ExchangeError = remote_play::RudpCtrlHttpExchangeError;

class RudpCtrlHttpExchange : public ::testing::Test {
 protected:
  auto run(std::uint16_t local, std::uint16_t remote,
           std::span<const std::byte> request, platform::StopToken stop = {}) {
    remote_play::RudpCtrlHttpExchange exchange(local, remote, request, channel_,
                                               clock_, stop);
    return exchange.run();
  }

  static auto http_response_packet() -> rudp_ctrl::Datagram {
    auto payload = ExchangeChannel::counter_bytes(0x5678);
    const std::vector<std::byte> http = {
        std::byte{0x48}, std::byte{0x54}, std::byte{0x54},
        std::byte{0x50}, std::byte{0x0D}, std::byte{0x0A},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0xAA}};
    payload.insert(payload.end(), http.begin(), http.end());
    return ExchangeChannel::packet(0x0230, std::move(payload));
  }

  test_support::FakeClock clock_{0};
  ExchangeChannel channel_{clock_};
};

TEST_F(RudpCtrlHttpExchange, ResendsTheRequestUntilTheConsoleReplies) {
  bool queued = false;
  channel_.on_receive([&] {
    if (!queued &&
        clock_.now_microseconds() >= rudp::rudp_ctrl_reply_timeout_us) {
      channel_.inbox().push_back(
          ExchangeChannel::packet(0x2030, {std::byte{0x00}, std::byte{0x01},
                                           std::byte{0x00}, std::byte{0x02}}));
      channel_.inbox().push_back(http_response_packet());
      queued = true;
    }
  });
  const std::vector<std::byte> request = {std::byte{0xAA}, std::byte{0xBB}};
  const auto outcome = run(0x1000, 0x5677, request);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->next_local_counter, 0x1002);
  EXPECT_EQ(outcome->remote_counter, 0x5679);
  EXPECT_EQ(outcome->response.header_size, 8U);
  EXPECT_EQ(outcome->response.received.back(), std::byte{0xAA});
  ASSERT_EQ(channel_.sent().size(), 3U);
  EXPECT_EQ(channel_.sent()[0],
            rudp::FrameCodec::encode_session_message(0x1000, 0x5677, request)
                .value());
  EXPECT_EQ(channel_.sent()[1],
            rudp::FrameCodec::encode_session_message(0x1001, 0x5677, request)
                .value());
  EXPECT_EQ(channel_.sent()[2],
            rudp::FrameCodec::encode_ack_message(0x1002, 0x5679).value());
}

TEST_F(RudpCtrlHttpExchange, GivesUpAfterThreeTimeouts) {
  const std::vector<std::byte> request = {std::byte{0xAA}};
  const auto outcome = run(0x0100, 0x0200, request);
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::timeout);
  EXPECT_EQ(outcome.error().next_local_counter, 0x0103);
  EXPECT_EQ(channel_.sent().size(), 3U);
}

TEST_F(RudpCtrlHttpExchange, SendsNothingOnceStopped) {
  auto state = std::make_shared<test_support::StopState>();
  state->stopped = true;
  const auto outcome = run(1, 2, {}, platform::StopToken{state});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::stopped);
  EXPECT_TRUE(channel_.sent().empty());
}

TEST_F(RudpCtrlHttpExchange, ReportsAShortSend) {
  channel_.short_send(true);
  const auto outcome = run(1, 2, {});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::short_send);
}

TEST_F(RudpCtrlHttpExchange, ReportsAFailedSend) {
  channel_.send_error(datagram::TransportError::closed);
  const auto outcome = run(1, 2, {});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::send_failed);
}

TEST_F(RudpCtrlHttpExchange, ReportsAFailedReceive) {
  channel_.receive_error(datagram::TransportError::closed);
  const auto outcome = run(1, 2, {});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::receive_failed);
}

TEST_F(RudpCtrlHttpExchange, ReportsAShortAcknowledgement) {
  channel_.inbox().push_back(http_response_packet());
  channel_.short_send_on_call(2);
  const auto outcome = run(1, 2, {});
  ASSERT_FALSE(outcome);
  EXPECT_EQ(outcome.error().error, ExchangeError::short_send);
  EXPECT_EQ(channel_.sent().size(), 2U);
}

TEST_F(RudpCtrlHttpExchange, CarriesASessionRequestAndItsAcceptance) {
  constexpr std::string_view http = CtrlFrames::accepted_http;
  auto payload = ExchangeChannel::counter_bytes(0x5678);
  for (const char character : http)
    payload.push_back(static_cast<std::byte>(character));
  channel_.inbox().push_back(
      ExchangeChannel::packet(0x0230, std::move(payload)));
  const auto request = remote_play::SessionCodec::encode_request(
      remote_play::RegistrationTarget::ps5_1, "127.0.0.1", 9295,
      CtrlFrames::registration_key);
  ASSERT_TRUE(request);
  const auto outcome = run(0x1000, 0x5677, request.value().bytes());
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->next_local_counter, 0x1001);
  EXPECT_EQ(outcome->remote_counter, 0x5679);
  EXPECT_EQ(outcome->response.header_size, http.size());
  ASSERT_EQ(channel_.sent().size(), 2U);
  EXPECT_EQ(channel_.sent()[0], rudp::FrameCodec::encode_session_message(
                                    0x1000, 0x5677, request.value().bytes())
                                    .value());
  EXPECT_EQ(channel_.sent()[1],
            rudp::FrameCodec::encode_ack_message(0x1001, 0x5679).value());
  const auto response = remote_play::SessionCodec::parse_response(
      remote_play::RegistrationTarget::ps5_1,
      {outcome->response.received.data(), outcome->response.received.size()});
  ASSERT_TRUE(response);
  ASSERT_TRUE(response.value());
  for (std::size_t i = 0; i < 16; ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(std::to_integer<unsigned>(response.value()->nonce().data()[i]), i);
  }
}

}  // namespace
}  // namespace play_test
