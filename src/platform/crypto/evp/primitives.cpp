#include "src/platform/crypto/evp/primitives.hpp"

#include "src/core/utils/memory/c_handle.hpp"

#include <libchiaking/detail/secret_array.hpp>

#include <openssl/evp.h>

#if defined(OPENSSL_IS_BORINGSSL)
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#else
#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#endif

#include <algorithm>
#include <climits>
#include <cstddef>

namespace libchiaking::platform::evp {

auto Primitives::native(const std::byte* bytes) noexcept -> const
    unsigned char* {
  return reinterpret_cast<const unsigned char*>(bytes);
}

auto Primitives::native(std::byte* bytes) noexcept -> unsigned char* {
  return reinterpret_cast<unsigned char*>(bytes);
}

auto Primitives::invalid() noexcept -> Unexpected<CryptoError> {
  return libchiaking::unexpected(CryptoError::invalid_argument);
}

auto Primitives::failure() noexcept -> Unexpected<CryptoError> {
  return libchiaking::unexpected(CryptoError::backend_failure);
}

auto Primitives::sha256(std::span<const std::byte> input,
                        std::span<std::byte> digest) -> Outcome {
  if (digest.size() != sha256_digest_size) return invalid();
  SecretArray<sha256_digest_size> staged;
  unsigned int size = 0;
  if (EVP_Digest(input.data(), input.size(), native(staged.data()), &size,
                 EVP_sha256(), nullptr) != 1 ||
      size != sha256_digest_size)
    return failure();
  std::ranges::copy(staged, digest.begin());
  return {};
}

auto Primitives::hmac_sha256(std::span<const std::byte> key,
                             std::span<const std::byte> input,
                             std::span<std::byte> digest) -> Outcome {
  if (digest.size() != hmac_sha256_digest_size) return invalid();
  SecretArray<hmac_sha256_digest_size> staged;
#if defined(OPENSSL_IS_BORINGSSL)
  unsigned int size = 0;
  if (HMAC(EVP_sha256(), key.data(), key.size(), native(input.data()),
           input.size(), native(staged.data()), &size) == nullptr ||
      size != hmac_sha256_digest_size)
    return failure();
#else
  auto mac = memory::CHandle<EVP_MAC, EVP_MAC_free>(
      EVP_MAC_fetch(nullptr, "HMAC", nullptr));
  if (!mac) return failure();
  auto context = memory::CHandle<EVP_MAC_CTX, EVP_MAC_CTX_free>(
      EVP_MAC_CTX_new(mac.get()));
  if (!context) return failure();
  char digest_name[] = "SHA256";
  const OSSL_PARAM parameters[]{
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
      OSSL_PARAM_construct_end(),
  };
  std::size_t size = 0;
  if (EVP_MAC_init(context.get(), native(key.data()), key.size(), parameters) !=
          1 ||
      EVP_MAC_update(context.get(), native(input.data()), input.size()) != 1 ||
      EVP_MAC_final(context.get(), native(staged.data()), &size,
                    staged.size()) != 1 ||
      size != hmac_sha256_digest_size)
    return failure();
#endif
  std::ranges::copy(staged, digest.begin());
  return {};
}

auto Primitives::aes128_encrypt_block(std::span<const std::byte> key,
                                      std::span<const std::byte> block,
                                      std::span<std::byte> output) -> Outcome {
  if (key.size() != aes128_key_size || block.size() != aes128_block_size ||
      output.size() != aes128_block_size)
    return invalid();
  SecretArray<aes128_block_size> staged;
  if (auto encrypted = aes128_encrypt_blocks(key, block, staged.span());
      !encrypted)
    return encrypted;
  std::ranges::copy(staged, output.begin());
  return {};
}

auto Primitives::aes128_encrypt_blocks(std::span<const std::byte> key,
                                       std::span<const std::byte> blocks,
                                       std::span<std::byte> output) -> Outcome {
  if (key.size() != aes128_key_size || blocks.size() % aes128_block_size != 0 ||
      output.size() != blocks.size() ||
      blocks.size() > static_cast<std::size_t>(INT_MAX))
    return invalid();
  if (blocks.empty()) return {};
  auto context = memory::CHandle<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>(
      EVP_CIPHER_CTX_new());
  if (!context) return failure();
  const auto size = static_cast<int>(blocks.size());
  int written = 0;
  int finished = 0;
  if (EVP_EncryptInit_ex(context.get(), EVP_aes_128_ecb(), nullptr,
                         native(key.data()), nullptr) != 1 ||
      EVP_CIPHER_CTX_set_padding(context.get(), 0) != 1 ||
      EVP_EncryptUpdate(context.get(), native(output.data()), &written,
                        native(blocks.data()), size) != 1 ||
      EVP_EncryptFinal_ex(context.get(), native(output.data()) + written,
                          &finished) != 1 ||
      written + finished != size)
    return failure();
  return {};
}

auto Primitives::aes128_gmac(std::span<const std::byte> key,
                             std::span<const std::byte> iv,
                             std::span<const std::byte> aad,
                             std::span<std::byte> tag) -> Outcome {
  if (key.size() != aes128_key_size || iv.size() < aes128_gcm_iv_size_min ||
      iv.size() > aes128_gcm_iv_size_max ||
      aad.size() > static_cast<std::size_t>(INT_MAX) ||
      tag.size() < aes128_gcm_tag_size_min ||
      tag.size() > aes128_gcm_tag_size_max)
    return invalid();
  auto context = memory::CHandle<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>(
      EVP_CIPHER_CTX_new());
  if (!context) return failure();
  SecretArray<aes128_gcm_tag_size> staged;
  int unused = 0;
  if (EVP_EncryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN,
                          static_cast<int>(iv.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, native(key.data()),
                         native(iv.data())) != 1 ||
      (!aad.empty() &&
       EVP_EncryptUpdate(context.get(), nullptr, &unused, native(aad.data()),
                         static_cast<int>(aad.size())) != 1) ||
      EVP_EncryptFinal_ex(context.get(), native(staged.data()), &unused) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG,
                          static_cast<int>(tag.size()), staged.data()) != 1)
    return failure();
  std::ranges::copy(staged.span().first(tag.size()), tag.begin());
  return {};
}

auto Primitives::hkdf(HkdfDigest digest, std::span<const std::byte> salt,
                      std::span<const std::byte> input,
                      std::span<const std::byte> info,
                      std::span<std::byte> output) -> Outcome {
  const auto block_size = hkdf_block_size(digest);
  if (block_size == 0 || output.empty() ||
      output.size() > hkdf_max_blocks * block_size)
    return invalid();
#if defined(OPENSSL_IS_BORINGSSL)
  const auto* algorithm =
      digest == HkdfDigest::sha256 ? EVP_sha256() : EVP_sha512();
  if (HKDF(native(output.data()), output.size(), algorithm,
           native(input.data()), input.size(), native(salt.data()), salt.size(),
           native(info.data()), info.size()) != 1)
    return failure();
#else
  auto kdf = memory::CHandle<EVP_KDF, EVP_KDF_free>(
      EVP_KDF_fetch(nullptr, "HKDF", nullptr));
  if (!kdf) return failure();
  auto context = memory::CHandle<EVP_KDF_CTX, EVP_KDF_CTX_free>(
      EVP_KDF_CTX_new(kdf.get()));
  if (!context) return failure();
  char sha256_name[] = "SHA256";
  char sha512_name[] = "SHA512";
  auto* name = digest == HkdfDigest::sha256 ? sha256_name : sha512_name;
  std::byte empty{};
  auto* key = input.empty() ? &empty : const_cast<std::byte*>(input.data());
  auto* seed = const_cast<std::byte*>(salt.data());
  auto* bind = const_cast<std::byte*>(info.data());
  OSSL_PARAM parameters[5]{};
  std::size_t count = 0;
  parameters[count++] =
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, name, 0);
  parameters[count++] =
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, key, input.size());
  if (!salt.empty())
    parameters[count++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                            seed, salt.size());
  if (!info.empty())
    parameters[count++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                                            bind, info.size());
  parameters[count] = OSSL_PARAM_construct_end();
  if (EVP_KDF_derive(context.get(), native(output.data()), output.size(),
                     parameters) != 1)
    return failure();
#endif
  return {};
}

}  // namespace libchiaking::platform::evp
