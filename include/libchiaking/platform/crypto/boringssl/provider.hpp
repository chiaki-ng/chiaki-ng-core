/// @file
/// @brief Declares the stock CryptoProvider built on BoringSSL.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::platform::boringssl {

/// @brief Stock CryptoProvider that implements the mandatory primitives and
/// HKDF with BoringSSL.
///
/// It depends on BoringSSL being linked into the build and shares its
/// implementation of the mandatory primitives with the OpenSSL provider. It
/// holds no state, so one instance may serve every thread. SHA-256,
/// HMAC-SHA-256, the AES-128 block cipher, AES-128-GMAC and HKDF are provided;
/// ECDH and the other optional primitives keep the CryptoProvider defaults and
/// report CryptoError::unsupported_capability, so Kakusan is not available
/// with this provider.
class BoringSslCryptoProvider final : public CryptoProvider {
 public:
  /// @brief Destroys the provider; there is nothing to release.
  ~BoringSslCryptoProvider() override = default;

  /// @brief Computes SHA-256 with BoringSSL.
  ///
  /// See CryptoProvider::sha256.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param input Message to hash. May be empty.
  /// @param digest Destination of exactly sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure BoringSSL failed.
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Computes HMAC-SHA-256 with BoringSSL.
  ///
  /// See CryptoProvider::hmac_sha256.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure BoringSSL failed.
  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Encrypts one AES-128 block with BoringSSL.
  ///
  /// See CryptoProvider::aes128_encrypt_block.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param block Exactly aes128_block_size bytes of plaintext.
  /// @param output Destination of exactly aes128_block_size bytes. Left
  ///     untouched on failure.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure BoringSSL failed.
  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Encrypts a run of AES-128 blocks with one key schedule.
  ///
  /// See CryptoProvider::aes128_encrypt_blocks.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param blocks Plaintext whose length is a multiple of aes128_block_size
  ///     and at most INT_MAX bytes. May be empty.
  /// @param output Destination of the same length as @p blocks.
  /// @return Nothing on success, including for no blocks.
  /// @retval CryptoError::invalid_argument The key has the wrong size, @p
  /// blocks is not a whole
  ///     number of blocks or is too long, or @p output differs in length.
  /// @retval CryptoError::backend_failure BoringSSL failed; @p output may hold
  /// part of the
  ///     result.
  [[nodiscard]] auto aes128_encrypt_blocks(std::span<const std::byte> key,
                                           std::span<const std::byte> blocks,
                                           std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Computes an AES-128-GCM tag over unencrypted data with BoringSSL.
  ///
  /// See CryptoProvider::aes128_gmac.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param iv Nonce of aes128_gcm_iv_size_min to aes128_gcm_iv_size_max
  ///     bytes.
  /// @param aad Data to authenticate, at most INT_MAX bytes. May be empty.
  /// @param tag Destination of aes128_gcm_tag_size_min to
  ///     aes128_gcm_tag_size_max bytes; a shorter one receives the leading
  ///     bytes of the full tag.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The key, nonce, data or tag size is
  /// out of
  ///     range.
  /// @retval CryptoError::backend_failure BoringSSL failed.
  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override;

  /// @brief Derives key material with BoringSSL's own HKDF.
  ///
  /// See CryptoProvider::hkdf. SHA-256 and SHA-512 are both available,
  /// so this never reports CryptoError::unsupported_capability.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param digest Hash to build HKDF on.
  /// @param salt Extract salt of any length; empty means a block of zero
  ///     bytes.
  /// @param input Input keying material. May be empty.
  /// @param info Context bound into the output. May be empty.
  /// @param output Destination for the derived bytes; its length selects how
  ///     many are produced.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p output is empty, longer than
  /// hkdf_max_blocks
  ///     blocks of @p digest, or @p digest names no hash.
  /// @retval CryptoError::backend_failure BoringSSL failed.
  [[nodiscard]] auto hkdf(HkdfDigest digest, std::span<const std::byte> salt,
                          std::span<const std::byte> input,
                          std::span<const std::byte> info,
                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Generates a secp256r1 or secp521r1 key pair.
  ///
  /// See CryptoProvider::generate_ecdh. The pair keeps its private key inside
  /// BoringSSL, which wipes it when the pair is destroyed. BoringSSL has no
  /// secp256k1, so that curve is reported unsupported.
  ///
  /// Blocks until the result is written. The provider holds no state, so any
  /// number of threads may call it at once.
  ///
  /// @param curve Curve to generate on.
  /// @return The key pair, owned by the caller.
  /// @retval CryptoError::unsupported_capability @p curve is secp256k1.
  /// @retval CryptoError::invalid_argument @p curve names no supported curve.
  /// @retval CryptoError::backend_failure Key generation failed or memory ran
  ///     out.
  [[nodiscard]] auto generate_ecdh(EcdhCurve curve)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> override;

 private:
  class EcKeyPair;
};

}  // namespace libchiaking::platform::boringssl
