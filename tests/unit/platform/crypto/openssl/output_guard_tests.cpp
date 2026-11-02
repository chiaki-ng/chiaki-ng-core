#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <openssl/err.h>

#include <array>
#include <cstddef>
#include <span>

namespace platform_crypto_test {
namespace {

using libchiaking::Expected;
using libchiaking::platform::aes128_block_size;
using libchiaking::platform::aes128_gcm_iv_size_min;
using libchiaking::platform::aes128_gcm_tag_size;
using libchiaking::platform::aes128_gcm_tag_size_min;
using libchiaking::platform::aes128_key_size;
using libchiaking::platform::CryptoError;
using libchiaking::platform::hmac_sha256_digest_size;
using libchiaking::platform::sha256_digest_size;

class OpenSslOutputGuard : public OpenSslCryptoTest {
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

  void refuse_every_short_output() {
    EXPECT_TRUE(rejected(crypto.sha256(
        sha_input,
        std::span<std::byte>(sha_output).first(sha256_digest_size - 1))));
    EXPECT_TRUE(rejected(crypto.hmac_sha256(
        key, sha_input,
        std::span<std::byte>(hmac_output).first(hmac_sha256_digest_size - 1))));
    EXPECT_TRUE(rejected(crypto.aes128_encrypt_block(
        key, block,
        std::span<std::byte>(block_output).first(aes128_block_size - 1))));
    EXPECT_TRUE(rejected(crypto.aes128_gmac(
        key, iv, {},
        std::span<std::byte>(tag_output).first(aes128_gcm_tag_size_min - 1))));
  }

  const std::array<std::byte, sha256_digest_size> sha_input{};
  const std::array<std::byte, aes128_key_size> key{};
  const std::array<std::byte, aes128_block_size> block{};
  const std::array<std::byte, aes128_gcm_iv_size_min> iv{};
  std::array<std::byte, sha256_digest_size> sha_output =
      marked<sha256_digest_size>();
  std::array<std::byte, hmac_sha256_digest_size> hmac_output =
      marked<hmac_sha256_digest_size>();
  std::array<std::byte, aes128_block_size> block_output =
      marked<aes128_block_size>();
  std::array<std::byte, aes128_gcm_tag_size> tag_output =
      marked<aes128_gcm_tag_size>();
};

TEST_F(OpenSslOutputGuard, RefusesShortOutputsWithoutWritingThem) {
  refuse_every_short_output();
  EXPECT_EQ(sha_output, marked<sha256_digest_size>());
  EXPECT_EQ(hmac_output, marked<hmac_sha256_digest_size>());
  EXPECT_EQ(block_output, marked<aes128_block_size>());
  EXPECT_EQ(tag_output, marked<aes128_gcm_tag_size>());
}

TEST_F(OpenSslOutputGuard, KeepsTheCallersPendingErrorThroughRefusals) {
  ERR_raise(ERR_LIB_NONE, ERR_R_INTERNAL_ERROR);
  const unsigned long caller_error = ERR_peek_last_error();
  refuse_every_short_output();
  EXPECT_EQ(ERR_peek_last_error(), caller_error);
  EXPECT_TRUE(crypto.sha256(sha_input, sha_output).has_value());
  EXPECT_EQ(ERR_peek_last_error(), caller_error);
  ERR_clear_error();
}

TEST_F(OpenSslOutputGuard, LeavesAClearErrorQueueClearAfterSuccess) {
  ERR_clear_error();
  EXPECT_TRUE(crypto.sha256(sha_input, sha_output).has_value());
  EXPECT_EQ(ERR_peek_last_error(), 0UL);
}

}  // namespace
}  // namespace platform_crypto_test
