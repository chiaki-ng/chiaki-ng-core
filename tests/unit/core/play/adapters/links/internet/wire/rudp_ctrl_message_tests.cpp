#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using rudp_ctrl::CtrlFrames;
using rudp_ctrl::DecryptCrypto;
using Target = remote_play::RegistrationTarget;
using MessageError = remote_play::RudpCtrlMessageError;

class RudpCtrlMessage : public ::testing::Test {
 protected:
  auto decode(std::uint64_t counter, const remote_play::RudpOngoingCtrl& in) {
    return remote_play::RudpCtrl::decode_message(crypto_, Target::ps5_1,
                                                 material_, counter, in);
  }

  auto crypto() -> DecryptCrypto& { return crypto_; }

  static auto encrypted_frame() -> remote_play::RudpOngoingCtrl {
    const std::array<std::byte, 1> encrypted{std::byte{1}};
    return CtrlFrames::ongoing(0x05, encrypted);
  }

 private:
  DecryptCrypto crypto_;
  remote_play::AuthMaterial material_;
};

TEST_F(RudpCtrlMessage, PassesAStreamSwitchAckThroughWithoutDecrypting) {
  const auto result = decode(100, CtrlFrames::ongoing(0x34, {}));
  ASSERT_TRUE(result);
  EXPECT_TRUE(std::holds_alternative<remote_play::CtrlStreamSwitchAck>(
      result.value().message));
  EXPECT_EQ(result.value().next_remote_crypt_counter, 100U);
  EXPECT_EQ(result.value().remote_counter, 41);
  EXPECT_EQ(result.value().ack_counter, 12);
  EXPECT_EQ(crypto().decrypt_calls(), 0U);
}

TEST_F(RudpCtrlMessage, DecryptsAnEncryptedFrameWithTheNextCounter) {
  const auto result = decode(7, encrypted_frame());
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().next_remote_crypt_counter, 8U);
  EXPECT_EQ(crypto().decrypt_calls(), 1U);
}

TEST_F(RudpCtrlMessage, ReportsACryptoFailure) {
  ASSERT_TRUE(decode(7, encrypted_frame()));
  crypto().fail(true);
  const auto failed = decode(7, encrypted_frame());
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), MessageError::crypto_failure);
}

TEST_F(RudpCtrlMessage, RefusesAnExhaustedCounter) {
  const auto overflow =
      decode(std::numeric_limits<std::uint64_t>::max(), encrypted_frame());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), MessageError::counter_exhausted);
}

TEST_F(RudpCtrlMessage, RefusesAMalformedFrame) {
  remote_play::RudpOngoingCtrl in{};
  in.ctrl_frame = {std::byte{0}, std::byte{0}};
  const auto result = decode(0, in);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), MessageError::malformed);
}

}  // namespace
}  // namespace play_test
