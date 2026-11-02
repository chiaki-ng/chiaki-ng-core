#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/utils/text/hex.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace remote_play_test {
namespace {

using libchiaking::net::protocol::remote_play::CipherDirection;
using libchiaking::net::protocol::remote_play::RegistrationCipher;
using libchiaking::net::protocol::remote_play::RegistrationMaterial;
using libchiaking::net::protocol::remote_play::RegistrationTarget;
using libchiaking::platform::openssl::OpenSslCryptoProvider;

struct OpenSslRegistrationCipherCase {
  RegistrationTarget target;
  std::string_view cipher0;
  std::string_view cipher_ctr;
};

// Golden vectors from isolated mirror rpcrypt.c functions, synthetic ambassador
// 00..0f and PIN 0x01020304, fixed local registration prefix offsets 1 and 8.
class OpenSslRegistrationCipher
    : public ::testing::TestWithParam<OpenSslRegistrationCipherCase> {
 protected:
  static auto golden(std::string_view hex) -> std::vector<std::byte> {
    std::vector<std::byte> out(33);
    EXPECT_TRUE(libchiaking::text::decode_hex(hex, out)) << hex;
    return out;
  }

  static auto prefix(const std::vector<std::byte>& source, std::size_t length)
      -> std::vector<std::byte> {
    return {source.begin(),
            source.begin() + static_cast<std::ptrdiff_t>(length)};
  }

  void expect_golden(std::uint64_t counter, std::string_view hex) {
    constexpr std::uint32_t pin = 0x01020304U;
    std::array<std::byte, 16> ambassador{};
    for (std::size_t i = 0; i < ambassador.size(); ++i)
      ambassador[i] = static_cast<std::byte>(i);
    std::vector<std::byte> plain;
    for (unsigned i = 1; i <= 33U; ++i)
      plain.push_back(static_cast<std::byte>(i));
    const auto target = GetParam().target;
    const auto material =
        RegistrationMaterial::derive_local(target, ambassador, pin);
    ASSERT_TRUE(material);
    const auto expected = golden(hex);
    for (const std::size_t length :
         {std::size_t{0U}, std::size_t{1U}, std::size_t{15U}, std::size_t{16U},
          std::size_t{17U}, std::size_t{32U}, std::size_t{33U}}) {
      SCOPED_TRACE(testing::Message() << length << " bytes");
      std::vector<std::byte> out(length, std::byte{0});
      EXPECT_TRUE(RegistrationCipher::transform(
          provider, target, material.value().bright,
          material.value().ambassador, counter, CipherDirection::encrypt,
          std::span<const std::byte>(plain).first(length), out));
      EXPECT_EQ(out, prefix(expected, length));
      auto buffer = prefix(expected, length);
      EXPECT_TRUE(RegistrationCipher::transform(
          provider, target, material.value().bright,
          material.value().ambassador, counter, CipherDirection::decrypt,
          std::span<const std::byte>(buffer), buffer));
      EXPECT_EQ(buffer, prefix(plain, length));
    }
  }

  OpenSslCryptoProvider provider{};
};

TEST_P(OpenSslRegistrationCipher, MatchesTheReferenceAtCounterZero) {
  ASSERT_NO_FATAL_FAILURE(expect_golden(0U, GetParam().cipher0));
}

TEST_P(OpenSslRegistrationCipher, MatchesTheReferenceAtALargeCounter) {
  ASSERT_NO_FATAL_FAILURE(
      expect_golden(0x0102030405060708ULL, GetParam().cipher_ctr));
}

INSTANTIATE_TEST_SUITE_P(
    EveryTarget, OpenSslRegistrationCipher,
    ::testing::Values(
        OpenSslRegistrationCipherCase{RegistrationTarget::ps4_8,
                                      "602ed9c1581daf403e75ee9fb9e6577c33d56cdb"
                                      "75f0681d106268552d81e26ac5",
                                      "b82fb001d6c9d457645e57ee79724c265c0c94a9"
                                      "983de8ecf82ebe32f3816c0501"},
        OpenSslRegistrationCipherCase{RegistrationTarget::ps4_9,
                                      "602ed9c1581daf403e75ee9fb9e6577c33d56cdb"
                                      "75f0681d106268552d81e26ac5",
                                      "b82fb001d6c9d457645e57ee79724c265c0c94a9"
                                      "983de8ecf82ebe32f3816c0501"},
        OpenSslRegistrationCipherCase{RegistrationTarget::ps4_10,
                                      "27aa7f90cb5e2ac3f07360d74e2c599213eff25d"
                                      "2ce819fff277bdc964c6499fd4",
                                      "772d7c611e0894a39b6c8e4152960a58689ab3bc"
                                      "acb4f772882af931579bfd26d4"},
        OpenSslRegistrationCipherCase{RegistrationTarget::ps5_1,
                                      "30d1e804fbde25481682c60ec376f7983c9d914f"
                                      "d09f42cbeabe62709b8827bf76",
                                      "23b2b6bf0f09533461b6a5b23c2a5268dca4a3ed"
                                      "4dab0c6745f724a075344f6eaa"}));

}  // namespace
}  // namespace remote_play_test
