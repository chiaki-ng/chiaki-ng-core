#include "tests/support/enum_printing.hpp"
#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::aes128_gcm_iv_size_max;
using libchiaking::platform::aes128_gcm_iv_size_min;
using libchiaking::platform::aes128_gcm_tag_size;
using libchiaking::platform::aes128_gcm_tag_size_max;
using libchiaking::platform::aes128_gcm_tag_size_min;
using libchiaking::platform::aes128_key_size;
using libchiaking::platform::CryptoError;

class OpenSslGmacBounds : public OpenSslCryptoTest {
 protected:
  [[nodiscard]] static auto accepted(std::size_t iv_size, std::size_t tag_size)
      -> bool {
    return iv_size >= aes128_gcm_iv_size_min &&
           iv_size <= aes128_gcm_iv_size_max &&
           tag_size >= aes128_gcm_tag_size_min &&
           tag_size <= aes128_gcm_tag_size_max;
  }

  const std::array<std::byte, aes128_key_size> key{};
  const std::array<std::byte, aes128_gcm_iv_size_max + 2> iv{};
  std::array<std::byte, aes128_gcm_tag_size> tag{};
};

TEST_F(OpenSslGmacBounds, AcceptsExactlyTheSupportedIvAndTagSizes) {
  for (std::size_t iv_size = 0; iv_size <= iv.size(); ++iv_size) {
    for (std::size_t tag_size = 0; tag_size <= tag.size(); ++tag_size) {
      const auto result =
          crypto.aes128_gmac(key, std::span<const std::byte>(iv).first(iv_size),
                             {}, std::span<std::byte>(tag).first(tag_size));
      const bool expected = accepted(iv_size, tag_size);
      EXPECT_EQ(result.has_value(), expected)
          << "iv " << iv_size << " tag " << tag_size;
      if (!expected && !result.has_value()) {
        EXPECT_EQ(result.error(), CryptoError::invalid_argument)
            << "iv " << iv_size << " tag " << tag_size;
      }
    }
  }
}

}  // namespace
}  // namespace platform_crypto_test
