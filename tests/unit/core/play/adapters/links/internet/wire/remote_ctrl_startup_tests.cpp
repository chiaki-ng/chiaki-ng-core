#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/remote_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
using remote_ctrl::CtrlMessages;
using remote_ctrl::StartupChannel;
using remote_ctrl::ZeroCrypto;
using StartupError = internet::remote_ctrl::StartupError;

class RemoteCtrlStartup : public ::testing::Test {
 protected:
  auto start(remote_play::PreparedLocalCtrl&& request) {
    return internet::remote_ctrl::Connector::start_http(
        channel_, handshake_, std::move(request), crypto_,
        remote_play::RegistrationTarget::ps5_1, clock_, {});
  }

  auto prepared() -> remote_play::PreparedLocalCtrl {
    return CtrlMessages::prepared(crypto_);
  }

  static auto connect_failure(internet::remote_ctrl::StartupFailure failure)
      -> play::LinkFailure {
    return play::LinkFailure::refused(internet::remote_ctrl::ConnectFailure{
        internet::remote_ctrl::ConnectError::startup_failed, 0, failure});
  }

  test_support::FakeClock clock_{0};
  StartupChannel channel_{clock_};

 private:
  ZeroCrypto crypto_;
  rudp::HandshakeCompletion handshake_{0, 0x1234, 0x5677};
};

TEST_F(RemoteCtrlStartup, RefusesAnUnpreparedRequestBeforeSending) {
  const auto invalid = start(remote_play::PreparedLocalCtrl{});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().kind, StartupError::invalid_request);
  EXPECT_TRUE(channel_.sent().empty());
}

TEST_F(RemoteCtrlStartup, AcceptsTheReplyAndKeepsTheBytesAfterIt) {
  auto request = prepared();
  const auto request_bytes = std::vector<std::byte>(
      request.request.bytes().begin(), request.request.bytes().end());
  channel_.respond(StartupChannel::http_reply("HTTP/1.1 200 OK\r\n\r\nBODY"));
  auto result = start(std::move(request));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().final.next_local_counter, 0x1235);
  EXPECT_EQ(result.value().final.remote_counter, 0x5679);
  EXPECT_EQ(result.value().prepared.nonce[0], std::byte{1});
  EXPECT_EQ(result.value().local_crypt_counter,
            result.value().prepared.request.next_crypt_counter() + 1);
  const auto owner =
      internet::remote_ctrl::Session::create(std::move(result.value()), 3);
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner.value()->outbound_state().crypt_counter, 6U);
  ASSERT_EQ(result.value().final.trailing_received.size(), 4U);
  EXPECT_EQ(result.value().final.trailing_received[0], std::byte{'B'});
  ASSERT_EQ(channel_.sent().size(), 2U);
  EXPECT_EQ(channel_.sent()[0], rudp::FrameCodec::encode_session_message(
                                    0x1234, 0x5677, request_bytes)
                                    .value());
  EXPECT_EQ(channel_.sent()[1],
            rudp::FrameCodec::encode_ack_message(0x1235, 0x5679).value());
}

TEST_F(RemoteCtrlStartup, GivesUpAfterThreeUnansweredRequests) {
  const auto started = clock_.now_us;
  const auto failed = start(prepared());
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind, StartupError::exchange_failed);
  EXPECT_EQ(clock_.now_us - started, 3'000'000U);
  EXPECT_EQ(channel_.sent().size(), 3U);
  EXPECT_EQ(failed.error().cause,
            static_cast<std::uint32_t>(
                remote_play::RudpCtrlHttpExchangeError::timeout));
  const auto timed_out = connect_failure(failed.error());
  EXPECT_EQ(timed_out.result(), remote_play::StreamError::ctrl_timeout);
  EXPECT_NE(timed_out.sentence().find(
                "Ctrl startup response — the response timed out"),
            std::string::npos);
}

TEST_F(RemoteCtrlStartup, KeepsAskingPastAStrayAcknowledgement) {
  const rudp::Frame stray{0x2430,
                          {std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                           std::byte{0x78}, std::byte{0x00}, std::byte{0x92}}};
  auto encoded = rudp::FrameCodec::encode({&stray, 1});
  ASSERT_TRUE(encoded);
  channel_.respond(std::move(encoded.value()));
  const auto started = clock_.now_us;
  const auto unexpected_result = start(prepared());
  EXPECT_FALSE(unexpected_result);
  EXPECT_EQ(channel_.sent().size(), 3U);
  EXPECT_EQ(clock_.now_us - started, 2'000'000U);
}

TEST_F(RemoteCtrlStartup, ReportsARefusedReplyWithItsStatus) {
  channel_.respond(
      StartupChannel::http_reply("HTTP/1.1 403 Forbidden\r\n\r\n"));
  const auto rejected_result = start(prepared());
  ASSERT_FALSE(rejected_result);
  EXPECT_EQ(rejected_result.error().kind, StartupError::finalize_failed);
  const auto detail = connect_failure(rejected_result.error());
  EXPECT_EQ(detail.http_status(), 403);
  EXPECT_EQ(detail.received_bytes(), 26U);
  EXPECT_EQ(detail.result(), remote_play::StreamError::ctrl_refused);
  EXPECT_EQ(rejected_result.error().cause,
            static_cast<std::uint32_t>(
                remote_play::RemoteCtrlHttpFinalizeError::nonaccepted));
  EXPECT_NE(detail.sentence().find("HTTP 403."), std::string::npos);
}

}  // namespace
}  // namespace play_test
