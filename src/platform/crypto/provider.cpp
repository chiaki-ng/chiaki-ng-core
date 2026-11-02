#include <libchiaking/platform/crypto/provider.hpp>

#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/detail/secret_string.hpp>

#include <algorithm>
#include <array>
#include <memory>

namespace libchiaking::platform {

auto CryptoProvider::hkdf(HkdfDigest digest, std::span<const std::byte> salt,
                          std::span<const std::byte> input,
                          std::span<const std::byte> info,
                          std::span<std::byte> output)
    -> Expected<void, CryptoError> {
  const auto block_size = hkdf_block_size(digest);
  if (block_size == 0 || output.empty() ||
      output.size() > hkdf_max_blocks * block_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  const auto mac = [this, digest](std::span<const std::byte> key,
                                  std::span<const std::byte> message,
                                  std::span<std::byte> tag) {
    return digest == HkdfDigest::sha256 ? hmac_sha256(key, message, tag)
                                        : hmac_sha512(key, message, tag);
  };
  const std::array<std::byte, hmac_sha512_digest_size> zero_salt{};
  SecretArray<hmac_sha512_digest_size> pseudorandom_key;
  SecretArray<hmac_sha512_digest_size> block;
  const auto key = pseudorandom_key.span().first(block_size);
  const auto last = block.span().first(block_size);
  if (auto extracted =
          mac(salt.empty() ? std::span{zero_salt}.first(block_size) : salt,
              input, key);
      !extracted)
    return extracted;
  SecretBytes message;
  std::size_t written = 0;
  for (std::size_t counter = 1; written < output.size(); ++counter) {
    message.clear();
    const std::array trailer{static_cast<std::byte>(counter)};
    if (!message.try_append(counter == 1 ? std::span<const std::byte>{}
                                         : last) ||
        !message.try_append(info) || !message.try_append(trailer))
      return libchiaking::unexpected(CryptoError::backend_failure);
    if (auto expanded = mac(key, message.span(), last); !expanded)
      return expanded;
    const auto count = std::min(block_size, output.size() - written);
    std::ranges::copy(last.first(count), output.subspan(written).begin());
    written += count;
  }
  return {};
}

class CryptoProvider::KeyedAes128Cipher final : public Aes128Cipher {
 public:
  KeyedAes128Cipher(CryptoProvider& provider,
                    std::span<const std::byte, aes128_key_size> key) noexcept
      : provider_(provider) {
    std::ranges::copy(key, key_.begin());
  }

  [[nodiscard]] auto encrypt_blocks(std::span<const std::byte> blocks,
                                    std::span<std::byte> output) const
      -> Expected<void, CryptoError> override {
    return provider_.aes128_encrypt_blocks(key_.span(), blocks, output);
  }

  [[nodiscard]] auto gmac(std::span<const std::byte> iv,
                          std::span<const std::byte> aad,
                          std::span<std::byte> tag) const
      -> Expected<void, CryptoError> override {
    return provider_.aes128_gmac(key_.span(), iv, aad, tag);
  }

 private:
  CryptoProvider& provider_;
  SecretArray<aes128_key_size> key_;
};

auto CryptoProvider::aes128_cipher(std::span<const std::byte> key)
    -> Expected<std::unique_ptr<Aes128Cipher>, CryptoError> {
  if (key.size() != aes128_key_size)
    return libchiaking::unexpected(CryptoError::invalid_argument);
  try {
    return std::unique_ptr<Aes128Cipher>{std::make_unique<KeyedAes128Cipher>(
        *this, key.first<aes128_key_size>())};
  } catch (...) {
    return libchiaking::unexpected(CryptoError::backend_failure);
  }
}

}  // namespace libchiaking::platform
