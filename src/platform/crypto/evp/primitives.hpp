/// @file
/// @brief Declares the symmetric primitives the stock OpenSSL and BoringSSL
/// crypto providers share, written once against the EVP interface both
/// libraries offer.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <cstddef>
#include <span>

namespace libchiaking::platform::evp {

/// @brief Hashing, HMAC, AES-128 and HKDF on OpenSSL or BoringSSL's EVP API,
/// with the CryptoProvider argument rules.
///
/// Every size is checked before the library is called. Digests and single
/// blocks are produced in a wiping buffer and copied out only on success, so
/// a failure leaves the output untouched; the multi-block encryption and HKDF
/// write the output directly and may leave it partly written on failure.
/// Stateless; every function is reentrant and safe to call from any thread.
/// None blocks.
class Primitives {
 public:
  /// @brief Nothing on success, or the failure.
  using Outcome = Expected<void, CryptoError>;

  /// @brief Hashes input with SHA-256.
  ///
  /// @param input The bytes to hash; may be empty.
  /// @param digest Receives the digest; must be sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument `digest` has the wrong size.
  /// @retval CryptoError::backend_failure The library refused.
  static auto sha256(std::span<const std::byte> input,
                     std::span<std::byte> digest) -> Outcome;
  /// @brief Authenticates input with HMAC-SHA-256.
  ///
  /// @param key The key, of any length.
  /// @param input The message.
  /// @param digest Receives the MAC; must be hmac_sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument `digest` has the wrong size.
  /// @retval CryptoError::backend_failure The library refused.
  static auto hmac_sha256(std::span<const std::byte> key,
                          std::span<const std::byte> input,
                          std::span<std::byte> digest) -> Outcome;
  /// @brief Encrypts one block with AES-128 and no chaining.
  ///
  /// @param key The key; must be aes128_key_size bytes.
  /// @param block The plaintext block; must be aes128_block_size bytes.
  /// @param output Receives the ciphertext block; must be
  ///     aes128_block_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A size is wrong.
  /// @retval CryptoError::backend_failure The library refused.
  static auto aes128_encrypt_block(std::span<const std::byte> key,
                                   std::span<const std::byte> block,
                                   std::span<std::byte> output) -> Outcome;
  /// @brief Encrypts whole blocks with AES-128 in ECB mode, without padding,
  /// as Takion uses to turn counter blocks into keystream.
  ///
  /// @param key The key; must be aes128_key_size bytes.
  /// @param blocks The plaintext; a multiple of aes128_block_size bytes, at
  ///     most `INT_MAX`; empty does nothing.
  /// @param output Receives the ciphertext; the same size as `blocks`.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A size is wrong.
  /// @retval CryptoError::backend_failure The library refused.
  static auto aes128_encrypt_blocks(std::span<const std::byte> key,
                                    std::span<const std::byte> blocks,
                                    std::span<std::byte> output) -> Outcome;
  /// @brief Computes an AES-128-GMAC tag: GCM over additional data only,
  /// with no plaintext.
  ///
  /// @param key The key; must be aes128_key_size bytes.
  /// @param iv The nonce; aes128_gcm_iv_size_min to aes128_gcm_iv_size_max
  ///     bytes.
  /// @param aad The data to authenticate; at most `INT_MAX` bytes.
  /// @param tag Receives the tag, truncated to its size;
  ///     aes128_gcm_tag_size_min to aes128_gcm_tag_size_max bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A size is out of range.
  /// @retval CryptoError::backend_failure The library refused.
  static auto aes128_gmac(std::span<const std::byte> key,
                          std::span<const std::byte> iv,
                          std::span<const std::byte> aad,
                          std::span<std::byte> tag) -> Outcome;
  /// @brief Derives key material with HKDF (RFC 5869), extract then expand.
  ///
  /// @param digest The hash, SHA-256 or SHA-512.
  /// @param salt The salt; may be empty, meaning a zero salt.
  /// @param input The input keying material; may be empty.
  /// @param info The context information; may be empty.
  /// @param output Receives the key material; 1 to hkdf_max_blocks times
  ///     the digest's size.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument `output` is empty or too long,
  ///     or `digest` names no hash.
  /// @retval CryptoError::backend_failure The library refused.
  static auto hkdf(HkdfDigest digest, std::span<const std::byte> salt,
                   std::span<const std::byte> input,
                   std::span<const std::byte> info, std::span<std::byte> output)
      -> Outcome;

 private:
  /// @brief Views bytes as the library's `unsigned char`.
  ///
  /// @param bytes The bytes; may be null.
  /// @return The same address.
  static auto native(const std::byte* bytes) noexcept -> const unsigned char*;
  /// @brief Views writable bytes as the library's `unsigned char`.
  ///
  /// @param bytes The bytes; may be null.
  /// @return The same address.
  static auto native(std::byte* bytes) noexcept -> unsigned char*;
  /// @brief Makes the failure for an argument of the wrong size.
  ///
  /// @return CryptoError::invalid_argument.
  static auto invalid() noexcept -> Unexpected<CryptoError>;
  /// @brief Makes the failure for the library refusing.
  ///
  /// @return CryptoError::backend_failure.
  static auto failure() noexcept -> Unexpected<CryptoError>;
};

}  // namespace libchiaking::platform::evp
