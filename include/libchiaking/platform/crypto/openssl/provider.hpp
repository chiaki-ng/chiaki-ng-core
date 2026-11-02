/// @file
/// @brief Declares the stock CryptoProvider built on OpenSSL.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::platform::openssl {

/// @brief Stock CryptoProvider that implements every primitive with OpenSSL.
///
/// It depends on OpenSSL's libcrypto, version 3, being linked into the build.
/// It holds no state, so one instance may serve every thread. It is the
/// provider DefaultPlatform uses unless the build selects BoringSSL, and the
/// only stock provider with the optional primitives that Kakusan and ECDH
/// key exchange need. Each call leaves OpenSSL's error queue as it found it.
class OpenSslCryptoProvider final : public CryptoProvider {
 public:
  /// @brief Computes SHA-256 with OpenSSL.
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
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Computes HMAC-SHA-256 with OpenSSL.
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
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Encrypts one AES-128 block with OpenSSL.
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
  /// @retval CryptoError::backend_failure OpenSSL failed.
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
  /// @retval CryptoError::backend_failure OpenSSL failed; @p output may hold
  /// part of the
  ///     result.
  [[nodiscard]] auto aes128_encrypt_blocks(std::span<const std::byte> key,
                                           std::span<const std::byte> blocks,
                                           std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Computes an AES-128-GCM tag over unencrypted data with OpenSSL.
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
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override;

  /// @brief Derives key material with OpenSSL's own HKDF.
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
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto hkdf(HkdfDigest digest, std::span<const std::byte> salt,
                          std::span<const std::byte> input,
                          std::span<const std::byte> info,
                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Generates a secp256k1, secp521r1 or secp256r1 key pair.
  ///
  /// See CryptoProvider::generate_ecdh. The pair keeps its private key
  /// inside OpenSSL and wipes it when destroyed.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param curve Curve to generate on.
  /// @return The key pair, owned by the caller.
  /// @retval CryptoError::invalid_argument @p curve names no supported curve.
  /// @retval CryptoError::backend_failure Key generation failed or memory ran
  /// out.
  [[nodiscard]] auto generate_ecdh(EcdhCurve curve)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> override;

  /// @brief Computes SHA-512 with OpenSSL.
  ///
  /// See CryptoProvider::sha512.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param input Message to hash. May be empty.
  /// @param digest Destination of exactly sha512_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto sha512(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Computes HMAC-SHA-512 with OpenSSL.
  ///
  /// See CryptoProvider::hmac_sha512.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha512_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto hmac_sha512(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Computes HMAC-SHA-1 with OpenSSL.
  ///
  /// See CryptoProvider::hmac_sha1.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha1_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto hmac_sha1(std::span<const std::byte> key,
                               std::span<const std::byte> input,
                               std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Derives key material with OpenSSL's scrypt.
  ///
  /// See CryptoProvider::scrypt. The derivation is capped at 64 MiB of
  /// memory.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param password Secret to stretch.
  /// @param salt Salt for the derivation.
  /// @param parameters Work factors. ScryptParameters::cost must be a power of
  ///     two of at least 2, and the other two factors must not be zero.
  /// @param output Destination for the derived bytes. Must not be empty.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p output is empty or a work factor
  /// is not
  ///     acceptable.
  /// @retval CryptoError::backend_failure The derivation failed, for example
  /// because it
  ///     needs more memory than the cap allows.
  [[nodiscard]] auto scrypt(std::span<const std::byte> password,
                            std::span<const std::byte> salt,
                            ScryptParameters parameters,
                            std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Derives an Ed25519 public key from a seed.
  ///
  /// See CryptoProvider::ed25519_public_key.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param seed Exactly ed25519_seed_size bytes. Treat it as secret.
  /// @param public_key Destination of exactly ed25519_public_key_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto ed25519_public_key(std::span<const std::byte> seed,
                                        std::span<std::byte> public_key)
      -> Expected<void, CryptoError> override;

  /// @brief Signs a message with Ed25519.
  ///
  /// See CryptoProvider::ed25519_sign.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param seed Private seed of exactly ed25519_seed_size bytes.
  /// @param message Message to sign, of any length.
  /// @param signature Destination of exactly ed25519_signature_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto ed25519_sign(std::span<const std::byte> seed,
                                  std::span<const std::byte> message,
                                  std::span<std::byte> signature)
      -> Expected<void, CryptoError> override;

  /// @brief Checks an Ed25519 signature.
  ///
  /// See CryptoProvider::ed25519_verify.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param public_key Exactly ed25519_public_key_size bytes.
  /// @param message Message the signature should cover.
  /// @param signature Exactly ed25519_signature_size bytes.
  /// @return True when the signature is valid for the message and key; false
  /// when it is not, and also when OpenSSL cannot load @p public_key.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  /// @retval CryptoError::backend_failure OpenSSL could not perform the check.
  [[nodiscard]] auto ed25519_verify(std::span<const std::byte> public_key,
                                    std::span<const std::byte> message,
                                    std::span<const std::byte> signature)
      -> Expected<bool, CryptoError> override;

  /// @brief Multiplies the P-256 generator by a scalar.
  ///
  /// See CryptoProvider::p256_base_multiply.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param scalar Big-endian scalar of exactly p256_scalar_size bytes. Treat
  ///     it as secret.
  /// @param point Destination of exactly p256_point_size bytes, in
  ///     uncompressed SEC1 form.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size, or the
  /// product is
  ///     the point at infinity.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto p256_base_multiply(std::span<const std::byte> scalar,
                                        std::span<std::byte> point)
      -> Expected<void, CryptoError> override;

  /// @brief Multiplies a P-256 point by a scalar.
  ///
  /// See CryptoProvider::p256_multiply.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param scalar Big-endian scalar of exactly p256_scalar_size bytes. Treat
  ///     it as secret.
  /// @param point Point in uncompressed SEC1 form, exactly p256_point_size
  ///     bytes, on the curve.
  /// @param product Destination of exactly p256_point_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size, @p point
  /// is not on
  ///     the curve, or the product is the point at infinity.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto p256_multiply(std::span<const std::byte> scalar,
                                   std::span<const std::byte> point,
                                   std::span<std::byte> product)
      -> Expected<void, CryptoError> override;

  /// @brief Adds two P-256 points.
  ///
  /// See CryptoProvider::p256_add.
  ///
  /// Blocks until the result is written. The provider holds no state, so
  /// any number of threads may call it at once.
  ///
  /// @param left First point in uncompressed SEC1 form, exactly
  ///     p256_point_size bytes.
  /// @param right Second point in the same form.
  /// @param sum Destination of exactly p256_point_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size, an
  /// operand is not
  ///     on the curve, or the sum is the point at infinity.
  /// @retval CryptoError::backend_failure OpenSSL failed.
  [[nodiscard]] auto p256_add(std::span<const std::byte> left,
                              std::span<const std::byte> right,
                              std::span<std::byte> sum)
      -> Expected<void, CryptoError> override;
};

}  // namespace libchiaking::platform::openssl
