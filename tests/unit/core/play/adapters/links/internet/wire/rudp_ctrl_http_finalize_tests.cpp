#include "src/core/net/protocol/remote_play/rudp_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using rudp_ctrl::CtrlFrames;
using rudp_ctrl::DecryptCrypto;
using FinalizeError = remote_play::RemoteCtrlHttpFinalizeError;

class RudpCtrlHttpFinalize : public ::testing::Test {
 protected:
  auto finalize(std::string_view reply) {
    return remote_play::RudpCtrl::finalize_http(
        CtrlFrames::exchange_outcome(reply), crypto_,
        remote_play::RegistrationTarget::ps5_1, material_);
  }

  static constexpr std::string_view server_type_reply =
      "HTTP/1.1 200 OK\r\n"
      "RP-Server-Type: AgAAAAAAAAAAAAAAAAAAAA==\r\n\r\nBODY";

  DecryptCrypto crypto_{true};

 private:
  remote_play::AuthMaterial material_;
};

TEST_F(RudpCtrlHttpFinalize, DecryptsTheServerTypeAndKeepsTheTrailingBytes) {
  const auto result = finalize(server_type_reply);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().response.server_type, 2);
  EXPECT_EQ(result.value().response.next_remote_counter, 1U);
  EXPECT_EQ(result.value().next_local_counter, 41);
  EXPECT_EQ(result.value().remote_counter, 7);
  EXPECT_TRUE(std::ranges::equal(result.value().trailing_received,
                                 std::array{std::byte{0x42}, std::byte{0x4F},
                                            std::byte{0x44}, std::byte{0x59}}));
  EXPECT_EQ(crypto_.decrypt_calls(), 1U);
}

TEST_F(RudpCtrlHttpFinalize, LeavesTheServerTypeOutWhenItCannotBeDecrypted) {
  ASSERT_TRUE(finalize(server_type_reply));
  crypto_.fail(true);
  const auto failed_crypto = finalize(server_type_reply);
  ASSERT_TRUE(failed_crypto);
  EXPECT_FALSE(failed_crypto.value().response.server_type);
  EXPECT_EQ(failed_crypto.value().response.next_remote_counter, 1U);
  EXPECT_EQ(crypto_.decrypt_calls(), 2U);
}

TEST_F(RudpCtrlHttpFinalize, AcceptsAReplyWithoutAServerType) {
  const auto result = finalize("HTTP/1.1 200 OK\r\n\r\n");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().response.server_type);
  EXPECT_TRUE(result.value().trailing_received.empty());
}

TEST_F(RudpCtrlHttpFinalize, RefusesAReplyThatIsNotAccepted) {
  const auto result = finalize("HTTP/1.1 403 Forbidden\r\n\r\n");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), FinalizeError::nonaccepted);
}

TEST_F(RudpCtrlHttpFinalize, ReadsAProhibition) {
  const auto result = finalize("HTTP/1.1 200 OK\r\nRP-Prohibit: 1\r\n\r\n");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().response.prohibited);
}

TEST_F(RudpCtrlHttpFinalize, RefusesAReplyThatIsNotHttp) {
  const auto result = finalize("garbage\r\n\r\n");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), FinalizeError::invalid);
}

TEST_F(RudpCtrlHttpFinalize, RefusesAnUnfinishedHeader) {
  const auto result = finalize("HTTP/1.1 200 OK\r\n");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), FinalizeError::incomplete);
}

}  // namespace
}  // namespace play_test
