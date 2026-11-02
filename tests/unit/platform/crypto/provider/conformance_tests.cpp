#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/platform.hpp>
#include <libchiaking/testing/platform_conformance.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace platform_crypto_test {
namespace {

using libchiaking::Expected;
using libchiaking::platform::ClockProvider;
using libchiaking::platform::CryptoError;
using libchiaking::platform::CryptoProvider;
using libchiaking::platform::DefaultPlatformFactory;
using libchiaking::platform::EcdhCurve;
using libchiaking::platform::NetworkProvider;
using libchiaking::platform::PlatformCapability;
using libchiaking::platform::PlatformFactory;
using libchiaking::platform::SecureRandomProvider;
using libchiaking::platform::ThreadingProvider;
using libchiaking::platform::crypto_conformance::Check;
using libchiaking::platform::crypto_conformance::run;

class MockCryptoProvider final : public CryptoProvider {
 public:
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    return table_sha256(input, digest);
  }

  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)input;
    (void)digest;
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)block;
    (void)output;
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)iv;
    (void)aad;
    (void)tag;
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

 private:
  static Expected<void, CryptoError> table_sha256(
      std::span<const std::byte> input, std::span<std::byte> digest) {
    constexpr std::array<std::byte, 32> abc{
        std::byte{0xba}, std::byte{0x78}, std::byte{0x16}, std::byte{0xbf},
        std::byte{0x8f}, std::byte{0x01}, std::byte{0xcf}, std::byte{0xea},
        std::byte{0x41}, std::byte{0x41}, std::byte{0x40}, std::byte{0xde},
        std::byte{0x5d}, std::byte{0xae}, std::byte{0x22}, std::byte{0x23},
        std::byte{0xb0}, std::byte{0x03}, std::byte{0x61}, std::byte{0xa3},
        std::byte{0x96}, std::byte{0x17}, std::byte{0x7a}, std::byte{0x9c},
        std::byte{0xb4}, std::byte{0x10}, std::byte{0xff}, std::byte{0x61},
        std::byte{0xf2}, std::byte{0x00}, std::byte{0x15}, std::byte{0xad}};
    if (input.size() != 3 || digest.size() != 32)
      return libchiaking::unexpected(CryptoError::invalid_argument);
    for (std::size_t index = 0; index < abc.size(); ++index)
      digest[index] = abc[index];
    return {};
  }
};

class MockFactory final : public PlatformFactory {
 public:
  [[nodiscard]] libchiaking::platform::PlatformInfo info()
      const noexcept override {
    return {.name = "mock-crypto",
            .abi_version = libchiaking::platform::platform_abi_version,
            .capabilities = PlatformCapability::crypto};
  }
  [[nodiscard]] ThreadingProvider& threading() noexcept override {
    return default_.threading();
  }
  [[nodiscard]] ClockProvider& clock() noexcept override {
    return default_.clock();
  }
  [[nodiscard]] ClockProvider& wall_clock() noexcept override {
    return default_.wall_clock();
  }
  [[nodiscard]] SecureRandomProvider& random() noexcept override {
    return default_.random();
  }
  [[nodiscard]] NetworkProvider& network() noexcept override {
    return default_.network();
  }
  [[nodiscard]] CryptoProvider& crypto() noexcept override { return crypto_; }
  [[nodiscard]] libchiaking::platform::HttpProvider& http() noexcept override {
    return default_.http();
  }
  [[nodiscard]] libchiaking::platform::WebSocketProvider& websocket() noexcept
      override {
    return default_.websocket();
  }
  [[nodiscard]] libchiaking::platform::DiagnosticsProvider&
  diagnostics() noexcept override {
    return default_.diagnostics();
  }
  libchiaking::platform::GatewayProvider& gateway() noexcept override {
    return default_.gateway();
  }
  libchiaking::platform::SecureDatagramProvider& secure_datagram() noexcept
      override {
    return default_.secure_datagram();
  }

 private:
  DefaultPlatformFactory default_;
  MockCryptoProvider crypto_;
};

static_assert(std::has_virtual_destructor_v<CryptoProvider>);
static_assert(
    !std::is_copy_constructible_v<libchiaking::platform::EcdhKeyPair>);
static_assert(libchiaking::platform::ecdh_public_coordinates_size(
                  libchiaking::platform::EcdhCurve::secp256k1) == 64);
static_assert(libchiaking::platform::ecdh_public_coordinates_size(
                  libchiaking::platform::EcdhCurve::secp521r1) == 132);
static_assert(libchiaking::platform::ecdh_shared_secret_size(
                  libchiaking::platform::EcdhCurve::secp256k1) == 32);
static_assert(libchiaking::platform::ecdh_shared_secret_size(
                  libchiaking::platform::EcdhCurve::secp521r1) == 66);
static_assert(libchiaking::platform::ecdh_shared_secret_size(
                  static_cast<libchiaking::platform::EcdhCurve>(99)) == 0);

class CryptoConformance : public ::testing::Test {
 protected:
  [[nodiscard]] static constexpr auto mask(Check check) -> std::uint32_t {
    return static_cast<std::uint32_t>(check);
  }

  MockFactory mock_factory;
};

TEST_F(CryptoConformance, PassesForAFactoryWithoutCrypto) {
  DefaultPlatformFactory default_factory;
  const auto report = run(default_factory);
  EXPECT_TRUE(report.ok) << "failed mask 0x" << std::hex << report.failed_mask;
}

TEST_F(CryptoConformance, FailsEveryKnownAnswerAMockProviderGetsWrong) {
  const auto report = run(mock_factory);
  EXPECT_FALSE(report.ok);
  EXPECT_EQ(report.failed_mask, mask(Check::sha256_known_answer) |
                                    mask(Check::hmac_sha256_known_answer) |
                                    mask(Check::aes128_block_known_answer) |
                                    mask(Check::aes128_gmac_known_answer) |
                                    mask(Check::hkdf_known_answer));
}

TEST_F(CryptoConformance, ProviderWithoutEcdhRefusesBothCurves) {
  ASSERT_TRUE(mock_factory.supplies(PlatformCapability::crypto));
  auto& crypto = mock_factory.crypto();
  const auto secp256k1 = crypto.generate_ecdh(EcdhCurve::secp256k1);
  ASSERT_FALSE(secp256k1);
  EXPECT_EQ(secp256k1.error(), CryptoError::unsupported_capability);
  const auto secp521r1 = crypto.generate_ecdh(EcdhCurve::secp521r1);
  ASSERT_FALSE(secp521r1);
  EXPECT_EQ(secp521r1.error(), CryptoError::unsupported_capability);
}

}  // namespace
}  // namespace platform_crypto_test
