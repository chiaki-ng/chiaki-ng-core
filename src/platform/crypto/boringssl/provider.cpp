#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/crypto/evp/primitives.hpp"

#include <libchiaking/platform/crypto/boringssl/provider.hpp>

#include <libchiaking/detail/secret_array.hpp>

#include <openssl/base.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdh.h>
#include <openssl/err.h>
#include <openssl/nid.h>

#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

#ifndef OPENSSL_IS_BORINGSSL
#error "LIBCHIAKING_CRYPTO_BORINGSSL requires BoringSSL, not OpenSSL"
#endif

namespace libchiaking::platform::boringssl {

class BoringSslCryptoProvider::EcKeyPair final : public EcdhKeyPair {
 public:
  using Key = memory::CHandle<EC_KEY, EC_KEY_free>;

  EcKeyPair(Key&& key, EcdhCurve curve) noexcept
      : key_(std::move(key)), curve_(curve) {}

  [[nodiscard]] static auto nid_of(EcdhCurve curve) noexcept
      -> std::optional<int> {
    switch (curve) {
      case EcdhCurve::secp256r1:
        return NID_X9_62_prime256v1;
      case EcdhCurve::secp521r1:
        return NID_secp521r1;
      case EcdhCurve::secp256k1:
        break;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto public_coordinates(std::span<std::byte> output) const
      -> Expected<void, CryptoError> override {
    const auto size = ecdh_public_coordinates_size(curve_);
    if (output.size() != size)
      return libchiaking::unexpected(CryptoError::invalid_argument);
    std::array<unsigned char, sec1_uncompressed_point_prefix +
                                  max_ecdh_public_coordinates_size>
        point{};
    const auto written = EC_POINT_point2oct(
        EC_KEY_get0_group(key_.get()), EC_KEY_get0_public_key(key_.get()),
        POINT_CONVERSION_UNCOMPRESSED, point.data(), point.size(), nullptr);
    if (written != sec1_uncompressed_point_prefix + size) return failure();
    for (std::size_t i = 0; i < size; ++i)
      output[i] = std::byte{point[i + sec1_uncompressed_point_prefix]};
    return {};
  }

  [[nodiscard]] auto derive_secret(std::span<const std::byte> peer_coordinates,
                                   std::span<std::byte> secret) const
      -> Expected<void, CryptoError> override {
    const auto coordinates_size = ecdh_public_coordinates_size(curve_);
    const auto secret_size = ecdh_shared_secret_size(curve_);
    if (peer_coordinates.size() != coordinates_size ||
        secret.size() != secret_size)
      return libchiaking::unexpected(CryptoError::invalid_argument);
    std::array<unsigned char, sec1_uncompressed_point_prefix +
                                  max_ecdh_public_coordinates_size>
        encoded{};
    encoded[0] = sec1_uncompressed_tag;
    for (std::size_t i = 0; i < coordinates_size; ++i)
      encoded[i + sec1_uncompressed_point_prefix] =
          std::to_integer<unsigned char>(peer_coordinates[i]);
    const auto* group = EC_KEY_get0_group(key_.get());
    memory::CHandle<EC_POINT, EC_POINT_free> peer{EC_POINT_new(group)};
    if (!peer) return failure();
    if (EC_POINT_oct2point(group, peer.get(), encoded.data(),
                           sec1_uncompressed_point_prefix + coordinates_size,
                           nullptr) != 1) {
      ERR_clear_error();
      return libchiaking::unexpected(CryptoError::invalid_argument);
    }
    SecretArray<max_ecdh_shared_secret_size> staging;
    const auto derived = ECDH_compute_key(staging.data(), secret_size,
                                          peer.get(), key_.get(), nullptr);
    if (derived != static_cast<int>(secret_size)) return failure();
    for (std::size_t i = 0; i < secret_size; ++i) secret[i] = staging[i];
    return {};
  }

  [[nodiscard]] static auto failure() -> Expected<void, CryptoError> {
    ERR_clear_error();
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

 private:
  Key key_;
  EcdhCurve curve_;
};

auto BoringSslCryptoProvider::sha256(std::span<const std::byte> input,
                                     std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  return evp::Primitives::sha256(input, digest);
}

auto BoringSslCryptoProvider::hmac_sha256(std::span<const std::byte> key,
                                          std::span<const std::byte> input,
                                          std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  return evp::Primitives::hmac_sha256(key, input, digest);
}

auto BoringSslCryptoProvider::aes128_encrypt_block(
    std::span<const std::byte> key, std::span<const std::byte> block,
    std::span<std::byte> output) -> Expected<void, CryptoError> {
  return evp::Primitives::aes128_encrypt_block(key, block, output);
}

auto BoringSslCryptoProvider::aes128_encrypt_blocks(
    std::span<const std::byte> key, std::span<const std::byte> blocks,
    std::span<std::byte> output) -> Expected<void, CryptoError> {
  return evp::Primitives::aes128_encrypt_blocks(key, blocks, output);
}

auto BoringSslCryptoProvider::aes128_gmac(std::span<const std::byte> key,
                                          std::span<const std::byte> iv,
                                          std::span<const std::byte> aad,
                                          std::span<std::byte> tag)
    -> Expected<void, CryptoError> {
  return evp::Primitives::aes128_gmac(key, iv, aad, tag);
}

auto BoringSslCryptoProvider::hkdf(HkdfDigest digest,
                                   std::span<const std::byte> salt,
                                   std::span<const std::byte> input,
                                   std::span<const std::byte> info,
                                   std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  return evp::Primitives::hkdf(digest, salt, input, info, output);
}

auto BoringSslCryptoProvider::generate_ecdh(EcdhCurve curve)
    -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
  if (curve == EcdhCurve::secp256k1)
    return libchiaking::unexpected(CryptoError::unsupported_capability);
  const auto nid = EcKeyPair::nid_of(curve);
  if (!nid) return libchiaking::unexpected(CryptoError::invalid_argument);
  EcKeyPair::Key key{EC_KEY_new_by_curve_name(*nid)};
  if (!key || EC_KEY_generate_key(key.get()) != 1) {
    ERR_clear_error();
    return libchiaking::unexpected(CryptoError::backend_failure);
  }
  std::unique_ptr<EcdhKeyPair> pair{new (std::nothrow)
                                        EcKeyPair(std::move(key), curve)};
  if (!pair) return libchiaking::unexpected(CryptoError::backend_failure);
  return pair;
}

}  // namespace libchiaking::platform::boringssl
