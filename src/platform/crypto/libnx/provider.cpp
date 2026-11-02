#include <libchiaking/platform/crypto/libnx/provider.hpp>

#include "src/platform/crypto/armv8/gmac.hpp"
#include "src/platform/crypto/micro_ecc/key_pair.hpp"

#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/detail/secure_zero.hpp>

#include <switch.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>

namespace libchiaking::platform::libnx {

class LibnxCryptoProvider::HardwareCipher final : public Aes128Cipher {
 public:
  explicit HardwareCipher(
      std::span<const std::byte, aes128_key_size> key) noexcept
      : context_(expanded(key)), gmac_(hash_key(context_).span()) {}

  ~HardwareCipher() override {
    memory::secure_zero(std::as_writable_bytes(std::span{&context_, 1}));
  }

  HardwareCipher(const HardwareCipher&) = delete;
  HardwareCipher& operator=(const HardwareCipher&) = delete;

  void encrypt(std::span<const std::byte, aes128_block_size> block,
               std::span<std::byte, aes128_block_size> output) const noexcept {
    ::aes128EncryptBlock(&context_, output.data(), block.data());
  }

  [[nodiscard]] auto encrypt_blocks(std::span<const std::byte> blocks,
                                    std::span<std::byte> output) const
      -> Expected<void, CryptoError> override {
    if (blocks.size() % aes128_block_size != 0 ||
        output.size() != blocks.size())
      return libchiaking::unexpected(CryptoError::invalid_argument);
    for (std::size_t offset = 0; offset < blocks.size();
         offset += aes128_block_size)
      encrypt(blocks.subspan(offset).first<aes128_block_size>(),
              output.subspan(offset).first<aes128_block_size>());
    return {};
  }

  [[nodiscard]] auto gmac(std::span<const std::byte> iv,
                          std::span<const std::byte> aad,
                          std::span<std::byte> tag) const
      -> Expected<void, CryptoError> override {
    if (iv.size() < aes128_gcm_iv_size_min ||
        iv.size() > aes128_gcm_iv_size_max ||
        tag.size() < aes128_gcm_tag_size_min ||
        tag.size() > aes128_gcm_tag_size_max)
      return libchiaking::unexpected(CryptoError::invalid_argument);
    const auto counter = gmac_.counter_block(iv);
    SecretArray<aes128_block_size> encrypted_counter;
    encrypt(counter, encrypted_counter.span());
    gmac_.authenticate(aad, encrypted_counter.span(), tag);
    return {};
  }

 private:
  static auto expanded(std::span<const std::byte, aes128_key_size> key) noexcept
      -> ::Aes128Context {
    ::Aes128Context context{};
    ::aes128ContextCreate(&context, key.data(), true);
    return context;
  }

  static auto hash_key(const ::Aes128Context& context) noexcept
      -> SecretArray<aes128_block_size> {
    constexpr armv8::PmullGmac::Block zero_block{};
    SecretArray<aes128_block_size> key;
    ::aes128EncryptBlock(&context, key.span().data(), zero_block.data());
    return key;
  }

  ::Aes128Context context_;
  armv8::PmullGmac gmac_;
};

LibnxCryptoProvider::LibnxCryptoProvider(SecureRandomProvider& random) noexcept
    : random_(&random) {}

auto LibnxCryptoProvider::sha256(std::span<const std::byte> input,
                                 std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  if (digest.size() != sha256_digest_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  ::sha256CalculateHash(digest.data(), input.data(), input.size());
  return {};
}

auto LibnxCryptoProvider::hmac_sha256(std::span<const std::byte> key,
                                      std::span<const std::byte> input,
                                      std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  if (digest.size() != hmac_sha256_digest_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  ::hmacSha256CalculateMac(digest.data(), key.data(), key.size(), input.data(),
                           input.size());
  return {};
}

auto LibnxCryptoProvider::aes128_encrypt_block(std::span<const std::byte> key,
                                               std::span<const std::byte> block,
                                               std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  if (key.size() != aes128_key_size || block.size() != aes128_block_size ||
      output.size() != aes128_block_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  const HardwareCipher cipher{key.first<aes128_key_size>()};
  SecretArray<aes128_block_size> staging;
  cipher.encrypt(block.first<aes128_block_size>(), staging.span());
  std::ranges::copy(staging, output.begin());
  return {};
}

auto LibnxCryptoProvider::aes128_encrypt_blocks(
    std::span<const std::byte> key, std::span<const std::byte> blocks,
    std::span<std::byte> output) -> Expected<void, CryptoError> {
  if (key.size() != aes128_key_size || blocks.size() % aes128_block_size != 0 ||
      output.size() != blocks.size())
    return libchiaking::unexpected(CryptoError::invalid_argument);
  return HardwareCipher{key.first<aes128_key_size>()}.encrypt_blocks(blocks,
                                                                     output);
}

auto LibnxCryptoProvider::aes128_gmac(std::span<const std::byte> key,
                                      std::span<const std::byte> iv,
                                      std::span<const std::byte> aad,
                                      std::span<std::byte> tag)
    -> Expected<void, CryptoError> {
  if (key.size() != aes128_key_size || iv.size() < aes128_gcm_iv_size_min ||
      iv.size() > aes128_gcm_iv_size_max ||
      tag.size() < aes128_gcm_tag_size_min ||
      tag.size() > aes128_gcm_tag_size_max)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  return HardwareCipher{key.first<aes128_key_size>()}.gmac(iv, aad, tag);
}

auto LibnxCryptoProvider::aes128_cipher(std::span<const std::byte> key)
    -> Expected<std::unique_ptr<Aes128Cipher>, CryptoError> {
  if (key.size() != aes128_key_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  try {
    return std::unique_ptr<Aes128Cipher>{
        std::make_unique<HardwareCipher>(key.first<aes128_key_size>())};
  } catch (...) {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }
}

auto LibnxCryptoProvider::generate_ecdh(EcdhCurve curve)
    -> Expected<std::unique_ptr<EcdhKeyPair>, CryptoError> {
  switch (curve) {
    case EcdhCurve::secp256k1:
      return micro_ecc::KeyPair<EcdhCurve::secp256k1>::generate(*random_);
    case EcdhCurve::secp521r1:
      return micro_ecc::KeyPair<EcdhCurve::secp521r1>::generate(*random_);
    case EcdhCurve::secp256r1:
      return micro_ecc::KeyPair<EcdhCurve::secp256r1>::generate(*random_);
  }
  return libchiaking::unexpected(CryptoError::invalid_argument);
}

auto LibnxCryptoProvider::hmac_sha1(std::span<const std::byte> key,
                                    std::span<const std::byte> input,
                                    std::span<std::byte> digest)
    -> Expected<void, CryptoError> {
  if (digest.size() != hmac_sha1_digest_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  ::hmacSha1CalculateMac(digest.data(), key.data(), key.size(), input.data(),
                         input.size());
  return {};
}

}  // namespace libchiaking::platform::libnx
