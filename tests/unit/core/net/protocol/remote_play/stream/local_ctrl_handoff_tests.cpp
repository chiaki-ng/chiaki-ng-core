#include "tests/unit/core/net/protocol/remote_play/stream/local_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

static_assert(!std::is_copy_constructible_v<remote_play::LocalCtrlHandoff>);

class LocalCtrlHandoffAdoption : public LocalCtrlRig {
 protected:
  auto connection() -> remote_play::CtrlStartupConnection {
    remote_play::CtrlStartupConnection value;
    value.socket = std::make_unique<LocalCtrlSocket>(network, closes);
    return value;
  }

  static auto grant(std::string_view reply) -> remote_play::CtrlGrant {
    auto parsed = remote_play::CtrlCodec::parse_response(
        {reinterpret_cast<const std::byte*>(reply.data()), reply.size()});
    EXPECT_TRUE(parsed && parsed.value());
    return std::move(parsed.value().value());
  }

  static constexpr std::string_view typed_reply =
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
      "RP-Prohibit: 1\r\n\r\n";
  static constexpr std::string_view unprohibited_reply =
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAAAAAAAAAAAAAAAAAAAAA==\r\n\r\n";

  libchiaking::platform::DefaultNetworkProvider network{threading(), steady()};
  LocalCtrlCrypto crypto;
  unsigned closes{};
};

TEST_F(LocalCtrlHandoffAdoption, OwnsTheAcceptedConnectionUntilItIsDropped) {
  auto accepted = connection();
  accepted.response = grant(typed_reply);
  accepted.next_local_crypt_counter = 5;
  {
    const auto result = remote_play::LocalCtrlHandoff::adopt(
        crypto, std::move(accepted), remote_play::RegistrationTarget::ps5_1,
        remote_play::AuthMaterial{}, 42);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().owner);
    EXPECT_EQ(result.value().owner->state(),
              remote_play::ctrl_lifecycle_detail::State::waiting_id);
    EXPECT_EQ(result.value().response.server_type, 0);
    EXPECT_TRUE(result.value().response.prohibited);
    EXPECT_EQ(result.value().response.next_remote_counter, 1U);
    EXPECT_EQ(crypto.hmac_calls, 1U);
    EXPECT_EQ(closes, 0U);
  }
  EXPECT_EQ(closes, 1U);
}

TEST_F(LocalCtrlHandoffAdoption,
       AdvancesTheCounterWhenTheServerTypeCannotBeDecrypted) {
  crypto.fail_hmac = true;
  auto accepted = connection();
  accepted.response = grant(unprohibited_reply);
  {
    const auto result = remote_play::LocalCtrlHandoff::adopt(
        crypto, std::move(accepted), remote_play::RegistrationTarget::ps5_1,
        remote_play::AuthMaterial{}, 1);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().owner);
    ASSERT_FALSE(result.value().response.server_type);
    EXPECT_EQ(result.value().response.server_type.error(),
              remote_play::ServerTypeError::undecryptable);
    EXPECT_EQ(result.value().response.next_remote_counter, 1U);
    EXPECT_EQ(closes, 0U);
    EXPECT_EQ(crypto.hmac_calls, 1U);
  }
  EXPECT_EQ(closes, 1U);
}

}  // namespace
}  // namespace remote_play_test
