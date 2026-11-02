#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
using rudp_ctrl::SpyCrypto;
using OutboundError = remote_play::RudpCtrlOutboundError;

class RudpCtrlOutbound : public ::testing::Test {
 protected:
  auto encode(std::uint16_t type, std::span<const std::byte> payload,
              std::uint64_t crypt_counter, std::uint16_t rudp_counter) {
    return remote_play::RudpCtrl::encode_outbound(
        crypto_, remote_play::RegistrationTarget::ps5_1, material_, type,
        payload, crypt_counter, rudp_counter);
  }

  auto crypto() const -> const SpyCrypto& { return crypto_; }

  static constexpr std::array<std::byte, 1> pin{std::byte{0x42}};

 private:
  SpyCrypto crypto_;
  remote_play::AuthMaterial material_;
};

TEST_F(RudpCtrlOutbound, SendsAHeartbeatReplyInTheClear) {
  const auto heartbeat = encode(0x01fe, {}, 5, 0x1234);
  ASSERT_TRUE(heartbeat);
  EXPECT_EQ(heartbeat.value().next_crypt_counter, 5U);
  EXPECT_EQ(heartbeat.value().next_rudp_counter, 0x1235);
  EXPECT_EQ(crypto().calls(), 0U);
  const auto frames = rudp::FrameCodec::decode(heartbeat.value().bytes);
  ASSERT_TRUE(frames);
  ASSERT_EQ(frames.value().size(), 1U);
  EXPECT_EQ(frames.value()[0].type, 0x0230);
  ASSERT_EQ(frames.value()[0].payload.size(), 10U);
  EXPECT_EQ(frames.value()[0].payload[0], std::byte{0x12});
  EXPECT_EQ(frames.value()[0].payload[1], std::byte{0x34});
  const auto ctrl = remote_play::CtrlCodec::decode_frame(
      std::span<const std::byte>(frames.value()[0].payload).subspan(2));
  ASSERT_TRUE(ctrl);
  EXPECT_EQ(ctrl.value().type, 0x01fe);
  EXPECT_TRUE(ctrl.value().payload.empty());
}

TEST_F(RudpCtrlOutbound, SealsAPinWithTheCounterBeforeItsCryptCounter) {
  const auto pin_zero = encode(0x8004, pin, 0, 0xffff);
  ASSERT_TRUE(pin_zero);
  EXPECT_EQ(pin_zero.value().next_crypt_counter, 1U);
  EXPECT_EQ(pin_zero.value().next_rudp_counter, 0);
  for (const auto byte : crypto().counter()) EXPECT_EQ(byte, std::byte{0xff});
  const auto pin_one = encode(0x8004, pin, 1, 7);
  ASSERT_TRUE(pin_one);
  EXPECT_EQ(pin_one.value().next_crypt_counter, 2U);
  for (const auto byte : crypto().counter()) EXPECT_EQ(byte, std::byte{0});
}

TEST_F(RudpCtrlOutbound, SealsAnOrdinaryFrameWithItsOwnCounter) {
  const auto ordinary = encode(0x0050, pin, 5, 7);
  ASSERT_TRUE(ordinary);
  EXPECT_EQ(ordinary.value().next_crypt_counter, 6U);
  EXPECT_EQ(crypto().counter().back(), std::byte{5});
}

TEST_F(RudpCtrlOutbound, RefusesAnExhaustedCounter) {
  const auto exhausted =
      encode(0x8004, pin, std::numeric_limits<std::uint64_t>::max(), 7);
  ASSERT_FALSE(exhausted);
  EXPECT_EQ(exhausted.error(), OutboundError::counter_exhausted);
}

TEST_F(RudpCtrlOutbound, KeepsALargeKeyboardFrameInOneDatagram) {
  const std::vector<std::byte> keyboard(578, std::byte{0x61});
  const auto large = encode(0x0023, keyboard, 0, 7);
  ASSERT_TRUE(large);
  const auto large_frames = rudp::FrameCodec::decode(large.value().bytes);
  ASSERT_TRUE(large_frames);
  ASSERT_EQ(large_frames.value().size(), 1U);
  ASSERT_EQ(large_frames.value().front().payload.size(),
            2 + 8 + keyboard.size());
  EXPECT_EQ(large_frames.value().front().payload[4], std::byte{0x02});
  EXPECT_EQ(large_frames.value().front().payload[5], std::byte{0x42});
}

TEST_F(RudpCtrlOutbound, RefusesAFrameLargerThanADatagramHolds) {
  const std::vector<std::byte> fits(4087 - 10);
  EXPECT_TRUE(encode(0x0023, fits, 0, 7));
  const std::vector<std::byte> huge(4087 - 9);
  const auto too_large = encode(0x8004, huge, 0, 7);
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), OutboundError::too_large);
}

}  // namespace
}  // namespace play_test
