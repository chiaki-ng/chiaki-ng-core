#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/crypto/boringssl/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

namespace platform_crypto_test {
namespace {

using libchiaking::Expected;
using libchiaking::platform::CryptoError;
using libchiaking::platform::boringssl::BoringSslCryptoProvider;

class BoringSslOutputGuard : public ::testing::Test {
 protected:
  static constexpr std::byte marker{0x5a};

  template <std::size_t Size>
  [[nodiscard]] static auto marked() -> std::array<std::byte, Size> {
    std::array<std::byte, Size> bytes{};
    bytes.fill(marker);
    return bytes;
  }

  [[nodiscard]] static auto rejected(const Expected<void, CryptoError>& result)
      -> ::testing::AssertionResult {
    if (result.has_value())
      return ::testing::AssertionFailure() << "the call succeeded";
    if (result.error() != CryptoError::invalid_argument)
      return ::testing::AssertionFailure()
             << "failed with " << static_cast<int>(result.error())
             << " rather than invalid_argument";
    return ::testing::AssertionSuccess();
  }

  BoringSslCryptoProvider provider;
  const std::array<std::byte, 32> input{};
  const std::array<std::byte, 16> key{};
  const std::array<std::byte, 16> block{};
  const std::array<std::byte, 12> iv{};
  std::array<std::byte, 32> sha_output = marked<32>();
  std::array<std::byte, 32> hmac_output = marked<32>();
  std::array<std::byte, 16> block_output = marked<16>();
  std::array<std::byte, 16> tag_output = marked<16>();
};

TEST_F(BoringSslOutputGuard, RefusesAShortSha256Digest) {
  EXPECT_TRUE(rejected(
      provider.sha256(input, std::span<std::byte>(sha_output).first(31))));
  EXPECT_EQ(sha_output, marked<32>());
}

TEST_F(BoringSslOutputGuard, RefusesAShortHmacSha256Digest) {
  EXPECT_TRUE(rejected(provider.hmac_sha256(
      key, input, std::span<std::byte>(hmac_output).first(31))));
  EXPECT_EQ(hmac_output, marked<32>());
}

TEST_F(BoringSslOutputGuard, RefusesAShortAesBlock) {
  EXPECT_TRUE(rejected(provider.aes128_encrypt_block(
      key, block, std::span<std::byte>(block_output).first(15))));
  EXPECT_EQ(block_output, marked<16>());
}

TEST_F(BoringSslOutputGuard, RefusesAGmacTagBelowTheMinimum) {
  EXPECT_TRUE(rejected(provider.aes128_gmac(
      key, iv, {}, std::span<std::byte>(tag_output).first(3))));
  EXPECT_EQ(tag_output, marked<16>());
}

}  // namespace
}  // namespace platform_crypto_test
