#include "src/core/net/protocol/remote_play/auth_material.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::RegistrationTarget;

static_assert(!std::is_copy_constructible_v<remote_play::AuthMaterial>);
static_assert(std::is_nothrow_move_constructible_v<remote_play::AuthMaterial>);

class AuthMaterialDerive : public ::testing::Test {
 protected:
  void SetUp() override {
    for (std::size_t i = 0; i < 16; ++i) {
      nonce[i] = static_cast<std::byte>(i);
      morning[i] = static_cast<std::byte>(16 + i);
    }
  }

  void use_high_inputs() {
    for (std::size_t i = 0; i < 16; ++i) {
      nonce[i] = std::byte{0xff};
      morning[i] = static_cast<std::byte>(128 + i);
    }
  }

  static auto digit(char c) -> unsigned {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    return static_cast<unsigned>(c - 'a' + 10);
  }

  static auto matches_hex(std::span<const std::byte> actual,
                          std::string_view hex) -> testing::AssertionResult {
    if (actual.size() * 2 != hex.size())
      return testing::AssertionFailure() << actual.size() << " bytes against "
                                         << hex.size() << " hex digits";
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const auto expected = digit(hex[2 * i]) << 4 | digit(hex[2 * i + 1]);
      if (std::to_integer<unsigned>(actual[i]) != expected)
        return testing::AssertionFailure()
               << "byte " << i << " is " << std::to_integer<unsigned>(actual[i])
               << ", not " << expected;
    }
    return testing::AssertionSuccess();
  }

  std::array<std::byte, 16> nonce{};
  std::array<std::byte, 16> morning{};
};

TEST_F(AuthMaterialDerive, MatchesThePreTenPs4Vectors) {
  const auto pre10 = remote_play::AuthMaterial::derive(
      RegistrationTarget::ps4_8, nonce, morning);
  ASSERT_TRUE(pre10);
  EXPECT_TRUE(matches_hex(pre10.value().ambassador,
                          "d8905e42bce052e092e354911ad33688"));
  EXPECT_TRUE(
      matches_hex(pre10.value().bright, "a5a9da7d9dfc4ac6b0435337d87b8a4f"));
  const auto pre10_9 = remote_play::AuthMaterial::derive(
      RegistrationTarget::ps4_9, nonce, morning);
  ASSERT_TRUE(pre10_9);
  EXPECT_TRUE(matches_hex(pre10_9.value().ambassador,
                          "d8905e42bce052e092e354911ad33688"));
}

TEST_F(AuthMaterialDerive, MatchesThePs4TenVectors) {
  const auto ps4 = remote_play::AuthMaterial::derive(RegistrationTarget::ps4_10,
                                                     nonce, morning);
  ASSERT_TRUE(ps4);
  EXPECT_TRUE(
      matches_hex(ps4.value().ambassador, "e978b89a1098296fb32415b9b5713be0"));
  EXPECT_TRUE(
      matches_hex(ps4.value().bright, "8797b0180c9a7c379c8c820fb7501758"));
}

TEST_F(AuthMaterialDerive, MatchesThePs5Vectors) {
  const auto ps5 = remote_play::AuthMaterial::derive(RegistrationTarget::ps5_1,
                                                     nonce, morning);
  ASSERT_TRUE(ps5);
  EXPECT_TRUE(
      matches_hex(ps5.value().ambassador, "ac7d50f394ea6588f0488f76e4170c8a"));
  EXPECT_TRUE(
      matches_hex(ps5.value().bright, "c91f3930c120e8d02bd53a46799b210b"));
}

TEST_F(AuthMaterialDerive, MatchesEveryTargetsVectorsForHighInputs) {
  use_high_inputs();
  const auto pre10 = remote_play::AuthMaterial::derive(
      RegistrationTarget::ps4_9, nonce, morning);
  ASSERT_TRUE(pre10);
  EXPECT_TRUE(matches_hex(pre10.value().ambassador,
                          "d99e514eb1ea59e89bf543850fc12598"));
  EXPECT_TRUE(
      matches_hex(pre10.value().bright, "aaa7d77196f643ceb7455633db798b4f"));
  const auto ps4 = remote_play::AuthMaterial::derive(RegistrationTarget::ps4_10,
                                                     nonce, morning);
  ASSERT_TRUE(ps4);
  EXPECT_TRUE(
      matches_hex(ps4.value().ambassador, "b594d7518513d8743598e3fc299594c0"));
  EXPECT_TRUE(
      matches_hex(ps4.value().bright, "e5e1ed0333de00fdfb8f4972c8ffcc0b"));
  const auto ps5 = remote_play::AuthMaterial::derive(RegistrationTarget::ps5_1,
                                                     nonce, morning);
  ASSERT_TRUE(ps5);
  EXPECT_TRUE(
      matches_hex(ps5.value().ambassador, "d52b1f25d52aa10a3943c3e688a38367"));
  EXPECT_TRUE(
      matches_hex(ps5.value().bright, "7ca873b6cd743e4755d85b1f279e3bbe"));
}

TEST_F(AuthMaterialDerive, RefusesAnUnknownTarget) {
  const auto target = remote_play::AuthMaterial::derive(
      static_cast<RegistrationTarget>(99), nonce, morning);
  ASSERT_FALSE(target);
  EXPECT_EQ(target.error(), remote_play::AuthMaterialError::invalid_target);
}

TEST_F(AuthMaterialDerive, RefusesAShortNonce) {
  const auto short_nonce = remote_play::AuthMaterial::derive(
      RegistrationTarget::ps5_1, std::span(nonce).first(15), morning);
  ASSERT_FALSE(short_nonce);
  EXPECT_EQ(short_nonce.error(), remote_play::AuthMaterialError::invalid_nonce);
}

TEST_F(AuthMaterialDerive, RefusesAShortMorning) {
  const auto short_morning = remote_play::AuthMaterial::derive(
      RegistrationTarget::ps5_1, nonce, std::span(morning).first(15));
  ASSERT_FALSE(short_morning);
  EXPECT_EQ(short_morning.error(),
            remote_play::AuthMaterialError::invalid_morning);
}

}  // namespace
}  // namespace remote_play_test
