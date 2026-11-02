#include "src/platform/crypto/micro_ecc/key_pair.hpp"

#include <microecc/uECC.h>
#include <microecc/uECC_p521.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace libchiaking::platform::micro_ecc {

template <EcdhCurve Curve>
KeyPair<Curve>::KeyPair(PrivateKey&& private_key,
                                        const PublicKey& public_key) noexcept
    : private_key_(std::move(private_key)), public_key_(public_key) {}

template <EcdhCurve Curve>
auto KeyPair<Curve>::generate(SecureRandomProvider& random)
    -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
  PrivateKey private_key;
  PublicKey public_key{};
  for (int attempt = 0; attempt < generation_attempts; ++attempt) {
    if (!random.fill_bytes(private_key.span()))
      return libchiaking::unexpected(CryptoError::backend_failure);
    restrict_to_order_width(private_key);
    if (!compute_public_key(private_key, public_key)) continue;
    std::unique_ptr<EcdhKeyPair> pair{
        new (std::nothrow) KeyPair(std::move(private_key), public_key)};
    if (!pair) return libchiaking::unexpected(CryptoError::backend_failure);
    return pair;
  }
  return libchiaking::unexpected(CryptoError::backend_failure);
}

template <EcdhCurve Curve>
auto KeyPair<Curve>::public_coordinates(
    std::span<std::byte> output) const -> Expected<void, CryptoError> {
  if (output.size() != coordinates_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  std::ranges::copy(public_key_, output.begin());
  return {};
}

template <EcdhCurve Curve>
auto KeyPair<Curve>::derive_secret(
    std::span<const std::byte> peer_coordinates,
    std::span<std::byte> secret) const -> Expected<void, CryptoError> {
  if (peer_coordinates.size() != coordinates_size ||
      secret.size() != ecdh_shared_secret_size(Curve))
    return libchiaking::unexpected(CryptoError::invalid_argument);
  const auto peer = peer_coordinates.template first<coordinates_size>();
  if (!valid_public_key(peer))
    return libchiaking::unexpected(CryptoError::invalid_argument);
  Secret staging;
  if (!shared_secret(peer, private_key_, staging))
    return libchiaking::unexpected(CryptoError::backend_failure);
  std::ranges::copy(staging, secret.begin());
  return {};
}

template <EcdhCurve Curve>
auto KeyPair<Curve>::compute_public_key(const PrivateKey& private_key,
                                                PublicKey& public_key) noexcept
    -> bool {
  const auto* scalar =
      reinterpret_cast<const std::uint8_t*>(private_key.data());
  auto* point = reinterpret_cast<std::uint8_t*>(public_key.data());
  if constexpr (Curve == EcdhCurve::secp256k1)
    return uECC_compute_public_key(scalar, point, uECC_secp256k1()) == 1;
  else if constexpr (Curve == EcdhCurve::secp256r1)
    return uECC_compute_public_key(scalar, point, uECC_secp256r1()) == 1;
  else
    return uECC_p521_compute_public_key(scalar, point, uECC_p521_secp521r1()) ==
           1;
}

template <EcdhCurve Curve>
auto KeyPair<Curve>::valid_public_key(
    std::span<const std::byte, coordinates_size> coordinates) noexcept -> bool {
  const auto* point = reinterpret_cast<const std::uint8_t*>(coordinates.data());
  if constexpr (Curve == EcdhCurve::secp256k1)
    return uECC_valid_public_key(point, uECC_secp256k1()) == 1;
  else if constexpr (Curve == EcdhCurve::secp256r1)
    return uECC_valid_public_key(point, uECC_secp256r1()) == 1;
  else
    return uECC_p521_valid_public_key(point, uECC_p521_secp521r1()) == 1;
}

template <EcdhCurve Curve>
auto KeyPair<Curve>::shared_secret(
    std::span<const std::byte, coordinates_size> coordinates,
    const PrivateKey& private_key, Secret& secret) noexcept -> bool {
  const auto* point = reinterpret_cast<const std::uint8_t*>(coordinates.data());
  const auto* scalar =
      reinterpret_cast<const std::uint8_t*>(private_key.data());
  auto* output = reinterpret_cast<std::uint8_t*>(secret.data());
  if constexpr (Curve == EcdhCurve::secp256k1)
    return uECC_shared_secret(point, scalar, output, uECC_secp256k1()) == 1;
  else if constexpr (Curve == EcdhCurve::secp256r1)
    return uECC_shared_secret(point, scalar, output, uECC_secp256r1()) == 1;
  else
    return uECC_p521_shared_secret(point, scalar, output,
                                   uECC_p521_secp521r1()) == 1;
}

template <EcdhCurve Curve>
void KeyPair<Curve>::restrict_to_order_width(
    PrivateKey& private_key) noexcept {
  if constexpr (Curve == EcdhCurve::secp521r1)
    private_key[0] &= std::byte{0x01};
}

template class KeyPair<EcdhCurve::secp256k1>;
template class KeyPair<EcdhCurve::secp521r1>;
template class KeyPair<EcdhCurve::secp256r1>;

}  // namespace libchiaking::platform::micro_ecc
