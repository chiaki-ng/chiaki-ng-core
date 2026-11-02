#include "tests/unit/platform/crypto/openssl/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace platform_crypto_test {
namespace {

using libchiaking::platform::hmac_sha1_digest_size;
using libchiaking::platform::sha512_digest_size;

class OpenSslDigests : public OpenSslCryptoTest {
 protected:
  std::array<std::byte, sha512_digest_size> digest{};
  std::array<std::byte, hmac_sha1_digest_size> sha1{};
  std::array<std::byte, 32> short_digest{};
};

TEST_F(OpenSslDigests, Sha512MatchesTheAbcKnownAnswer) {
  ASSERT_TRUE(crypto.sha512(text_bytes("abc"), digest));
  EXPECT_EQ(hex(digest),
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

TEST_F(OpenSslDigests, Sha512RefusesAShortDigest) {
  EXPECT_FALSE(crypto.sha512(text_bytes("abc"), short_digest));
}

TEST_F(OpenSslDigests, HmacSha512MatchesTheQuickBrownFoxKnownAnswer) {
  ASSERT_TRUE(crypto.hmac_sha512(
      text_bytes("key"),
      text_bytes("The quick brown fox jumps over the lazy dog"), digest));
  EXPECT_EQ(hex(digest),
            "b42af09057bac1e2d41708e48a902e09b5ff7f12ab428a4fe86653c73dd248fb"
            "82f948a549f7b791a5b41915ee4d1ec3935357e4e2317250d0372afa2ebeeb3a");
}

TEST_F(OpenSslDigests, HmacSha512AcceptsAnEmptyKeyAndRefusesAShortDigest) {
  EXPECT_TRUE(crypto.hmac_sha512({}, text_bytes("x"), digest));
  EXPECT_FALSE(
      crypto.hmac_sha512(text_bytes("key"), text_bytes("x"), short_digest));
}

TEST_F(OpenSslDigests, HmacSha1MatchesTheQuickBrownFoxKnownAnswer) {
  ASSERT_TRUE(crypto.hmac_sha1(
      text_bytes("key"),
      text_bytes("The quick brown fox jumps over the lazy dog"), sha1));
  EXPECT_EQ(hex(sha1), "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9");
}

TEST_F(OpenSslDigests, HmacSha1MatchesTheRfc2202FirstCase) {
  ASSERT_TRUE(
      crypto.hmac_sha1(unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
                       text_bytes("Hi There"), sha1));
  EXPECT_EQ(hex(sha1), "b617318655057264e28bc0b6fb378c8ef146be00");
}

TEST_F(OpenSslDigests, HmacSha1AcceptsAnEmptyKeyAndRefusesAShortDigest) {
  EXPECT_TRUE(crypto.hmac_sha1({}, text_bytes("x"), sha1));
  EXPECT_FALSE(
      crypto.hmac_sha1(text_bytes("key"), text_bytes("x"), short_digest));
}

}  // namespace
}  // namespace platform_crypto_test
