#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/crypto/evp/primitives.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>

#include <libchiaking/detail/secret_array.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <algorithm>

#include <array>
#include <climits>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::platform::openssl {
class OpenSslSessionPrimitives {
 public:
  class ErrorQueueMark {
   public:
    ErrorQueueMark() {
      if (ERR_peek_last_error() != 0) {
        marked_ = ERR_set_mark() == 1;
        return;
      }

      ERR_raise(ERR_LIB_NONE, ERR_R_INTERNAL_ERROR);
      marked_ = ERR_set_mark() == 1;
      if (marked_)
        seeded_ = true;
      else
        (void)ERR_get_error();
    }

    ~ErrorQueueMark() {
      if (marked_) ERR_pop_to_mark();
      if (seeded_) (void)ERR_get_error();
    }

    ErrorQueueMark(const ErrorQueueMark&) = delete;
    ErrorQueueMark& operator=(const ErrorQueueMark&) = delete;

   private:
    bool marked_ = false;
    bool seeded_ = false;
  };

 private:
  template <std::size_t Size>
  static auto raw(SecretArray<Size>& secret) noexcept -> unsigned char* {
    return reinterpret_cast<unsigned char*>(secret.data());
  }

  static Expected<void, CryptoError> failure() {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  [[nodiscard]] static auto ecdh_group(EcdhCurve curve) noexcept
      -> std::optional<std::string_view> {
    switch (curve) {
      case EcdhCurve::secp256k1:
        return "secp256k1";
      case EcdhCurve::secp521r1:
        return "P-521";
      case EcdhCurve::secp256r1:
        return "P-256";
    }
    return std::nullopt;
  }

  using Pkey = memory::CHandle<EVP_PKEY, EVP_PKEY_free>;

  class EvpEcdhKeyPair final : public EcdhKeyPair {
   public:
    EvpEcdhKeyPair(Pkey&& key, EcdhCurve curve) noexcept
        : key_(std::move(key)), curve_(curve) {}

    [[nodiscard]] auto public_coordinates(std::span<std::byte> output) const
        -> Expected<void, CryptoError> override {
      ErrorQueueMark mark;
      const auto size = ecdh_public_coordinates_size(curve_);
      if (output.size() != size)
        return libchiaking::unexpected(CryptoError::invalid_argument);
      BIGNUM* x_raw = nullptr;
      BIGNUM* y_raw = nullptr;
      const bool fetched =
          EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_X, &x_raw) ==
              1 &&
          EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_Y, &y_raw) ==
              1;
      memory::CHandle<BIGNUM, BN_clear_free> x(x_raw);
      memory::CHandle<BIGNUM, BN_clear_free> y(y_raw);
      if (!fetched) return failure();
      SecretArray<max_ecdh_public_coordinates_size> staging;
      const auto half = static_cast<int>(size / 2);
      if (BN_bn2binpad(x.get(), raw(staging), half) != half ||
          BN_bn2binpad(y.get(), raw(staging) + half, half) != half)
        return failure();
      for (std::size_t i = 0; i < size; ++i) output[i] = staging[i];
      return {};
    }

    [[nodiscard]] auto derive_secret(
        std::span<const std::byte> peer_coordinates,
        std::span<std::byte> secret) const
        -> Expected<void, CryptoError> override {
      ErrorQueueMark mark;
      const auto coordinates_size = ecdh_public_coordinates_size(curve_);
      const auto secret_size = ecdh_shared_secret_size(curve_);
      if (peer_coordinates.size() != coordinates_size ||
          secret.size() != secret_size)
        return libchiaking::unexpected(CryptoError::invalid_argument);

      std::array<unsigned char, sec1_uncompressed_point_prefix +
                                    max_ecdh_public_coordinates_size>
          point{};
      point[0] = sec1_uncompressed_tag;
      for (std::size_t i = 0; i < coordinates_size; ++i)
        point[i + sec1_uncompressed_point_prefix] =
            static_cast<unsigned char>(peer_coordinates[i]);
      auto from = memory::CHandle<EVP_PKEY_CTX, EVP_PKEY_CTX_free>(
          EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
      if (!from || EVP_PKEY_fromdata_init(from.get()) != 1) return failure();
      auto* group = const_cast<char*>(ecdh_group(curve_)->data());
      OSSL_PARAM params[]{
          OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group,
                                           0),
          OSSL_PARAM_construct_octet_string(
              OSSL_PKEY_PARAM_PUB_KEY, point.data(),
              sec1_uncompressed_point_prefix + coordinates_size),
          OSSL_PARAM_construct_end(),
      };
      EVP_PKEY* peer_raw = nullptr;
      const auto imported =
          EVP_PKEY_fromdata(from.get(), &peer_raw, EVP_PKEY_PUBLIC_KEY, params);
      memory::CHandle<EVP_PKEY, EVP_PKEY_free> peer(peer_raw);
      if (imported != 1)
        return libchiaking::unexpected(CryptoError::invalid_argument);
      auto check = memory::CHandle<EVP_PKEY_CTX, EVP_PKEY_CTX_free>(
          EVP_PKEY_CTX_new_from_pkey(nullptr, peer.get(), nullptr));
      if (!check) return failure();
      if (EVP_PKEY_public_check(check.get()) != 1)
        return libchiaking::unexpected(CryptoError::invalid_argument);

      auto derive = memory::CHandle<EVP_PKEY_CTX, EVP_PKEY_CTX_free>(
          EVP_PKEY_CTX_new_from_pkey(nullptr, key_.get(), nullptr));
      SecretArray<max_ecdh_shared_secret_size> staging;
      std::size_t derived_size = secret_size;
      if (!derive || EVP_PKEY_derive_init(derive.get()) != 1 ||
          EVP_PKEY_derive_set_peer(derive.get(), peer.get()) != 1 ||
          EVP_PKEY_derive(derive.get(), raw(staging), &derived_size) != 1 ||
          derived_size != secret_size)
        return failure();
      for (std::size_t i = 0; i < secret_size; ++i) secret[i] = staging[i];
      return {};
    }

   private:
    Pkey key_;
    EcdhCurve curve_;
  };

 public:
  static auto generate_ecdh(EcdhCurve curve)
      -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
    ErrorQueueMark mark;
    const auto group = ecdh_group(curve);
    if (!group) return libchiaking::unexpected(CryptoError::invalid_argument);
    Pkey key{EVP_EC_gen(group->data())};
    if (!key) return libchiaking::unexpected(CryptoError::backend_failure);
    try {
      return std::unique_ptr<EcdhKeyPair>{
          new EvpEcdhKeyPair(std::move(key), curve)};
    } catch (...) {
      return libchiaking::unexpected(CryptoError::backend_failure);
    }
  }
};

auto OpenSslCryptoProvider::sha256(std::span<const std::byte> input,
                                   std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::sha256(input, digest);
}

auto OpenSslCryptoProvider::hmac_sha256(std::span<const std::byte> key,
                                        std::span<const std::byte> input,
                                        std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::hmac_sha256(key, input, digest);
}

auto OpenSslCryptoProvider::aes128_encrypt_block(
    std::span<const std::byte> key, std::span<const std::byte> block,
    std::span<std::byte> output) -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::aes128_encrypt_block(key, block, output);
}

auto OpenSslCryptoProvider::aes128_encrypt_blocks(
    std::span<const std::byte> key, std::span<const std::byte> blocks,
    std::span<std::byte> output) -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::aes128_encrypt_blocks(key, blocks, output);
}

auto OpenSslCryptoProvider::aes128_gmac(std::span<const std::byte> key,
                                        std::span<const std::byte> iv,
                                        std::span<const std::byte> aad,
                                        std::span<std::byte> tag)
    -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::aes128_gmac(key, iv, aad, tag);
}

auto OpenSslCryptoProvider::hkdf(HkdfDigest digest,
                                 std::span<const std::byte> salt,
                                 std::span<const std::byte> input,
                                 std::span<const std::byte> info,
                                 std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  const OpenSslSessionPrimitives::ErrorQueueMark mark;
  return evp::Primitives::hkdf(digest, salt, input, info, output);
}

auto OpenSslCryptoProvider::generate_ecdh(EcdhCurve curve)
    -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
  return OpenSslSessionPrimitives::generate_ecdh(curve);
}

}  // namespace libchiaking::platform::openssl
