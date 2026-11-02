/// @file
/// @brief Declares the CryptoProvider for Nintendo Switch homebrew, built on
/// libnx's crypto routines.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>

#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::platform::libnx {

/// @brief CryptoProvider for the Nintendo Switch, over libnx and in-tree code.
///
/// SHA-256, HMAC-SHA-256, HMAC-SHA-1 and the AES-128 block cipher come from
/// libnx. AES-128-GMAC hashes with the ARMv8 PMULL instruction around libnx's
/// AES, and ECDH on secp256k1, secp521r1 and secp256r1 runs on the vendored
/// micro-ecc through its public key-exchange functions only,
/// with private keys drawn from the SecureRandomProvider given at
/// construction. micro-ecc's process-wide random hook is never installed:
/// shared-secret derivation therefore skips micro-ecc's projective coordinate
/// randomisation on the Switch, where micro-ecc has no generator of its own.
///
/// SHA-512, HMAC-SHA-512, scrypt, Ed25519 and the P-256 point operations are
/// not
/// provided and report CryptoError::unsupported_capability, so the features
/// that need them, such as Kakusan, are unavailable. HKDF uses the
/// CryptoProvider default, so HKDF-SHA-256 works and HKDF-SHA-512 reports
/// CryptoError::unsupported_capability.
///
/// The provider holds only the borrowed random source, so any number of
/// threads may call it at once as long as that source is thread-safe. Pass it
/// to DefaultPlatformFactory to run crypto_conformance::run() against it on
/// hardware.
class LibnxCryptoProvider final : public CryptoProvider {
 public:
  /// @brief Makes a provider that draws ECDH private keys from a random
  /// source.
  ///
  /// @param random Source of private keys. Borrowed; it must outlive the
  ///     provider and every key pair it generates is independent of it
  ///     afterwards.
  explicit LibnxCryptoProvider(SecureRandomProvider& random) noexcept;

  /// @brief Computes SHA-256 with libnx.
  ///
  /// See CryptoProvider::sha256.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param input Message to hash. May be empty.
  /// @param digest Destination of exactly sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  [[nodiscard]] auto sha256(std::span<const std::byte> input,
                            std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Computes HMAC-SHA-256 with libnx.
  ///
  /// See CryptoProvider::hmac_sha256.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha256_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  [[nodiscard]] auto hmac_sha256(std::span<const std::byte> key,
                                 std::span<const std::byte> input,
                                 std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Encrypts one AES-128 block with libnx.
  ///
  /// See CryptoProvider::aes128_encrypt_block.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param block Exactly aes128_block_size bytes of plaintext.
  /// @param output Destination of exactly aes128_block_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size.
  [[nodiscard]] auto aes128_encrypt_block(std::span<const std::byte> key,
                                          std::span<const std::byte> block,
                                          std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Encrypts a run of AES-128 blocks with one libnx key schedule.
  ///
  /// See CryptoProvider::aes128_encrypt_blocks.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param blocks Plaintext whose length is a multiple of aes128_block_size.
  ///     May be empty.
  /// @param output Destination of the same length as @p blocks.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The key has the wrong size, @p
  ///     blocks is not a whole number of blocks, or @p output differs in
  ///     length.
  [[nodiscard]] auto aes128_encrypt_blocks(std::span<const std::byte> key,
                                           std::span<const std::byte> blocks,
                                           std::span<std::byte> output)
      -> Expected<void, CryptoError> override;

  /// @brief Computes an AES-128-GCM tag over unencrypted data, hashing with
  /// PMULL around libnx's AES.
  ///
  /// See CryptoProvider::aes128_gmac.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @param iv Nonce of aes128_gcm_iv_size_min to aes128_gcm_iv_size_max
  ///     bytes.
  /// @param aad Data to authenticate. May be empty.
  /// @param tag Destination of aes128_gcm_tag_size_min to
  ///     aes128_gcm_tag_size_max bytes; a shorter one receives the leading
  ///     bytes of the full tag.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument The key, nonce or tag size is out
  ///     of range.
  [[nodiscard]] auto aes128_gmac(std::span<const std::byte> key,
                                 std::span<const std::byte> iv,
                                 std::span<const std::byte> aad,
                                 std::span<std::byte> tag)
      -> Expected<void, CryptoError> override;

  /// @brief Generates a secp256k1, secp521r1 or secp256r1 key pair with
  /// micro-ecc.
  ///
  /// See CryptoProvider::generate_ecdh. The private key is drawn from the
  /// random source given at construction, kept in a wiping buffer inside the
  /// pair and erased when the pair is destroyed.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param curve Curve to generate on.
  /// @return The key pair, owned by the caller.
  /// @retval CryptoError::invalid_argument @p curve names no supported curve.
  /// @retval CryptoError::backend_failure The random source failed, no valid
  ///     private key was drawn, or memory ran out.
  [[nodiscard]] auto generate_ecdh(EcdhCurve curve)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> override;

  /// @brief Computes HMAC-SHA-1 with libnx, for STUN message integrity.
  ///
  /// See CryptoProvider::hmac_sha1.
  ///
  /// Blocks until the result is written. Any number of threads may call it
  /// at once.
  ///
  /// @param key Secret key of any length.
  /// @param input Message to authenticate.
  /// @param digest Destination of exactly hmac_sha1_digest_size bytes.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p digest has the wrong size.
  [[nodiscard]] auto hmac_sha1(std::span<const std::byte> key,
                               std::span<const std::byte> input,
                               std::span<std::byte> digest)
      -> Expected<void, CryptoError> override;

  /// @brief Prepares an AES-128 key with libnx's hardware AES and a PMULL
  /// GHASH key, both computed once.
  ///
  /// See CryptoProvider::aes128_cipher. The cipher holds the expanded key
  /// schedule and wipes it when destroyed; any number of threads may use it
  /// at once.
  ///
  /// @param key Exactly aes128_key_size bytes.
  /// @return The cipher.
  /// @retval CryptoError::invalid_argument The key has the wrong size.
  /// @retval CryptoError::backend_failure Memory ran out.
  [[nodiscard]] auto aes128_cipher(std::span<const std::byte> key)
      -> Expected<std::unique_ptr<Aes128Cipher>, CryptoError> override;

 private:
  class HardwareCipher;

  SecureRandomProvider* random_;
};

}  // namespace libchiaking::platform::libnx
