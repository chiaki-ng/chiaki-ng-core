#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_session_init_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <ranges>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
using rudp_ctrl::CtrlFrames;

class RemoteSessionInit : public rudp_ctrl::SessionInitRig {};

TEST_F(RemoteSessionInit, RunsTheHandshakeAndTheSessionRequestToAcceptance) {
  channel_.queue_session_init_round(0x5678, CtrlFrames::accepted_http);
  std::ranges::copy(CtrlFrames::registration_key,
                    console().registration_key.data());
  const auto result = run();
  ASSERT_TRUE(result);
  for (std::size_t i = 0; i < 16; ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(
        std::to_integer<unsigned>(result.value().response.nonce().data()[i]), i);
  }
  EXPECT_EQ(result.value().completion.local_header, 0xA1B2C3D4U);
  EXPECT_EQ(result.value().completion.next_local_counter, 0x1237);
  EXPECT_EQ(result.value().completion.remote_counter, 0x567B);
  ASSERT_EQ(channel_.sent().size(), 4U);
  EXPECT_EQ(channel_.sent()[0],
            rudp::FrameCodec::encode_init_request(0x1234, 0xA1B2C3D4U).value());
  EXPECT_EQ(channel_.sent()[1],
            rudp::FrameCodec::encode_cookie_request(
                0x1235, 0xA1B2C3D4U,
                std::array<std::byte, 2>{std::byte{0xC0}, std::byte{0x0E}})
                .value());
  const auto request = remote_play::SessionCodec::encode_request(
      console().target, "127.0.0.1", 9295, CtrlFrames::registration_key);
  ASSERT_TRUE(request);
  EXPECT_EQ(channel_.sent()[2], rudp::FrameCodec::encode_session_message(
                                    0x1236, 0x567A, request.value().bytes())
                                    .value());
  EXPECT_EQ(channel_.sent()[3],
            rudp::FrameCodec::encode_ack_message(0x1237, 0x567B).value());
}

TEST_F(RemoteSessionInit, RefusesAnAttemptOfZeroBeforeSending) {
  const auto result = run(0, reference_handshake);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind,
            remote_play::RemoteSessionInitError::invalid_attempt);
  EXPECT_TRUE(channel_.sent().empty());
}

}  // namespace
}  // namespace play_test
