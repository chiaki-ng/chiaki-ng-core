/// @file
/// @brief Declares PlatformTakionCryptoProvider, the GKCrypt implementation of
/// Takion's packet protection over the platform's AES, GMAC and hashes.
#pragma once

#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <optional>

namespace libchiaking::net::protocol::takion {

/// @brief Derives and runs GKCrypt cipher directions with the platform crypto
/// provider.
///
/// Key derivation: HMAC-SHA256 keyed by the ECDH secret over the 21 bytes
/// `01 index 00 handshake_key[16] 01 00` gives 32 bytes; the first 16 are the
/// AES-128 key, the last 16 the IV. The base GMAC key is SHA-256(key || IV)
/// with its two halves XORed together. The keystream is AES-128-CTR whose
/// counter block is the IV plus position / 16 as a little-endian 128-bit sum,
/// entered at byte position % 16. A tag is AES-GMAC truncated to 4 bytes with
/// the nonce IV + position / 16; after every 45000 key positions the GMAC key
/// is re-derived as the folded SHA-256 of the base key and the IV advanced by
/// 44910 per rotation.
///
/// Holds references to the platform crypto and threading providers, which
/// must outlive it and every direction it creates. Safe to share between
/// sessions; each direction guards its GMAC key cache with a mutex.
class PlatformTakionCryptoProvider final : public CryptoProvider {
 public:
  /// @brief Wraps a platform provider.
  ///
  /// @param provider AES, GMAC, SHA-256 and HMAC; must outlive this object.
  /// @param threading Makes each direction's GMAC cache mutex; must outlive
  ///     this object and every direction it creates.
  PlatformTakionCryptoProvider(platform::CryptoProvider& provider,
                               platform::ThreadingProvider& threading) noexcept
      : provider_(provider), threading_(threading) {}

  /// @brief Creates the direction this side sends with.
  ///
  /// @param material Handshake key, ECDH secret and GKCrypt index.
  /// @return The direction.
  /// @retval CryptoError::invalid_key_material No index, a handshake key not 16
  ///     bytes, or a secret of neither 32 nor 66 bytes.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto create_local(CryptoMaterial material)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> override;
  /// @brief Creates the direction the far end sends with; derived exactly as
  /// create_local().
  ///
  /// @param material Handshake key, ECDH secret and GKCrypt index.
  /// @return The direction.
  /// @retval CryptoError::invalid_key_material The material is unusable.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto create_remote(CryptoMaterial material)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> override;
  /// @brief Derives a direction's key and IV for forwarding.
  ///
  /// @param material Handshake key, ECDH secret and GKCrypt index.
  /// @return The keys.
  /// @retval CryptoError::invalid_key_material The material is unusable.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto export_receive_keys(const CryptoMaterial& material)
      -> Expected<SourceReceiveKeys, CryptoError> override;
  /// @brief Creates a direction from forwarded keys.
  ///
  /// @param keys The key and IV.
  /// @return The direction.
  /// @retval CryptoError::provider_failure The platform failed.
  [[nodiscard]] auto create_receive_only(const SourceReceiveKeys& keys)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> override;

 private:
  /// @brief Bytes of the AES key and IV.
  static constexpr std::size_t key_base_size = takion_crypto_block_size;
  /// @brief Bytes of a SHA-256 or HMAC-SHA256 digest.
  static constexpr std::size_t derived_digest_size =
      2 * takion_crypto_block_size;
  /// @brief Builds the provider_failure result.
  ///
  /// @return The unexpected value.
  [[nodiscard]] static auto provider_failure() noexcept
      -> libchiaking::Unexpected<CryptoError>;
  /// @brief Adds a 64-bit value to a 128-bit little-endian number.
  ///
  /// @param output Receives the sum.
  /// @param base The 128-bit number.
  /// @param value The amount.
  static void add_le128(
      std::span<std::byte, takion_crypto_block_size> output,
      std::span<const std::byte, takion_crypto_block_size> base,
      std::uint64_t value) noexcept;
  /// @brief Folds SHA-256 of two 16-byte halves into a 16-byte key by XORing
  /// the digest's halves.
  ///
  /// @param provider The platform provider.
  /// @param first The first 16 bytes hashed.
  /// @param second The next 16 bytes hashed.
  /// @return The key.
  /// @retval CryptoError::provider_failure The hash failed.
  [[nodiscard]] static auto hashed_key(
      platform::CryptoProvider& provider,
      std::span<const std::byte, key_base_size> first,
      std::span<const std::byte, key_base_size> second)
      -> Expected<SecretArray<key_base_size>, CryptoError>;
  /// @brief Derives a direction's key and IV from its material.
  ///
  /// @param provider The platform provider.
  /// @param material The material.
  /// @return The key and IV.
  /// @retval CryptoError::invalid_key_material The material is unusable.
  /// @retval CryptoError::provider_failure The HMAC failed.
  [[nodiscard]] static auto derive_keys(platform::CryptoProvider& provider,
                                        const CryptoMaterial& material)
      -> Expected<SourceReceiveKeys, CryptoError>;
  /// @brief Builds a GKCrypt direction from a key and IV.
  ///
  /// @param provider The platform provider.
  /// @param threading Makes the direction's GMAC cache mutex.
  /// @param keys The key and IV.
  /// @return The direction.
  /// @retval CryptoError::provider_failure Hashing or allocation failed.
  [[nodiscard]] static auto create_direction(
      platform::CryptoProvider& provider,
      platform::ThreadingProvider& threading, SourceReceiveKeys keys)
      -> Expected<std::unique_ptr<CryptoDirection>, CryptoError>;
  /// @brief The GKCrypt cipher direction, defined in the .cpp.
  class GKCryptDirection;

  /// @brief The platform provider.
  platform::CryptoProvider& provider_;
  /// @brief Makes each direction's GMAC cache mutex.
  platform::ThreadingProvider& threading_;
};

}  // namespace libchiaking::net::protocol::takion
