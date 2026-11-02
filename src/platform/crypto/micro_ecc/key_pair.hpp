/// @file
/// @brief Declares KeyPair, the ECDH key pair on the vendored
/// micro-ecc for secp256k1, secp521r1 and secp256r1.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/platform/crypto/error.hpp>
#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::platform::micro_ecc {

/// @brief An ephemeral ECDH key pair computed with micro-ecc.
///
/// The private key is drawn from a SecureRandomProvider, held in a
/// SecretArray and wiped when the pair is destroyed. micro-ecc's own
/// process-wide random hook is never installed, so generation does not touch
/// global state; shared-secret derivation then uses micro-ecc's coordinate
/// randomisation only where micro-ecc finds a platform generator of its own.
/// Public keys and secrets use the big-endian, fixed-width layout
/// CryptoProvider documents. A pair is immutable after construction and may
/// be read from several threads.
///
/// @tparam Curve The curve, EcdhCurve::secp256k1, EcdhCurve::secp521r1 or
///     EcdhCurve::secp256r1.
template <EcdhCurve Curve>
class KeyPair final : public EcdhKeyPair {
 public:
  /// @brief Generates a pair from fresh random bytes.
  ///
  /// Private keys outside [1, n - 1] are drawn again, a bounded number of
  /// times.
  ///
  /// @param random Source of the private key.
  /// @return The key pair, owned by the caller.
  /// @retval CryptoError::backend_failure The random source failed, every
  ///     draw was out of range, or memory ran out.
  [[nodiscard]] static auto generate(SecureRandomProvider& random)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError>;

  /// @brief Writes the public key as raw X then Y coordinates.
  ///
  /// See EcdhKeyPair::public_coordinates.
  ///
  /// @param output Destination of exactly ecdh_public_coordinates_size()
  ///     bytes for the curve; untouched when the size is wrong.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument @p output has the wrong size.
  [[nodiscard]] auto public_coordinates(std::span<std::byte> output) const
      -> Expected<void, CryptoError> override;

  /// @brief Computes the shared secret with a peer's public key.
  ///
  /// See EcdhKeyPair::derive_secret.
  ///
  /// @param peer_coordinates The peer's raw X then Y coordinates.
  /// @param secret Destination of exactly ecdh_shared_secret_size() bytes;
  ///     unchanged on failure.
  /// @return Nothing on success.
  /// @retval CryptoError::invalid_argument A span has the wrong size or the
  ///     peer point is not on the curve.
  /// @retval CryptoError::backend_failure micro-ecc could not derive the
  ///     secret.
  [[nodiscard]] auto derive_secret(std::span<const std::byte> peer_coordinates,
                                   std::span<std::byte> secret) const
      -> Expected<void, CryptoError> override;

 private:
  /// @brief Size of the raw X then Y public key on the curve.
  static constexpr std::size_t coordinates_size =
      ecdh_public_coordinates_size(Curve);
  /// @brief Size of a big-endian private scalar, the byte width of the
  /// curve order.
  static constexpr std::size_t private_key_size =
      ecdh_shared_secret_size(Curve);
  /// @brief Draws generate() makes before it gives up.
  static constexpr int generation_attempts = 64;

  /// @brief The private scalar, wiped on destruction.
  using PrivateKey = SecretArray<private_key_size>;
  /// @brief The public key as raw X then Y coordinates.
  using PublicKey = std::array<std::byte, coordinates_size>;
  /// @brief A shared secret while it is staged, wiped on destruction.
  using Secret = SecretArray<ecdh_shared_secret_size(Curve)>;

  /// @brief Takes a private key whose public key is already computed.
  ///
  /// @param private_key The private scalar; empty afterwards.
  /// @param public_key Its public key.
  KeyPair(PrivateKey&& private_key,
                  const PublicKey& public_key) noexcept;

  /// @brief Multiplies the generator by a private scalar with micro-ecc.
  ///
  /// @param private_key The scalar.
  /// @param public_key Receives the product.
  /// @return False when the scalar is zero or not below the curve order.
  static auto compute_public_key(const PrivateKey& private_key,
                                 PublicKey& public_key) noexcept -> bool;
  /// @brief Checks that coordinates encode a point on the curve.
  ///
  /// @param coordinates Raw X then Y coordinates.
  /// @return True when micro-ecc accepts the point.
  static auto valid_public_key(
      std::span<const std::byte, coordinates_size> coordinates) noexcept
      -> bool;
  /// @brief Multiplies a peer's point by a private scalar with micro-ecc.
  ///
  /// @param coordinates The peer's raw X then Y coordinates, already
  ///     validated.
  /// @param private_key The scalar.
  /// @param secret Receives the X coordinate of the product.
  /// @return False when micro-ecc fails.
  static auto shared_secret(
      std::span<const std::byte, coordinates_size> coordinates,
      const PrivateKey& private_key, Secret& secret) noexcept -> bool;
  /// @brief Clears the bits of a drawn scalar above the curve order's width.
  ///
  /// @param private_key The freshly drawn scalar.
  static void restrict_to_order_width(PrivateKey& private_key) noexcept;

  /// @brief The private scalar.
  PrivateKey private_key_;
  /// @brief The public key derived from it.
  PublicKey public_key_;
};

extern template class KeyPair<EcdhCurve::secp256k1>;
extern template class KeyPair<EcdhCurve::secp521r1>;
extern template class KeyPair<EcdhCurve::secp256r1>;

}  // namespace libchiaking::platform::micro_ecc
