#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include "src/core/net/protocol/takion/takion.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

using libchiaking::unexpected;
using libchiaking::net::protocol::takion::CryptoError;
using libchiaking::net::protocol::takion::CryptoMaterial;
using libchiaking::net::protocol::takion::PlatformTakionCryptoProvider;
using libchiaking::net::protocol::takion::SecretBytes;
using libchiaking::platform::openssl::OpenSslCryptoProvider;
using PlatformCryptoError = libchiaking::platform::CryptoError;
using PlatformResult = libchiaking::Expected<void, PlatformCryptoError>;

enum class PlatformCryptoFailureStage {
  hmac,
  block,
  gmac,
  base_sha256,
};

class FailingPlatformCryptoProvider final
    : public libchiaking::platform::CryptoProvider {
 public:
  explicit FailingPlatformCryptoProvider(PlatformCryptoFailureStage stage)
      : stage_(stage) {}

  auto sha256(std::span<const std::byte> input, std::span<std::byte> digest)
      -> PlatformResult override {
    if (stage_ == PlatformCryptoFailureStage::base_sha256)
      return unexpected(PlatformCryptoError::backend_failure);
    return underlying_.sha256(input, digest);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest) -> PlatformResult override {
    if (stage_ == PlatformCryptoFailureStage::hmac)
      return unexpected(PlatformCryptoError::backend_failure);
    return underlying_.hmac_sha256(key, input, digest);
  }

  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte> block,
                            std::span<std::byte> output)
      -> PlatformResult override {
    if (stage_ == PlatformCryptoFailureStage::block)
      return unexpected(PlatformCryptoError::backend_failure);
    return underlying_.aes128_encrypt_block(key, block, output);
  }

  auto aes128_gmac(std::span<const std::byte> key,
                   std::span<const std::byte> iv,
                   std::span<const std::byte> aad, std::span<std::byte> tag)
      -> PlatformResult override {
    if (stage_ == PlatformCryptoFailureStage::gmac)
      return unexpected(PlatformCryptoError::backend_failure);
    return underlying_.aes128_gmac(key, iv, aad, tag);
  }

 private:
  OpenSslCryptoProvider underlying_;
  PlatformCryptoFailureStage stage_;
};

class CountingPlatformCryptoProvider final
    : public libchiaking::platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte> input, std::span<std::byte> digest)
      -> PlatformResult override {
    ++sha256_calls_;
    return underlying_.sha256(input, digest);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest) -> PlatformResult override {
    return underlying_.hmac_sha256(key, input, digest);
  }

  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte> block,
                            std::span<std::byte> output)
      -> PlatformResult override {
    return underlying_.aes128_encrypt_block(key, block, output);
  }

  auto aes128_gmac(std::span<const std::byte> key,
                   std::span<const std::byte> iv,
                   std::span<const std::byte> aad, std::span<std::byte> tag)
      -> PlatformResult override {
    ++gmac_calls_;
    gmac_iv_sizes_.push_back(iv.size());
    gmac_tag_sizes_.push_back(tag.size());
    return underlying_.aes128_gmac(key, iv, aad, tag);
  }

  [[nodiscard]] auto sha256_calls() const -> std::size_t {
    return sha256_calls_;
  }
  [[nodiscard]] auto gmac_calls() const -> std::size_t { return gmac_calls_; }
  [[nodiscard]] auto gmac_iv_sizes() const -> const std::vector<std::size_t>& {
    return gmac_iv_sizes_;
  }
  [[nodiscard]] auto gmac_tag_sizes() const -> const std::vector<std::size_t>& {
    return gmac_tag_sizes_;
  }

 private:
  OpenSslCryptoProvider underlying_;
  std::size_t sha256_calls_{0};
  std::size_t gmac_calls_{0};
  std::vector<std::size_t> gmac_iv_sizes_;
  std::vector<std::size_t> gmac_tag_sizes_;
};

class PlatformTakionCryptoProviderTest : public ::testing::Test {
 protected:
  static constexpr std::uint64_t high_position =
      (std::uint64_t{1} << 32U) + 0x420U;

  static auto make_secret(std::span<const std::uint8_t> bytes) -> SecretBytes {
    return SecretBytes(bytes_of(bytes));
  }

  static auto material(std::span<const std::uint8_t> handshake_key,
                       std::span<const std::uint8_t> ecdh_secret,
                       std::uint8_t index) -> CryptoMaterial {
    return CryptoMaterial(make_secret(handshake_key), make_secret(ecdh_secret),
                          index);
  }

  static auto bytes_of(std::span<const std::uint8_t> input)
      -> std::vector<std::byte> {
    std::vector<std::byte> output(input.size());
    std::ranges::transform(input, output.begin(), [](std::uint8_t value) {
      return static_cast<std::byte>(value);
    });
    return output;
  }

  template <std::size_t Size>
  static auto to_bytes(const std::array<std::uint8_t, Size>& input)
      -> std::array<std::byte, Size> {
    std::array<std::byte, Size> output{};
    std::ranges::transform(input, output.begin(), [](std::uint8_t value) {
      return static_cast<std::byte>(value);
    });
    return output;
  }

  static auto equal_bytes(std::span<const std::byte> left,
                          std::span<const std::uint8_t> right)
      -> testing::AssertionResult {
    if (std::ranges::equal(left, bytes_of(right)))
      return testing::AssertionSuccess();
    return testing::AssertionFailure() << "the bytes differ";
  }

  libchiaking::platform::DefaultThreadingProvider& threading =
      test_support::process_threading();
  OpenSslCryptoProvider provider;
};

TEST_F(PlatformTakionCryptoProviderTest,
       ReceiveOnlyKeysAuthenticateAndDecryptLikeTheFullDirection) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
      0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
      0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  const auto keys =
      adapter.export_receive_keys(material(handshake_key, ecdh_secret, 3));
  ASSERT_TRUE(keys);
  const auto outgoing =
      adapter.export_receive_keys(material(handshake_key, ecdh_secret, 2));
  ASSERT_TRUE(outgoing);
  EXPECT_FALSE(std::ranges::equal(keys.value().key(), outgoing.value().key()));
  EXPECT_FALSE(std::ranges::equal(keys.value().iv(), outgoing.value().iv()));

  auto full = adapter.create_remote(material(handshake_key, ecdh_secret, 3));
  auto receive_only = adapter.create_receive_only(keys.value());
  ASSERT_TRUE(full);
  ASSERT_TRUE(receive_only);

  std::array<std::byte, 53> packet{};
  for (std::size_t index = 0; index < packet.size(); ++index)
    packet[index] = static_cast<std::byte>(0x40U + index);
  for (const std::uint64_t position :
       {std::uint64_t{0}, std::uint64_t{17}, std::uint64_t{44999},
        std::uint64_t{45000}, std::uint64_t{45001}, std::uint64_t{90001},
        (std::uint64_t{1} << 32U) + 5}) {
    const auto expected_mac = full.value()->authenticate(position, packet);
    const auto mac = receive_only.value()->authenticate(position, packet);
    ASSERT_TRUE(expected_mac) << "position " << position;
    ASSERT_TRUE(mac) << "position " << position;
    EXPECT_EQ(mac.value(), expected_mac.value()) << "position " << position;

    auto expected_plain = packet;
    auto plain = packet;
    EXPECT_TRUE(
        full.value()->apply_keystream(position, expected_plain, expected_plain))
        << "position " << position;
    EXPECT_TRUE(receive_only.value()->apply_keystream(position, plain, plain))
        << "position " << position;
    EXPECT_EQ(plain, expected_plain) << "position " << position;
    EXPECT_NE(plain, packet) << "position " << position;
  }
}

TEST_F(PlatformTakionCryptoProviderTest,
       ReceiveOnlyKeysWithAnAlteredIvAuthenticateDifferently) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
      0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
      0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  const auto keys =
      adapter.export_receive_keys(material(handshake_key, ecdh_secret, 3));
  ASSERT_TRUE(keys);
  auto full = adapter.create_remote(material(handshake_key, ecdh_secret, 3));
  ASSERT_TRUE(full);
  std::array<std::byte, 53> packet{};
  for (std::size_t index = 0; index < packet.size(); ++index)
    packet[index] = static_cast<std::byte>(0x40U + index);

  libchiaking::SecretArray<libchiaking::net::protocol::takion::source_key_size>
      altered_key;
  libchiaking::SecretArray<libchiaking::net::protocol::takion::source_key_size>
      altered_iv;
  std::ranges::copy(keys.value().key(), altered_key.begin());
  std::ranges::copy(keys.value().iv(), altered_iv.begin());
  altered_iv[0] ^= std::byte{1};
  libchiaking::net::protocol::takion::SourceReceiveKeys altered{
      std::move(altered_key), std::move(altered_iv)};
  auto mismatched = adapter.create_receive_only(altered);
  ASSERT_TRUE(mismatched);
  EXPECT_NE(mismatched.value()->authenticate(17, packet).value(),
            full.value()->authenticate(17, packet).value());
}

TEST_F(PlatformTakionCryptoProviderTest,
       ExportingReceiveKeysNeedsTypedMaterialAndAWorkingProvider) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
      0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
      0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  const auto invalid =
      adapter.export_receive_keys(CryptoMaterial(make_secret(handshake_key)));
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), CryptoError::invalid_key_material);

  FailingPlatformCryptoProvider failing(PlatformCryptoFailureStage::hmac);
  PlatformTakionCryptoProvider failing_adapter(failing, threading);
  const auto failed = failing_adapter.export_receive_keys(
      material(handshake_key, ecdh_secret, 3));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), CryptoError::provider_failure);
}

TEST_F(PlatformTakionCryptoProviderTest,
       KeystreamMatchesTheReferenceAtLowAndHighPositions) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x83, 0xcf, 0x93, 0x1a, 0x6a, 0xa7, 0x69, 0xa6,
      0xc4, 0x48, 0x5d, 0x19, 0xc1, 0x5c, 0xcc, 0x52,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x73, 0xc8, 0xd5, 0x49, 0xc4, 0xd9, 0xdb, 0x50, 0x2e, 0xc0, 0x44,
      0xea, 0x33, 0x64, 0x8c, 0x6a, 0xc9, 0xf3, 0x6c, 0x41, 0xb6, 0xa0,
      0x50, 0x4f, 0xe0, 0x93, 0xde, 0xfb, 0x61, 0x9b, 0x09, 0x73,
  };
  static constexpr std::array<std::uint8_t, 32> expected{
      0x0f, 0x6d, 0x89, 0x85, 0x5b, 0xa7, 0x86, 0x74, 0x5b, 0xa1, 0xfe,
      0x5c, 0x81, 0x19, 0x6c, 0xd5, 0x54, 0xc4, 0x1c, 0xca, 0xf6, 0xe9,
      0x34, 0xa4, 0x89, 0x26, 0x98, 0xb0, 0x62, 0x12, 0xb3, 0x1a,
  };
  static constexpr std::array<std::uint8_t, 32> expected_high{
      0x14, 0x31, 0x9f, 0xf8, 0xbe, 0x08, 0x17, 0xa4, 0xfc, 0x28, 0x49,
      0x0b, 0x1e, 0x5f, 0x7b, 0x7e, 0xfa, 0xe9, 0x14, 0xde, 0xc6, 0xdf,
      0xe7, 0x5d, 0xd6, 0x9c, 0x75, 0x3f, 0x94, 0x62, 0xd5, 0x2c,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  auto direction =
      adapter.create_local(material(handshake_key, ecdh_secret, 42));
  ASSERT_TRUE(direction);
  ASSERT_NE(direction.value(), nullptr);

  std::array<std::byte, 32> zeros{};
  EXPECT_TRUE(direction.value()->apply_keystream(0x30, zeros, zeros));
  EXPECT_TRUE(equal_bytes(zeros, expected));

  zeros = {};
  EXPECT_TRUE(direction.value()->apply_keystream(high_position, zeros, zeros));
  EXPECT_TRUE(equal_bytes(zeros, expected_high));

  std::array<std::byte, 1> empty{};
  EXPECT_TRUE(direction.value()->apply_keystream(high_position, {}, {}));
  EXPECT_EQ(empty[0], std::byte{});
}

TEST_F(PlatformTakionCryptoProviderTest,
       LocalEncryptionIsUndoneByTheRemoteDirection) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x14, 0xf1, 0xe6, 0x94, 0x6c, 0x5d, 0xce, 0xa8,
      0xb7, 0xaa, 0x48, 0x50, 0xf6, 0x4d, 0x21, 0xac,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x0c, 0xeb, 0x77, 0x09, 0x83, 0x4d, 0x7a, 0xfc, 0x50, 0xb8, 0x46,
      0x8c, 0xc6, 0x3c, 0x1e, 0x7c, 0x4e, 0x4a, 0x88, 0x93, 0x42, 0x80,
      0xc1, 0x28, 0xe6, 0x1e, 0xe9, 0xd4, 0x1b, 0x8c, 0x69, 0x36,
  };
  static constexpr std::array<std::uint8_t, 37> clear_data{
      0x4e, 0x61, 0x9f, 0x94, 0x5d, 0x4b, 0x8e, 0xbd, 0x2a, 0x15,
      0x4d, 0x03, 0x6a, 0xcd, 0x49, 0x56, 0x9c, 0xc7, 0x5c, 0xe3,
      0xe7, 0x00, 0x17, 0x9a, 0x38, 0xd9, 0x69, 0x53, 0x45, 0xf9,
      0x0c, 0xb5, 0x8c, 0x05, 0x65, 0x0f, 0x70,
  };
  static constexpr std::array<std::uint8_t, 37> expected{
      0x23, 0xf4, 0x8d, 0xd8, 0xaa, 0xf9, 0x58, 0x9b, 0xb1, 0x94,
      0x4f, 0xad, 0x2b, 0x8d, 0xaa, 0x8d, 0x25, 0x88, 0xfa, 0xf8,
      0xb6, 0xd4, 0x17, 0xf4, 0x5f, 0x78, 0xec, 0xf5, 0x4e, 0x37,
      0x20, 0xb0, 0x76, 0x81, 0x07, 0x67, 0x9a,
  };
  static constexpr std::array<std::uint8_t, 37> expected_high{
      0x2b, 0x9e, 0xe9, 0x83, 0x27, 0x44, 0x08, 0xb1, 0x17, 0xf5,
      0x37, 0xa0, 0xd9, 0xc2, 0xc6, 0x05, 0x95, 0x78, 0xb2, 0x78,
      0xfd, 0x17, 0x8c, 0x52, 0xf4, 0x17, 0x9d, 0xee, 0x3f, 0x62,
      0xe6, 0x30, 0x01, 0x61, 0x4c, 0xf4, 0xa1,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  auto local = adapter.create_local(material(handshake_key, ecdh_secret, 42));
  auto remote = adapter.create_remote(material(handshake_key, ecdh_secret, 42));
  ASSERT_TRUE(local);
  ASSERT_TRUE(remote);

  const auto clear = to_bytes(clear_data);
  std::array<std::byte, 37> buffer{};
  std::array<std::byte, 37> decrypted{};

  buffer = clear;
  EXPECT_TRUE(local.value()->apply_keystream(0x11, buffer, buffer));
  EXPECT_TRUE(equal_bytes(buffer, expected));
  decrypted = buffer;
  EXPECT_TRUE(remote.value()->apply_keystream(0x11, decrypted, decrypted));
  EXPECT_EQ(decrypted, clear);
  std::array<std::byte, 37> apart{};
  EXPECT_TRUE(remote.value()->apply_keystream(0x11, buffer, apart));
  EXPECT_EQ(apart, clear);
  EXPECT_TRUE(equal_bytes(buffer, expected));
  EXPECT_EQ(remote.value()
                ->apply_keystream(0x11, buffer, std::span{apart}.first(36))
                .error(),
            CryptoError::invalid_coverage);

  const auto local_mac = local.value()->authenticate(0x11, clear);
  const auto remote_mac = remote.value()->authenticate(0x11, clear);
  ASSERT_TRUE(local_mac);
  ASSERT_TRUE(remote_mac);
  EXPECT_EQ(local_mac.value(), remote_mac.value());

  buffer = clear;
  EXPECT_TRUE(local.value()->apply_keystream(high_position, buffer, buffer));
  EXPECT_TRUE(equal_bytes(buffer, expected_high));
  decrypted = buffer;
  EXPECT_TRUE(
      remote.value()->apply_keystream(high_position, decrypted, decrypted));
  EXPECT_EQ(decrypted, clear);
  const auto local_mac_high = local.value()->authenticate(high_position, clear);
  const auto remote_mac_high =
      remote.value()->authenticate(high_position, clear);
  ASSERT_TRUE(local_mac_high);
  ASSERT_TRUE(remote_mac_high);
  EXPECT_EQ(local_mac_high.value(), remote_mac_high.value());
}

TEST_F(PlatformTakionCryptoProviderTest,
       KeystreamAppliedInPiecesMatchesItAppliedWhole) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x14, 0xf1, 0xe6, 0x94, 0x6c, 0x5d, 0xce, 0xa8,
      0xb7, 0xaa, 0x48, 0x50, 0xf6, 0x4d, 0x21, 0xac,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x0c, 0xeb, 0x77, 0x09, 0x83, 0x4d, 0x7a, 0xfc, 0x50, 0xb8, 0x46,
      0x8c, 0xc6, 0x3c, 0x1e, 0x7c, 0x4e, 0x4a, 0x88, 0x93, 0x42, 0x80,
      0xc1, 0x28, 0xe6, 0x1e, 0xe9, 0xd4, 0x1b, 0x8c, 0x69, 0x36,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  auto local = adapter.create_local(material(handshake_key, ecdh_secret, 42));
  ASSERT_TRUE(local);
  std::vector<std::byte> whole(1500, std::byte{0x5c});
  std::vector<std::byte> pieces = whole;
  EXPECT_TRUE(local.value()->apply_keystream(0x07, whole, whole));
  for (const auto& [from, to] : {std::pair<std::size_t, std::size_t>{0, 9},
                                 {9, 250},
                                 {250, 1300},
                                 {1300, 1500}}) {
    const auto piece = std::span{pieces}.subspan(from, to - from);
    EXPECT_TRUE(local.value()->apply_keystream(0x07 + from, piece, piece))
        << "piece from " << from;
  }
  EXPECT_EQ(pieces, whole);
}

TEST_F(PlatformTakionCryptoProviderTest,
       GmacTagMatchesTheReferenceInBothDirections) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x70, 0x58, 0x37, 0x50, 0x91, 0xea, 0xd1, 0x37,
      0x71, 0x58, 0xec, 0xb3, 0x0b, 0xea, 0x23, 0x87,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x3c, 0x3a, 0xf0, 0xec, 0xd6, 0x33, 0x1b, 0xb1, 0x6d, 0x24, 0x4f,
      0x48, 0x19, 0xde, 0x06, 0x3d, 0xc7, 0x0e, 0xac, 0x95, 0x70, 0xac,
      0x24, 0x92, 0x86, 0xa7, 0x24, 0xd0, 0x7a, 0x37, 0x55, 0x52,
  };
  static constexpr std::array<std::uint8_t, 259> aad{
      0x03, 0x11, 0xa4, 0x11, 0xa5, 0x00, 0x02, 0x50, 0x21, 0x05, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x6b, 0x1d, 0xe0, 0x00, 0x83, 0xf1, 0xc4, 0x79, 0x71,
      0xe4, 0x67, 0x7c, 0xcc, 0xb7, 0x92, 0x07, 0x8b, 0xba, 0x8a, 0x08, 0xd6,
      0x62, 0xf5, 0x07, 0x5d, 0x4a, 0x20, 0x14, 0xb3, 0xe7, 0x0e, 0x5c, 0x77,
      0xfd, 0x6d, 0x60, 0xa3, 0xc5, 0x48, 0xa9, 0xc0, 0x61, 0xf9, 0x84, 0xf6,
      0x37, 0x62, 0xac, 0xbc, 0x1f, 0x5d, 0xe4, 0x40, 0x90, 0x54, 0x71, 0x37,
      0xab, 0x33, 0x7e, 0xaa, 0xa5, 0xf4, 0xc4, 0x09, 0x03, 0xf4, 0xb6, 0xa1,
      0x2f, 0x02, 0x27, 0x53, 0xc6, 0xa5, 0xb1, 0x9c, 0x8f, 0xb4, 0x69, 0xd6,
      0x5c, 0x7f, 0x79, 0x5f, 0x14, 0x47, 0xcc, 0xa8, 0x8b, 0x37, 0xf7, 0x77,
      0x6d, 0x01, 0xf4, 0x5b, 0x59, 0xca, 0x96, 0x73, 0x0a, 0x20, 0xf3, 0x7a,
      0xd1, 0xd2, 0x7f, 0xfe, 0xb6, 0x66, 0xf4, 0xdb, 0x10, 0xaf, 0xf3, 0x07,
      0xee, 0x49, 0x8a, 0xa1, 0xa5, 0x73, 0xd8, 0x23, 0x83, 0x0e, 0x0c, 0x0c,
      0x3a, 0x49, 0xdc, 0xd8, 0x0c, 0xc8, 0xe4, 0xb6, 0xf2, 0x3c, 0x6d, 0x25,
      0x6a, 0x7d, 0xa0, 0x1c, 0x3a, 0x8e, 0x0f, 0x3f, 0x25, 0x47, 0x11, 0x89,
      0x67, 0xc4, 0xf9, 0xac, 0xc7, 0xe2, 0x93, 0x74, 0xc6, 0xfb, 0xdd, 0x48,
      0x57, 0xae, 0x53, 0xe2, 0xf4, 0x17, 0x80, 0x62, 0xf3, 0x09, 0xd8, 0xd4,
      0x7a, 0x46, 0x6b, 0x08, 0x76, 0x9c, 0x29, 0x81, 0x91, 0x4e, 0x57, 0xaf,
      0x70, 0xb6, 0x32, 0x78, 0x98, 0x9e, 0x85, 0xf2, 0xe3, 0x69, 0x15, 0xaf,
      0xd4, 0x32, 0x54, 0x0f, 0xd4, 0xa2, 0xfc, 0xf5, 0x36, 0xdc, 0x06, 0x45,
      0xf4, 0x06, 0xa3, 0x38, 0x21, 0x02, 0xfd, 0xf0, 0x51, 0x75, 0xea, 0xb1,
      0xf6, 0x8e, 0x39, 0x35, 0xfd, 0x75, 0x74, 0xad, 0xbc, 0x3e, 0xfb, 0xee,
      0x7d, 0x0f, 0x2d, 0x76, 0xbe, 0xfd, 0xee,
  };
  static constexpr std::array<std::uint8_t, 4> expected{0x20, 0xcc, 0xa5, 0xf1};

  PlatformTakionCryptoProvider adapter(provider, threading);
  auto local = adapter.create_local(material(handshake_key, ecdh_secret, 3));
  auto remote = adapter.create_remote(material(handshake_key, ecdh_secret, 3));
  ASSERT_TRUE(local);
  ASSERT_TRUE(remote);

  const auto authenticated = to_bytes(aad);
  const auto local_tag = local.value()->authenticate(0x6b1de0, authenticated);
  const auto remote_tag = remote.value()->authenticate(0x6b1de0, authenticated);
  ASSERT_TRUE(local_tag);
  ASSERT_TRUE(remote_tag);
  EXPECT_EQ(local_tag.value(), to_bytes(expected));
  EXPECT_EQ(local_tag.value(), remote_tag.value());
}

TEST_F(PlatformTakionCryptoProviderTest,
       GmacKeyIsDerivedOnceAndAgainOnlyAfterItsRotationBoundary) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
      0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
      0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5,
      0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
  };
  static constexpr std::array<std::uint8_t, 19> aad{
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
      0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
  };

  CountingPlatformCryptoProvider counting;
  PlatformTakionCryptoProvider adapter(counting, threading);
  auto direction =
      adapter.create_local(material(handshake_key, ecdh_secret, 5));
  ASSERT_TRUE(direction);
  EXPECT_EQ(counting.sha256_calls(), 1U);

  const auto authenticated = to_bytes(aad);
  EXPECT_TRUE(direction.value()->authenticate(0, authenticated));
  EXPECT_EQ(counting.sha256_calls(), 1U);

  EXPECT_TRUE(direction.value()->authenticate(45000, authenticated));
  EXPECT_EQ(counting.sha256_calls(), 1U);

  EXPECT_TRUE(direction.value()->authenticate(45001, authenticated));
  EXPECT_EQ(counting.sha256_calls(), 2U);

  EXPECT_EQ(counting.gmac_calls(), 3U);
  EXPECT_EQ(counting.gmac_iv_sizes().size(), 3U);
  EXPECT_EQ(counting.gmac_tag_sizes().size(), 3U);
  for (const auto iv_size : counting.gmac_iv_sizes()) EXPECT_EQ(iv_size, 16U);
  for (const auto tag_size : counting.gmac_tag_sizes()) EXPECT_EQ(tag_size, 4U);
}

TEST_F(PlatformTakionCryptoProviderTest,
       RefusesMaterialWithoutA16ByteKeyAndA32Or66ByteSecret) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
      0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
      0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
  };

  PlatformTakionCryptoProvider adapter(provider, threading);
  const auto untyped_local =
      adapter.create_local(CryptoMaterial(SecretBytes()));
  const auto untyped_remote =
      adapter.create_remote(CryptoMaterial(SecretBytes()));
  ASSERT_FALSE(untyped_local);
  ASSERT_FALSE(untyped_remote);
  EXPECT_EQ(untyped_local.error(), CryptoError::invalid_key_material);
  EXPECT_EQ(untyped_remote.error(), CryptoError::invalid_key_material);

  std::vector<std::uint8_t> short_handshake(handshake_key.begin(),
                                            handshake_key.end() - 1);
  std::vector<std::uint8_t> long_handshake(handshake_key.begin(),
                                           handshake_key.end());
  long_handshake.push_back(0x11);
  const auto short_local =
      adapter.create_local(material(short_handshake, ecdh_secret, 1));
  const auto long_local =
      adapter.create_local(material(long_handshake, ecdh_secret, 1));
  ASSERT_FALSE(short_local);
  ASSERT_FALSE(long_local);
  EXPECT_EQ(short_local.error(), CryptoError::invalid_key_material);
  EXPECT_EQ(long_local.error(), CryptoError::invalid_key_material);

  std::vector<std::uint8_t> secret31(ecdh_secret.begin(),
                                     ecdh_secret.end() - 1);
  std::vector<std::uint8_t> secret33(ecdh_secret.begin(), ecdh_secret.end());
  secret33.push_back(0x40);
  std::vector<std::uint8_t> secret65(ecdh_secret.begin(), ecdh_secret.end());
  secret65.insert(secret65.end(), 33, 0x41);
  std::vector<std::uint8_t> secret66(secret65);
  secret66.push_back(0x42);
  std::vector<std::uint8_t> secret67(secret66);
  secret67.push_back(0x43);

  for (const auto& secret : {secret31, secret33, secret65, secret67}) {
    const auto local = adapter.create_local(material(handshake_key, secret, 1));
    const auto remote =
        adapter.create_remote(material(handshake_key, secret, 1));
    ASSERT_FALSE(local) << "secret size " << secret.size();
    ASSERT_FALSE(remote) << "secret size " << secret.size();
    EXPECT_EQ(local.error(), CryptoError::invalid_key_material)
        << "secret size " << secret.size();
    EXPECT_EQ(remote.error(), CryptoError::invalid_key_material)
        << "secret size " << secret.size();
  }

  EXPECT_TRUE(adapter.create_local(material(handshake_key, ecdh_secret, 1)));
  EXPECT_TRUE(adapter.create_remote(material(handshake_key, ecdh_secret, 1)));
  EXPECT_TRUE(adapter.create_local(material(handshake_key, secret66, 1)));
  EXPECT_TRUE(adapter.create_remote(material(handshake_key, secret66, 1)));
}

TEST_F(PlatformTakionCryptoProviderTest,
       ProviderFailuresSurfaceWithoutTouchingTheBuffer) {
  static constexpr std::array<std::uint8_t, 16> handshake_key{
      0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
      0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60,
  };
  static constexpr std::array<std::uint8_t, 32> ecdh_secret{
      0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
      0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
      0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80,
  };

  FailingPlatformCryptoProvider hmac_provider(PlatformCryptoFailureStage::hmac);
  PlatformTakionCryptoProvider hmac_adapter(hmac_provider, threading);
  const auto create_failed =
      hmac_adapter.create_local(material(handshake_key, ecdh_secret, 7));
  ASSERT_FALSE(create_failed);
  EXPECT_EQ(create_failed.error(), CryptoError::provider_failure);

  FailingPlatformCryptoProvider base_sha256_provider(
      PlatformCryptoFailureStage::base_sha256);
  PlatformTakionCryptoProvider base_sha256_adapter(base_sha256_provider,
                                                   threading);
  const auto local_base_failed =
      base_sha256_adapter.create_local(material(handshake_key, ecdh_secret, 7));
  ASSERT_FALSE(local_base_failed);
  EXPECT_EQ(local_base_failed.error(), CryptoError::provider_failure);
  const auto remote_base_failed = base_sha256_adapter.create_remote(
      material(handshake_key, ecdh_secret, 7));
  ASSERT_FALSE(remote_base_failed);
  EXPECT_EQ(remote_base_failed.error(), CryptoError::provider_failure);

  FailingPlatformCryptoProvider block_provider(
      PlatformCryptoFailureStage::block);
  PlatformTakionCryptoProvider block_adapter(block_provider, threading);
  auto direction =
      block_adapter.create_local(material(handshake_key, ecdh_secret, 7));
  ASSERT_TRUE(direction);
  std::array<std::byte, 32> input{};
  for (std::size_t index = 0; index < input.size(); ++index)
    input[index] = static_cast<std::byte>(0x80U + index);
  const auto original = input;
  const auto encrypt_failed =
      direction.value()->apply_keystream(0x13, input, input);
  ASSERT_FALSE(encrypt_failed);
  EXPECT_EQ(encrypt_failed.error(), CryptoError::provider_failure);
  EXPECT_EQ(input, original);

  FailingPlatformCryptoProvider gmac_provider(PlatformCryptoFailureStage::gmac);
  PlatformTakionCryptoProvider gmac_adapter(gmac_provider, threading);
  auto authenticating =
      gmac_adapter.create_local(material(handshake_key, ecdh_secret, 7));
  ASSERT_TRUE(authenticating);
  const auto authenticate_failed =
      authenticating.value()->authenticate(0x13, input);
  ASSERT_FALSE(authenticate_failed);
  EXPECT_EQ(authenticate_failed.error(), CryptoError::provider_failure);
  EXPECT_EQ(input, original);
}

}  // namespace
}  // namespace takion_test
