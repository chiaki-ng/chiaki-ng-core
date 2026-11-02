#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_session_init_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using rudp_ctrl::CtrlFrames;
using rudp_ctrl::ExchangeChannel;
using InitError = remote_play::RemoteSessionInitError;
using Target = remote_play::RegistrationTarget;

class RemoteSessionInitRetry : public rudp_ctrl::SessionInitRig {
 protected:
  static constexpr std::string_view ps4_mismatch =
      "HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
      "80108b11\r\nRP-Version: 9.0\r\n\r\n";
  static constexpr std::string_view back_mismatch =
      "HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
      "80108b11\r\nRP-Version: 10.0\r\n\r\n";
  static constexpr std::string_view unknown_mismatch =
      "HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
      "80108bff\r\nRP-Version: 9.0\r\n\r\n";
  static constexpr std::string_view unknown_back =
      "HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
      "80108bff\r\nRP-Version: 10.0\r\n\r\n";
};

TEST_F(RemoteSessionInitRetry, AsksAgainWithTheVersionTheConsoleNames) {
  constexpr std::string_view mismatch =
      "HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
      "80108b11\r\nRP-Version: 5.0\r\n\r\n";
  channel_.queue_session_init_round(0x5678, mismatch);
  channel_.queue_session_init_round(0x6678, CtrlFrames::accepted_http);
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().target, Target::ps4_9);
  EXPECT_EQ(result.value().completion.local_header, 0xA1B2C3D4U);
  EXPECT_EQ(result.value().completion.remote_counter, 0x667B);
  EXPECT_EQ(channel_.count_frames(0x8030), 2U);
  EXPECT_EQ(channel_.count_frames(0x2030), 2U);
  EXPECT_TRUE(channel_.sent_contains("Rp-Version: 9.0"));
  EXPECT_TRUE(channel_.inbox().empty());
}

TEST_F(RemoteSessionInitRetry, StopsAfterThreeRequestsWhenTheVersionsBounce) {
  channel_.queue_session_init_round(0x5678, ps4_mismatch);
  channel_.queue_session_init_round(0x6678, back_mismatch);
  channel_.queue_session_init_round(0x7678, ps4_mismatch);
  channel_.queue_session_init_round(0x8678, CtrlFrames::accepted_http);
  console().target = Target::ps4_10;
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, InitError::response_rejected);
  EXPECT_EQ(play::LinkFailure::refused(result.error()).result(),
            remote_play::StreamError::session_init_version_mismatch);
  EXPECT_EQ(channel_.count_frames(0x2030), 3U);
  EXPECT_EQ(channel_.inbox().size(), 4U);
}

TEST_F(RemoteSessionInitRetry, ReportsAnUnknownReasonAfterTheVersionsBounce) {
  channel_.queue_session_init_round(0x5678, unknown_mismatch);
  channel_.queue_session_init_round(0x6678, unknown_back);
  channel_.queue_session_init_round(0x7678, unknown_mismatch);
  console().target = Target::ps4_10;
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, InitError::response_rejected);
  const auto detail = play::LinkFailure::refused(result.error());
  EXPECT_EQ(detail.result(), remote_play::StreamError::session_init_refused);
  EXPECT_EQ(detail.console_reason(), 0x80108bffU);
  EXPECT_EQ(detail.quit().reason,
            remote_play::QuitReason::session_request_unknown);
  EXPECT_EQ(channel_.count_frames(0x2030), 3U);
}

TEST_F(RemoteSessionInitRetry, GivesUpAfterThreeUnansweredRequests) {
  channel_.queue_session_init_round(0x5678, "");
  channel_.inbox().pop_back();
  channel_.inbox().pop_back();
  const auto result = run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, InitError::exchange_failed);
  EXPECT_EQ(clock_.now_microseconds(), 3'000'000U);
  EXPECT_EQ(channel_.count_frames(0x2030), 3U);
}

TEST_F(RemoteSessionInitRetry, WaitsForTheReplyAfterAnAcknowledgement) {
  channel_.queue_session_init_round(0x5678, CtrlFrames::accepted_http);
  channel_.inbox().insert(
      channel_.inbox().begin() + 2,
      ExchangeChannel::packet(
          0x2430, {std::byte{0x12}, std::byte{0x34}, std::byte{0x56},
                   std::byte{0x79}, std::byte{0x00}, std::byte{0x92}}));
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(clock_.now_microseconds(), 0U);
  EXPECT_EQ(channel_.count_frames(0x2030), 2U);
  EXPECT_EQ(channel_.count_frames(0x2430), 1U);
}

TEST_F(RemoteSessionInitRetry, AcknowledgesTheReplyUntilTheConsoleFinishes) {
  channel_.queue_session_init_round(0x5678, CtrlFrames::accepted_http);
  channel_.inbox().pop_back();
  const auto result = run();
  ASSERT_TRUE(result);
  EXPECT_EQ(channel_.count_frames(0x2430), 3U);
  EXPECT_EQ(clock_.now_microseconds(), 3'000'000U);
}

}  // namespace
}  // namespace play_test
