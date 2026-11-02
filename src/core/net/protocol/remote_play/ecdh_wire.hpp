/// @file
/// @brief Declares EcdhWireCodec, the signed ECDH public keys of the stream
/// connection's BIG/BANG handshake.
///
/// Each side sends its public key as an uncompressed SEC1 point (`04 || X ||
/// Y`) together with HMAC-SHA256 of that point keyed by the 16-byte handshake
/// key, which only the two ends of the session know. The curve depends on the
/// Takion version: secp256k1 for most, secp521r1 for versions 10, 13 and up.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/crypto/provider.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief The size of the handshake key that signs public keys, in bytes.
inline constexpr std::size_t ecdh_handshake_key_size = 16;
/// @brief The size of a public key signature (HMAC-SHA256), in bytes.
inline constexpr std::size_t ecdh_signature_size = 32;
/// @brief The largest encoded public key: the tag byte and both coordinates
/// of a secp521r1 point (133 bytes).
inline constexpr std::size_t ecdh_public_wire_max_size =
    platform::ecdh_public_coordinates_size(platform::EcdhCurve::secp521r1) + 1;
/// @brief The largest shared secret: the x coordinate of a secp521r1 point
/// (66 bytes).
inline constexpr std::size_t ecdh_shared_secret_max_size =
    platform::ecdh_shared_secret_size(platform::EcdhCurve::secp521r1);
/// @brief The SEC1 tag of an uncompressed point.
inline constexpr std::byte ec_point_uncompressed_tag{0x04};

/// @brief Why a key could not be prepared, encoded or agreed.
enum class EcdhWireError : std::uint8_t {
  /// @brief The curve is not one the provider knows.
  invalid_curve,
  /// @brief The handshake key is not 16 bytes.
  invalid_handshake_key,
  /// @brief The key pair could not export its coordinates.
  coordinates_failure,
  /// @brief HMAC-SHA256 failed.
  signature_failure,
  /// @brief The console's key is not an uncompressed point of the curve's size.
  invalid_wire_public_key,
  /// @brief The console's signature is not 32 bytes.
  invalid_remote_signature,
  /// @brief The console's signature does not match its key: it was not made
  /// with this session's handshake key.
  signature_mismatch,
  /// @brief Agreeing the secret failed, for example on an invalid point.
  derive_failure,
  /// @brief The provider could not generate a key pair.
  generation_failure,
};

/// @brief A public key ready for the wire with its signature.
///
/// Plain data; the key is public.
struct SignedEcdhPublicKey {
  /// @brief The encoded point; only the first `size` bytes are used.
  std::array<std::byte, ecdh_public_wire_max_size> bytes{};
  /// @brief The encoded point's length: 65 for secp256k1, 133 for secp521r1.
  std::size_t size{};
  /// @brief HMAC-SHA256 of the encoded point under the handshake key.
  std::array<std::byte, ecdh_signature_size> signature{};

  /// @brief The encoded point.
  ///
  /// @return A view of the first `size` bytes.
  auto view() const noexcept -> std::span<const std::byte> {
    return {bytes.data(), size};
  }
};

/// @brief An agreed ECDH secret, in wiped storage.
struct EcdhSharedSecret {
  /// @brief The secret; only the first `size` bytes are used.
  SecretArray<ecdh_shared_secret_max_size> secret;
  /// @brief The secret's length: 32 for secp256k1, 66 for secp521r1.
  std::size_t size{};

  /// @brief The secret.
  ///
  /// @return A view of the first `size` bytes.
  auto view() const noexcept -> std::span<const std::byte> {
    return {secret.data(), size};
  }
};

/// @brief A fresh key pair and its signed public key, made before the BIG
/// message is sent.
///
/// Move-only through the key pair.
struct PreparedEcdh {
  /// @brief The curve the pair belongs to.
  platform::EcdhCurve curve;
  /// @brief The private key pair.
  std::unique_ptr<platform::EcdhKeyPair> key_pair;
  /// @brief The signed public key to send.
  SignedEcdhPublicKey public_key;
};

/// @brief Encodes, signs and verifies handshake public keys.
///
/// Stateless; every member is static. Calls run in the caller's thread and
/// block only on the provider's computation.
class EcdhWireCodec {
 public:
  /// @brief Encodes a key pair's public key and signs it.
  ///
  /// @param crypto The provider that runs HMAC-SHA256.
  /// @param key_pair The local key pair.
  /// @param curve Its curve.
  /// @param handshake_key The 16-byte handshake key.
  /// @return The signed key.
  /// @retval EcdhWireError::invalid_curve The curve is unknown.
  /// @retval EcdhWireError::invalid_handshake_key The key is not 16 bytes.
  /// @retval EcdhWireError::coordinates_failure The coordinates could not be
  ///     read.
  /// @retval EcdhWireError::signature_failure HMAC failed.
  [[nodiscard]] static auto encode_public_key(
      platform::CryptoProvider& crypto, const platform::EcdhKeyPair& key_pair,
      platform::EcdhCurve curve, std::span<const std::byte> handshake_key)
      -> Expected<SignedEcdhPublicKey, EcdhWireError>;

  /// @brief Verifies the console's signed public key and agrees the shared
  /// secret.
  ///
  /// The signature is compared in constant time.
  ///
  /// @param crypto The provider that runs HMAC-SHA256.
  /// @param key_pair The local key pair.
  /// @param curve Its curve.
  /// @param handshake_key The 16-byte handshake key.
  /// @param remote_public_key The console's encoded point.
  /// @param remote_signature The console's 32-byte signature.
  /// @return The shared secret.
  /// @retval EcdhWireError::invalid_curve The curve is unknown.
  /// @retval EcdhWireError::invalid_handshake_key The key is not 16 bytes.
  /// @retval EcdhWireError::invalid_wire_public_key The point is malformed.
  /// @retval EcdhWireError::invalid_remote_signature The signature is not 32
  ///     bytes.
  /// @retval EcdhWireError::signature_failure HMAC failed.
  /// @retval EcdhWireError::signature_mismatch The signature is wrong.
  /// @retval EcdhWireError::derive_failure The secret could not be agreed.
  [[nodiscard]] static auto decode_shared_secret(
      platform::CryptoProvider& crypto, const platform::EcdhKeyPair& key_pair,
      platform::EcdhCurve curve, std::span<const std::byte> handshake_key,
      std::span<const std::byte> remote_public_key,
      std::span<const std::byte> remote_signature)
      -> Expected<EcdhSharedSecret, EcdhWireError>;

  /// @brief Generates a key pair on a curve and signs its public key.
  ///
  /// @param crypto The provider that generates keys and runs HMAC-SHA256.
  /// @param curve The curve.
  /// @param handshake_key The 16-byte handshake key.
  /// @return The pair and signed key.
  /// @retval EcdhWireError::invalid_curve The curve is unknown.
  /// @retval EcdhWireError::invalid_handshake_key The key is not 16 bytes.
  /// @retval EcdhWireError::generation_failure No key pair could be made.
  /// @retval EcdhWireError Any failure of encode_public_key().
  [[nodiscard]] static auto prepare(platform::CryptoProvider& crypto,
                                    platform::EcdhCurve curve,
                                    std::span<const std::byte> handshake_key)
      -> Expected<PreparedEcdh, EcdhWireError>;
};

}  // namespace libchiaking::net::protocol::remote_play
