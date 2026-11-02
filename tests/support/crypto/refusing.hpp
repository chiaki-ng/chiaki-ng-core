#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace test_support {

class NoCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  [[nodiscard]] auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return refused();
  }

  [[nodiscard]] auto hmac_sha256(std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return refused();
  }

  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte>,
                                          std::span<const std::byte>,
                                          std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return refused();
  }

  [[nodiscard]] auto aes128_gmac(std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<const std::byte>,
                                 std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return refused();
  }

 private:
  static auto refused()
      -> libchiaking::Unexpected<libchiaking::platform::CryptoError> {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class CountingKeyPair final : public libchiaking::platform::EcdhKeyPair {
 public:
  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = static_cast<std::byte>(index);
    return {};
  }

  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class RefusingTakionCrypto final
    : public libchiaking::net::protocol::takion::CryptoProvider {
 public:
  using CryptoDirection = libchiaking::net::protocol::takion::CryptoDirection;
  using CryptoError = libchiaking::net::protocol::takion::CryptoError;
  using CryptoMaterial = libchiaking::net::protocol::takion::CryptoMaterial;

  auto create_local(CryptoMaterial)
      -> libchiaking::Expected<std::unique_ptr<CryptoDirection>,
                               CryptoError> override {
    return libchiaking::unexpected(CryptoError::provider_failure);
  }

  auto create_remote(CryptoMaterial)
      -> libchiaking::Expected<std::unique_ptr<CryptoDirection>,
                               CryptoError> override {
    return libchiaking::unexpected(CryptoError::provider_failure);
  }
};

}  // namespace test_support
