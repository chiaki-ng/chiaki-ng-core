/// @file
/// @brief Declares AbsentCryptoProvider, the stand-in a factory returns when
/// the host supplies no cryptography port.
#pragma once

#include <libchiaking/platform/crypto/provider.hpp>

namespace libchiaking::platform {

/// @brief CryptoProvider whose every primitive is unsupported.
///
/// A factory returns it from PlatformFactory::crypto when the host supplies no
/// provider, and leaves PlatformCapability::crypto unset. The optional
/// primitives keep their CryptoProvider defaults. It has no state and may be
/// called from any thread at once.
class AbsentCryptoProvider final : public CryptoProvider {
 public:
  /// @brief Refuses to hash.
  ///
  /// @param input Ignored.
  /// @param digest Left untouched.
  /// @return Always CryptoError::unsupported_capability.
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    (void)input;
    (void)digest;
    return unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Refuses to authenticate.
  ///
  /// @param key Ignored.
  /// @param input Ignored.
  /// @param digest Left untouched.
  /// @return Always CryptoError::unsupported_capability.
  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)input;
    (void)digest;
    return unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Refuses to encrypt.
  ///
  /// @param key Ignored.
  /// @param block Ignored.
  /// @param output Left untouched.
  /// @return Always CryptoError::unsupported_capability.
  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)block;
    (void)output;
    return unexpected(CryptoError::unsupported_capability);
  }

  /// @brief Refuses to authenticate.
  ///
  /// @param key Ignored.
  /// @param iv Ignored.
  /// @param aad Ignored.
  /// @param tag Left untouched.
  /// @return Always CryptoError::unsupported_capability.
  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override {
    (void)key;
    (void)iv;
    (void)aad;
    (void)tag;
    return unexpected(CryptoError::unsupported_capability);
  }
};

}  // namespace libchiaking::platform
