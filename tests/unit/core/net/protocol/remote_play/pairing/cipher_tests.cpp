#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/crypto/error.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
using libchiaking::Expected;
using libchiaking::unexpected;
using libchiaking::net::protocol::remote_play::CipherDirection;
using libchiaking::net::protocol::remote_play::CipherError;
using libchiaking::net::protocol::remote_play::RegistrationCipher;
using libchiaking::net::protocol::remote_play::RegistrationTarget;

class RegistrationCipherXorCrypto final : public platform::CryptoProvider {
 public:
  std::vector<std::byte> last_hmac_key;
  std::vector<std::byte> last_hmac_input;
  bool fail_hmac = false;
  std::size_t aes_call_count = 0;
  bool fail_aes_second_block = false;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Expected<void, platform::CryptoError> override {
    return unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> Expected<void, platform::CryptoError> override {
    if (fail_hmac) return unexpected(platform::CryptoError::backend_failure);
    last_hmac_key.assign(key.begin(), key.end());
    last_hmac_input.assign(input.begin(), input.end());
    for (std::size_t i = 0; i < 32; ++i) digest[i] = static_cast<std::byte>(i);
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte> block,
                            std::span<std::byte> output)
      -> Expected<void, platform::CryptoError> override {
    ++aes_call_count;
    if (fail_aes_second_block && aes_call_count >= 2)
      return unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < 16; ++i) output[i] = block[i] ^ key[i];
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Expected<void, platform::CryptoError> override {
    return unexpected(platform::CryptoError::backend_failure);
  }
};

class RegistrationCipherTransform : public ::testing::Test {
 protected:
  static auto bytes(std::initializer_list<unsigned> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const unsigned value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto prefix(const std::vector<std::byte>& source, std::size_t length)
      -> std::vector<std::byte> {
    return {source.begin(),
            source.begin() + static_cast<std::ptrdiff_t>(length)};
  }

  auto transform(RegistrationTarget target, std::span<const std::byte> key,
                 std::span<const std::byte> ambassador, std::uint64_t counter,
                 CipherDirection direction, std::span<const std::byte> input,
                 std::span<std::byte> output) {
    return RegistrationCipher::transform(crypto, target, key, ambassador,
                                         counter, direction, input, output);
  }

  auto first_eight() const -> std::span<const std::byte> {
    return std::span<const std::byte>(plain).first(8);
  }

  RegistrationCipherXorCrypto crypto;
  const std::array<std::byte, 16> key16 = [] {
    std::array<std::byte, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(i + 1);
    return result;
  }();
  const std::array<std::byte, 16> amb{std::byte{0xaa}};
  const std::vector<std::byte> plain =
      bytes({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
             0xcc, 0xdd, 0xee, 0xff, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
             0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01, 0x02});
  const std::vector<std::byte> golden =
      bytes({0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xa9, 0xba,
             0xcb, 0xdc, 0xed, 0xfe, 0x1f, 0x01, 0x03, 0x01, 0x07, 0x01, 0x03,
             0x01, 0x0f, 0x01, 0x03, 0x01, 0x07, 0x01, 0x03, 0x01, 0x0e, 0x02});
};

// Fixed synthetic mode vector: IV=00..0f, E(block)=block XOR key.
TEST_F(RegistrationCipherTransform, EncryptsAndDecryptsTheSyntheticVector) {
  for (const std::size_t length :
       {std::size_t{0}, std::size_t{1}, std::size_t{16}, std::size_t{17},
        std::size_t{33}}) {
    SCOPED_TRACE(testing::Message() << length << " bytes");
    const auto in = prefix(plain, length);
    std::vector<std::byte> out(length);
    EXPECT_TRUE(transform(RegistrationTarget::ps5_1, key16, amb,
                          0x0102030405060708, CipherDirection::encrypt, in,
                          out));
    EXPECT_EQ(out, prefix(golden, length));
    std::vector<std::byte> back(length);
    EXPECT_TRUE(transform(RegistrationTarget::ps5_1, key16, amb,
                          0x0102030405060708, CipherDirection::decrypt, out,
                          back));
    EXPECT_EQ(back, in);
  }
}

TEST_F(RegistrationCipherTransform, WorksInPlace) {
  auto buffer = plain;
  EXPECT_TRUE(transform(RegistrationTarget::ps4_10, key16, amb, 1,
                        CipherDirection::encrypt, buffer, buffer));
  EXPECT_EQ(buffer, golden);
}

TEST_F(RegistrationCipherTransform, WorksOnOverlappingSpans) {
  std::vector<std::byte> joined(64, std::byte{0x00});
  std::ranges::copy(plain, joined.begin());
  EXPECT_TRUE(transform(RegistrationTarget::ps4_8, key16, amb, 2,
                        CipherDirection::encrypt,
                        std::span<const std::byte>(joined.data(), 32),
                        std::span<std::byte>(joined.data() + 1, 32)));
  EXPECT_TRUE(
      std::ranges::equal(std::span(joined).subspan(1, 32), prefix(golden, 32)));
}

TEST_F(RegistrationCipherTransform, RefusesAShortKey) {
  const std::array<std::byte, 15> bad_key{};
  std::array<std::byte, 8> out{};
  const auto result = transform(RegistrationTarget::ps4_9, bad_key, amb, 0,
                                CipherDirection::encrypt, first_eight(), out);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::invalid_argument);
}

TEST_F(RegistrationCipherTransform, RefusesAShortAmbassador) {
  const std::array<std::byte, 15> bad_amb{};
  std::array<std::byte, 8> out{};
  const auto result = transform(RegistrationTarget::ps4_9, key16, bad_amb, 0,
                                CipherDirection::encrypt, first_eight(), out);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::invalid_argument);
}

TEST_F(RegistrationCipherTransform, RefusesAnOutputOfAnotherSize) {
  std::array<std::byte, 9> out{};
  const auto result = transform(RegistrationTarget::ps4_9, key16, amb, 0,
                                CipherDirection::encrypt, first_eight(), out);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::invalid_argument);
}

TEST_F(RegistrationCipherTransform, HasNoPacketSizeCap) {
  const std::vector<std::byte> huge(1501, std::byte{0});
  std::vector<std::byte> huge_out(1501);
  EXPECT_TRUE(transform(RegistrationTarget::ps4_9, key16, amb, 0,
                        CipherDirection::encrypt, huge, huge_out));
  std::vector<std::byte> huge_back(1501);
  EXPECT_TRUE(transform(RegistrationTarget::ps4_9, key16, amb, 0,
                        CipherDirection::decrypt, huge_out, huge_back));
  EXPECT_EQ(huge_back, huge);
}

TEST_F(RegistrationCipherTransform, RefusesAnUnknownTarget) {
  std::array<std::byte, 8> out{};
  const auto result =
      transform(static_cast<RegistrationTarget>(99), key16, amb, 0,
                CipherDirection::encrypt, first_eight(), out);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::invalid_argument);
}

TEST_F(RegistrationCipherTransform, RefusesAnUnknownDirection) {
  std::array<std::byte, 8> out{};
  const auto result =
      transform(RegistrationTarget::ps4_9, key16, amb, 0,
                static_cast<CipherDirection>(7), first_eight(), out);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::invalid_argument);
}

TEST_F(RegistrationCipherTransform, LeavesTheOutputAloneWhenHmacFails) {
  crypto.fail_hmac = true;
  auto sentinel = plain;
  const auto result = transform(RegistrationTarget::ps5_1, key16, amb, 3,
                                CipherDirection::encrypt, plain, sentinel);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::provider_failure);
  EXPECT_EQ(sentinel, plain);
}

TEST_F(RegistrationCipherTransform, LeavesTheOutputAloneWhenALaterBlockFails) {
  crypto.fail_aes_second_block = true;
  auto sentinel = plain;
  const auto result = transform(RegistrationTarget::ps5_1, key16, amb, 3,
                                CipherDirection::encrypt, plain, sentinel);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), CipherError::provider_failure);
  EXPECT_EQ(sentinel, plain);
}

TEST_F(RegistrationCipherTransform,
       KeysTheHmacWithTheTargetKeyOverAmbassadorAndBigEndianCounter) {
  const auto in8 = prefix(plain, 8);
  std::vector<std::byte> out(8);
  EXPECT_TRUE(transform(RegistrationTarget::ps5_1, key16, amb,
                        0x1122334455667788ULL, CipherDirection::encrypt, in8,
                        out));
  ASSERT_EQ(crypto.last_hmac_key.size(), 16U);
  EXPECT_EQ(crypto.last_hmac_key[0], std::byte{0x46});
  ASSERT_GE(crypto.last_hmac_input.size(), 24U);
  std::vector<std::byte> want(crypto.last_hmac_input.size());
  std::ranges::copy(amb, want.begin());
  const auto counter = bytes({0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
  std::ranges::copy(counter, want.begin() + 16);
  EXPECT_EQ(crypto.last_hmac_input, want);
}

}  // namespace
}  // namespace remote_play_test
