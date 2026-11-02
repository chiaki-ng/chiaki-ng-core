#include "src/core/utils/memory/c_handle.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/params.h>
#include <libchiaking/detail/secret_array.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace libchiaking::platform::openssl {
class OpenSslPairingPrimitives {
 private:
  static constexpr std::uint64_t scrypt_memory_limit =
      64ULL * 1024ULL * 1024ULL;

  class ErrorScope {
   public:
    ErrorScope() noexcept { ERR_set_mark(); }
    ~ErrorScope() { ERR_pop_to_mark(); }
    ErrorScope(const ErrorScope&) = delete;
    ErrorScope& operator=(const ErrorScope&) = delete;
  };

  using Bn = memory::CHandle<BIGNUM, BN_clear_free>;
  using BnContext = memory::CHandle<BN_CTX, BN_CTX_free>;
  using Group = memory::CHandle<EC_GROUP, EC_GROUP_free>;
  using Point = memory::CHandle<EC_POINT, EC_POINT_clear_free>;
  using Pkey = memory::CHandle<EVP_PKEY, EVP_PKEY_free>;

  [[nodiscard]] static auto invalid() -> Unexpected<CryptoError> {
    return libchiaking::unexpected(CryptoError::invalid_argument);
  }

  [[nodiscard]] static auto failure() -> Unexpected<CryptoError> {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }

  static auto raw(std::span<const std::byte> bytes) noexcept -> const
      unsigned char* {
    return reinterpret_cast<const unsigned char*>(bytes.data());
  }

  static auto raw(std::span<std::byte> bytes) noexcept -> unsigned char* {
    return reinterpret_cast<unsigned char*>(bytes.data());
  }

  class Curve {
   public:
    [[nodiscard]] static auto create() -> Expected<Curve, CryptoError> {
      Group group{EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)};
      BnContext context{BN_CTX_new()};
      if (!group || !context) return failure();
      return Curve{std::move(group), std::move(context)};
    }

    auto group() const noexcept -> EC_GROUP* { return group_.get(); }
    auto context() const noexcept -> BN_CTX* { return context_.get(); }

    Curve(Curve&&) noexcept = default;
    auto operator=(Curve&&) noexcept -> Curve& = default;
    Curve(const Curve&) = delete;
    auto operator=(const Curve&) -> Curve& = delete;

   private:
    Curve(Group group, BnContext context) noexcept
        : group_{std::move(group)}, context_{std::move(context)} {}

    Group group_;
    BnContext context_;
  };

  [[nodiscard]] static auto load_scalar(std::span<const std::byte> scalar)
      -> Expected<Bn, CryptoError> {
    Bn value{BN_bin2bn(raw(scalar), static_cast<int>(scalar.size()), nullptr)};
    if (!value) return failure();
    BN_set_flags(value.get(), BN_FLG_CONSTTIME);
    return value;
  }

  [[nodiscard]] static auto load_point(const Curve& curve,
                                       std::span<const std::byte> encoded)
      -> Expected<Point, CryptoError> {
    if (encoded.size() != p256_point_size ||
        std::to_integer<std::uint8_t>(encoded[0]) != sec1_uncompressed_tag)
      return invalid();
    Point point{EC_POINT_new(curve.group())};
    if (!point ||
        EC_POINT_oct2point(curve.group(), point.get(), raw(encoded),
                           encoded.size(), curve.context()) != 1 ||
        EC_POINT_is_at_infinity(curve.group(), point.get()) == 1 ||
        EC_POINT_is_on_curve(curve.group(), point.get(), curve.context()) != 1)
      return invalid();
    return point;
  }

  [[nodiscard]] static auto store_point(const Curve& curve,
                                        const EC_POINT* point,
                                        std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    if (EC_POINT_is_at_infinity(curve.group(), point) == 1) return invalid();
    SecretArray<p256_point_size> staging{};
    if (EC_POINT_point2oct(
            curve.group(), point, POINT_CONVERSION_UNCOMPRESSED,
            raw(std::span<std::byte>{staging.data(), staging.size()}),
            staging.size(), curve.context()) != staging.size())
      return failure();
    std::ranges::copy(staging, output.begin());
    return {};
  }

  [[nodiscard]] static auto digest(const EVP_MD* algorithm, std::size_t size,
                                   std::span<const std::byte> input,
                                   std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    if (output.size() != size) return invalid();
    unsigned int written = 0;
    if (EVP_Digest(input.data(), input.size(), raw(output), &written, algorithm,
                   nullptr) != 1 ||
        written != size)
      return failure();
    return {};
  }

  [[nodiscard]] static auto keyed_digest(char* digest_name,
                                         std::size_t digest_size,
                                         std::span<const std::byte> key,
                                         std::span<const std::byte> input,
                                         std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (output.size() != digest_size) return invalid();
    memory::CHandle<EVP_MAC, EVP_MAC_free> mac{
        EVP_MAC_fetch(nullptr, "HMAC", nullptr)};
    if (!mac) return failure();
    memory::CHandle<EVP_MAC_CTX, EVP_MAC_CTX_free> context{
        EVP_MAC_CTX_new(mac.get())};
    if (!context) return failure();
    OSSL_PARAM params[2];
    params[0] =
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0);
    params[1] = OSSL_PARAM_construct_end();
    const unsigned char empty_key = 0;
    std::size_t written = 0;
    if (EVP_MAC_init(context.get(), key.empty() ? &empty_key : raw(key),
                     key.size(), params) != 1 ||
        EVP_MAC_update(context.get(), raw(input), input.size()) != 1 ||
        EVP_MAC_final(context.get(), raw(output), &written, output.size()) !=
            1 ||
        written != digest_size)
      return failure();
    return {};
  }

 public:
  static auto sha512(std::span<const std::byte> input,
                     std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    return digest(EVP_sha512(), sha512_digest_size, input, output);
  }

  static auto hmac_sha512(std::span<const std::byte> key,
                          std::span<const std::byte> input,
                          std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    char digest_name[] = "SHA512";
    return keyed_digest(digest_name, hmac_sha512_digest_size, key, input,
                        output);
  }

  static auto hmac_sha1(std::span<const std::byte> key,
                        std::span<const std::byte> input,
                        std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    char digest_name[] = "SHA1";
    return keyed_digest(digest_name, hmac_sha1_digest_size, key, input, output);
  }

  static auto scrypt(std::span<const std::byte> password,
                     std::span<const std::byte> salt,
                     ScryptParameters parameters, std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (output.empty() || parameters.cost < 2 ||
        (parameters.cost & (parameters.cost - 1)) != 0 ||
        parameters.block_size == 0 || parameters.parallelization == 0)
      return invalid();
    if (EVP_PBE_scrypt(reinterpret_cast<const char*>(password.data()),
                       password.size(), raw(salt), salt.size(), parameters.cost,
                       parameters.block_size, parameters.parallelization,
                       scrypt_memory_limit, raw(output), output.size()) != 1)
      return failure();
    return {};
  }

  static auto ed25519_public_key(std::span<const std::byte> seed,
                                 std::span<std::byte> public_key)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (seed.size() != ed25519_seed_size ||
        public_key.size() != ed25519_public_key_size)
      return invalid();
    Pkey key{EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw(seed),
                                          seed.size())};
    std::size_t written = public_key.size();
    if (!key ||
        EVP_PKEY_get_raw_public_key(key.get(), raw(public_key), &written) !=
            1 ||
        written != ed25519_public_key_size)
      return failure();
    return {};
  }

  static auto ed25519_sign(std::span<const std::byte> seed,
                           std::span<const std::byte> message,
                           std::span<std::byte> signature)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (seed.size() != ed25519_seed_size ||
        signature.size() != ed25519_signature_size)
      return invalid();
    Pkey key{EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw(seed),
                                          seed.size())};
    memory::CHandle<EVP_MD_CTX, EVP_MD_CTX_free> context{EVP_MD_CTX_new()};
    std::size_t written = signature.size();
    if (!key || !context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr,
                           key.get()) != 1 ||
        EVP_DigestSign(context.get(), raw(signature), &written, raw(message),
                       message.size()) != 1 ||
        written != ed25519_signature_size)
      return failure();
    return {};
  }

  static auto ed25519_verify(std::span<const std::byte> public_key,
                             std::span<const std::byte> message,
                             std::span<const std::byte> signature)
      -> Expected<bool, CryptoError> {
    ErrorScope scope;
    if (public_key.size() != ed25519_public_key_size ||
        signature.size() != ed25519_signature_size)
      return invalid();
    Pkey key{EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                         raw(public_key), public_key.size())};
    if (!key) return false;
    memory::CHandle<EVP_MD_CTX, EVP_MD_CTX_free> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr,
                                         nullptr, key.get()) != 1)
      return failure();
    return EVP_DigestVerify(context.get(), raw(signature), signature.size(),
                            raw(message), message.size()) == 1;
  }

  static auto p256_base_multiply(std::span<const std::byte> scalar,
                                 std::span<std::byte> point)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (scalar.size() != p256_scalar_size || point.size() != p256_point_size)
      return invalid();
    const auto created = Curve::create();
    if (!created) return failure();
    const auto& curve = created.value();
    const auto factor = load_scalar(scalar);
    if (!factor) return failure();
    Point product{EC_POINT_new(curve.group())};
    if (!product ||
        EC_POINT_mul(curve.group(), product.get(), factor.value().get(),
                     nullptr, nullptr, curve.context()) != 1)
      return failure();
    return store_point(curve, product.get(), point);
  }

  static auto p256_multiply(std::span<const std::byte> scalar,
                            std::span<const std::byte> point,
                            std::span<std::byte> product)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (scalar.size() != p256_scalar_size || product.size() != p256_point_size)
      return invalid();
    const auto created = Curve::create();
    if (!created) return failure();
    const auto& curve = created.value();
    const auto factor = load_scalar(scalar);
    if (!factor) return failure();
    const auto base = load_point(curve, point);
    if (!base) return invalid();
    Point result{EC_POINT_new(curve.group())};
    if (!result ||
        EC_POINT_mul(curve.group(), result.get(), nullptr, base.value().get(),
                     factor.value().get(), curve.context()) != 1)
      return failure();
    return store_point(curve, result.get(), product);
  }

  static auto p256_add(std::span<const std::byte> left,
                       std::span<const std::byte> right,
                       std::span<std::byte> sum)
      -> Expected<void, CryptoError> {
    ErrorScope scope;
    if (sum.size() != p256_point_size) return invalid();
    const auto created = Curve::create();
    if (!created) return failure();
    const auto& curve = created.value();
    const auto first = load_point(curve, left);
    const auto second = load_point(curve, right);
    if (!first || !second) return invalid();
    Point result{EC_POINT_new(curve.group())};
    if (!result ||
        EC_POINT_add(curve.group(), result.get(), first.value().get(),
                     second.value().get(), curve.context()) != 1)
      return failure();
    return store_point(curve, result.get(), sum);
  }
};

auto OpenSslCryptoProvider::sha512(std::span<const std::byte> input,
                                   std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::sha512(input, output);
}

auto OpenSslCryptoProvider::hmac_sha512(std::span<const std::byte> key,
                                        std::span<const std::byte> input,
                                        std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::hmac_sha512(key, input, output);
}

auto OpenSslCryptoProvider::hmac_sha1(std::span<const std::byte> key,
                                      std::span<const std::byte> input,
                                      std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::hmac_sha1(key, input, output);
}

auto OpenSslCryptoProvider::scrypt(std::span<const std::byte> password,
                                   std::span<const std::byte> salt,
                                   ScryptParameters parameters,
                                   std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::scrypt(password, salt, parameters, output);
}

auto OpenSslCryptoProvider::ed25519_public_key(std::span<const std::byte> seed,
                                               std::span<std::byte> public_key)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::ed25519_public_key(seed, public_key);
}

auto OpenSslCryptoProvider::ed25519_sign(std::span<const std::byte> seed,
                                         std::span<const std::byte> message,
                                         std::span<std::byte> signature)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::ed25519_sign(seed, message, signature);
}

auto OpenSslCryptoProvider::ed25519_verify(
    std::span<const std::byte> public_key, std::span<const std::byte> message,
    std::span<const std::byte> signature) -> Expected<bool, CryptoError> {
  return OpenSslPairingPrimitives::ed25519_verify(public_key, message,
                                                  signature);
}

auto OpenSslCryptoProvider::p256_base_multiply(
    std::span<const std::byte> scalar, std::span<std::byte> point)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::p256_base_multiply(scalar, point);
}

auto OpenSslCryptoProvider::p256_multiply(std::span<const std::byte> scalar,
                                          std::span<const std::byte> point,
                                          std::span<std::byte> product)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::p256_multiply(scalar, point, product);
}

auto OpenSslCryptoProvider::p256_add(std::span<const std::byte> left,
                                     std::span<const std::byte> right,
                                     std::span<std::byte> sum)
    -> Expected<void, CryptoError> {
  return OpenSslPairingPrimitives::p256_add(left, right, sum);
}

}  // namespace libchiaking::platform::openssl
