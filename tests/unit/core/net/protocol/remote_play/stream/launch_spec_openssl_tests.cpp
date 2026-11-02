#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "src/core/net/protocol/remote_play/stream/launch_spec.hpp"
#include "src/core/utils/text/hex.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace text = libchiaking::text;

class OpenSslLaunchSpec : public ::testing::Test {
 protected:
  using Digest = std::array<std::byte, 32>;

  auto encoded_digest(remote_play::RegistrationTarget target,
                      remote_play::VideoCodec codec,
                      std::size_t expected_length) -> std::optional<Digest> {
    remote_play::AuthMaterial auth;
    std::array<std::byte, 16> key{};
    for (std::size_t i = 0; i < 16; ++i) {
      auth.bright[i] = static_cast<std::byte>(0x20 + i);
      auth.ambassador[i] = static_cast<std::byte>(0x40 + i);
      key[i] = static_cast<std::byte>(i);
    }
    const auto encoded = remote_play::LaunchSpecCodec::encode(
        crypto, target, codec, {1280, 720, 60, 10000}, 1344, 2005, key, auth);
    EXPECT_TRUE(encoded);
    if (!encoded) return std::nullopt;
    EXPECT_EQ(encoded.value().size(), expected_length);
    Digest digest{};
    const auto hashed =
        crypto.sha256(std::as_bytes(std::span<const char>{
                          encoded.value().data(), encoded.value().size()}),
                      digest);
    EXPECT_TRUE(hashed);
    if (!hashed) return std::nullopt;
    return digest;
  }

  platform::openssl::OpenSslCryptoProvider crypto;
};

// Independently generated with HMAC-derived IV and OpenSSL CLI CFB128:
// encrypt zeros, XOR reference JSON including NUL, Base64, then SHA256.
TEST_F(OpenSslLaunchSpec, EncryptsAPs4H264SpecLikeTheReference) {
  const auto digest = encoded_digest(remote_play::RegistrationTarget::ps4_9,
                                     remote_play::VideoCodec::h264, 1116);
  ASSERT_TRUE(digest);
  EXPECT_EQ(*digest,
            text::hex_bytes<32>("b66ad114972574dbadb3ade6be23be09bda00ee66bf5c1"
                                "029db232f49baad245"));
}

TEST_F(OpenSslLaunchSpec, EncryptsAPs5HdrHevcSpecLikeTheReference) {
  const auto digest = encoded_digest(remote_play::RegistrationTarget::ps5_1,
                                     remote_play::VideoCodec::h265_hdr, 1212);
  ASSERT_TRUE(digest);
  EXPECT_EQ(*digest,
            text::hex_bytes<32>("83df73ae009bdf6177e486ded7ca85f510370c6d616438"
                                "bb7517f98d5199d112"));
}

}  // namespace
}  // namespace remote_play_test
