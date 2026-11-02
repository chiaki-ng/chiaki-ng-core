#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <libchiaking/detail/secret_array.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::takion {
[[nodiscard]] auto PlatformTakionCryptoProvider::provider_failure() noexcept
    -> libchiaking::Unexpected<CryptoError> {
  return libchiaking::unexpected(CryptoError::provider_failure);
}

void PlatformTakionCryptoProvider::add_le128(
    std::span<std::byte, takion_crypto_block_size> output,
    std::span<const std::byte, takion_crypto_block_size> base,
    std::uint64_t value) noexcept {
  const auto low =
      bytes::load_le<std::uint64_t>(base.first<sizeof(std::uint64_t)>());
  const auto high =
      bytes::load_le<std::uint64_t>(base.last<sizeof(std::uint64_t)>());
  const auto sum = low + value;
  bytes::store_le<std::uint64_t>(output.first<sizeof(std::uint64_t)>(), sum);
  bytes::store_le<std::uint64_t>(output.last<sizeof(std::uint64_t)>(),
                                 high + (sum < low ? 1U : 0U));
}

auto PlatformTakionCryptoProvider::hashed_key(
    platform::CryptoProvider& provider,
    std::span<const std::byte, key_base_size> first,
    std::span<const std::byte, key_base_size> second)
    -> Expected<SecretArray<key_base_size>, CryptoError> {
  SecretArray<derived_digest_size> input;
  std::ranges::copy(first, input.begin());
  std::ranges::copy(second, input.begin() + key_base_size);
  SecretArray<derived_digest_size> digest;
  if (!provider.sha256(input.span(), digest.span())) return provider_failure();
  SecretArray<key_base_size> key;
  std::ranges::transform(digest.span().first<key_base_size>(),
                         digest.span().last<key_base_size>(), key.begin(),
                         std::bit_xor<>());
  return key;
}

class PlatformTakionCryptoProvider::GKCryptDirection final
    : public CryptoDirection {
 public:
  GKCryptDirection(platform::CryptoProvider& provider,
                   platform::ThreadingProvider& threading,
                   SourceReceiveKeys keys,
                   SecretArray<key_base_size> key_gmac_base,
                   std::unique_ptr<platform::Aes128Cipher> stream,
                   std::unique_ptr<platform::Aes128Cipher> gmac_base)
      : provider_(provider),
        keys_(std::move(keys)),
        key_gmac_base_(std::move(key_gmac_base)),
        stream_(std::move(stream)),
        gmac_base_(std::move(gmac_base)),
        gmac_cache_mutex_(threading) {}

  [[nodiscard]] auto apply_keystream(std::uint64_t key_position,
                                     std::span<const std::byte> input,
                                     std::span<std::byte> output)
      -> Expected<void, CryptoError> override {
    if (output.size() != input.size())
      return unexpected(CryptoError::invalid_coverage);
    if (input.empty()) return {};
    const auto block_offset =
        static_cast<std::size_t>(key_position % takion_crypto_block_size);
    if (input.size() > std::numeric_limits<std::size_t>::max() - block_offset)
      return unexpected(CryptoError::counter_overflow);
    if (block_offset + input.size() <=
        short_chunk_blocks * takion_crypto_block_size)
      return apply_chunked<short_chunk_blocks>(key_position, input, output);
    return apply_chunked<keystream_chunk_blocks>(key_position, input, output);
  }

  [[nodiscard]] auto authenticate(
      std::uint64_t key_position,
      std::span<const std::byte> authenticated_bytes)
      -> Expected<Mac, CryptoError> override {
    SecretArray<takion_crypto_block_size> gmac_iv;
    add_le128(gmac_iv.span(), keys_.iv(),
              key_position / takion_crypto_block_size);
    Mac tag{};
    const auto rotation = key_position == 0
                              ? std::uint64_t{0}
                              : (key_position - 1) / gmac_key_refresh_position;
    if (rotation == 0) {
      if (!gmac_base_->gmac(gmac_iv.span(), authenticated_bytes,
                            std::span<std::byte>(tag)))
        return provider_failure();
      return tag;
    }
    const platform::Lock cache_lock{gmac_cache_mutex_};
    const auto cipher = rotated_gmac(rotation);
    if (!cipher) return unexpected(cipher.error());
    if (!cipher.value()->gmac(gmac_iv.span(), authenticated_bytes,
                              std::span<std::byte>(tag)))
      return provider_failure();
    return tag;
  }

 private:
  static constexpr std::size_t keystream_chunk_blocks = 64;
  static constexpr std::size_t short_chunk_blocks = 16;

  template <std::size_t Blocks>
  [[nodiscard]] auto apply_chunked(std::uint64_t key_position,
                                   std::span<const std::byte> input,
                                   std::span<std::byte> output)
      -> Expected<void, CryptoError> {
    constexpr auto chunk_size = Blocks * takion_crypto_block_size;
    auto block_offset =
        static_cast<std::size_t>(key_position % takion_crypto_block_size);
    SecretArray<chunk_size> counters;
    SecretArray<chunk_size> keystream;
    auto counter_value = key_position / takion_crypto_block_size;
    while (!input.empty()) {
      const auto spanned = block_offset + input.size();
      const auto blocks = std::min(
          Blocks, spanned / takion_crypto_block_size +
                      (spanned % takion_crypto_block_size != 0 ? 1 : 0));
      const auto size = blocks * takion_crypto_block_size;
      for (std::size_t block = 0; block < blocks; ++block)
        add_le128(counters.span()
                      .subspan(block * takion_crypto_block_size)
                      .template first<takion_crypto_block_size>(),
                  keys_.iv(), counter_value++);
      if (!stream_->encrypt_blocks(counters.span().first(size),
                                   keystream.span().first(size)))
        return provider_failure();
      const auto taken = std::min(size - block_offset, input.size());
      std::ranges::transform(input.first(taken),
                             keystream.span().subspan(block_offset, taken),
                             output.begin(), std::bit_xor<>());
      input = input.subspan(taken);
      output = output.subspan(taken);
      block_offset = 0;
    }
    return {};
  }
  static constexpr std::size_t gmac_cache_slots = 4;

  struct RotatedGmac {
    std::uint64_t rotation{};
    std::unique_ptr<platform::Aes128Cipher> cipher;
  };

  [[nodiscard]] auto rotated_gmac(std::uint64_t rotation)
      -> Expected<const platform::Aes128Cipher*, CryptoError> {
    for (const auto& slot : gmac_cache_)
      if (slot.cipher && slot.rotation == rotation) return slot.cipher.get();
    SecretArray<key_base_size> rotated_iv;
    add_le128(rotated_iv.span(), keys_.iv(),
              rotation * gmac_key_refresh_iv_offset);
    auto key = hashed_key(provider_, key_gmac_base_.span(), rotated_iv.span());
    if (!key) return unexpected(key.error());
    auto cipher = provider_.aes128_cipher(key.value().span());
    if (!cipher) return provider_failure();
    auto& slot = gmac_cache_[gmac_cache_next_];
    gmac_cache_next_ = (gmac_cache_next_ + 1) % gmac_cache_.size();
    slot = RotatedGmac{rotation, std::move(cipher).value()};
    return slot.cipher.get();
  }

  platform::CryptoProvider& provider_;
  SourceReceiveKeys keys_;
  SecretArray<key_base_size> key_gmac_base_;
  std::unique_ptr<platform::Aes128Cipher> stream_;
  std::unique_ptr<platform::Aes128Cipher> gmac_base_;
  platform::Mutex gmac_cache_mutex_;
  std::array<RotatedGmac, gmac_cache_slots> gmac_cache_{};
  std::size_t gmac_cache_next_{};
};

auto PlatformTakionCryptoProvider::derive_keys(
    platform::CryptoProvider& provider, const CryptoMaterial& material)
    -> Expected<SourceReceiveKeys, CryptoError> {
  const auto gkcrypt_index = material.gkcrypt_index();
  if (!gkcrypt_index) return unexpected(CryptoError::invalid_key_material);

  const auto handshake_key = material.handshake_key();
  const auto ecdh_secret = material.ecdh_secret();
  if (handshake_key.size() != takion_crypto_block_size ||
      (ecdh_secret.size() !=
           platform::ecdh_shared_secret_size(platform::EcdhCurve::secp256k1) &&
       ecdh_secret.size() !=
           platform::ecdh_shared_secret_size(platform::EcdhCurve::secp521r1)))
    return unexpected(CryptoError::invalid_key_material);

  SecretArray<3 + takion_crypto_block_size + 2> message;
  message[0] = std::byte{1};
  message[1] = static_cast<std::byte>(*gkcrypt_index);
  std::ranges::copy(handshake_key, message.begin() + 3);
  message[3 + takion_crypto_block_size] = std::byte{1};

  SecretArray<derived_digest_size> digest;
  if (!provider.hmac_sha256(ecdh_secret, message.span(), digest.span()))
    return provider_failure();
  SecretArray<source_key_size> key;
  SecretArray<source_key_size> iv;
  std::ranges::copy(digest.span().first<key_base_size>(), key.begin());
  std::ranges::copy(digest.span().last<key_base_size>(), iv.begin());
  return SourceReceiveKeys{std::move(key), std::move(iv)};
}

auto PlatformTakionCryptoProvider::create_direction(
    platform::CryptoProvider& provider, platform::ThreadingProvider& threading,
    SourceReceiveKeys keys)
    -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> {
  auto key_gmac_base = hashed_key(provider, keys.key(), keys.iv());
  if (!key_gmac_base) return unexpected(key_gmac_base.error());
  auto stream = provider.aes128_cipher(keys.key());
  if (!stream) return provider_failure();
  auto gmac_base = provider.aes128_cipher(key_gmac_base.value().span());
  if (!gmac_base) return provider_failure();
  try {
    std::unique_ptr<CryptoDirection> direction =
        std::make_unique<GKCryptDirection>(provider, threading, std::move(keys),
                                           std::move(key_gmac_base).value(),
                                           std::move(stream).value(),
                                           std::move(gmac_base).value());
    return direction;
  } catch (...) {
    return provider_failure();
  }
}

auto PlatformTakionCryptoProvider::create_local(CryptoMaterial material)
    -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> {
  return derive_keys(provider_, material)
      .and_then([this](SourceReceiveKeys keys) {
        return create_direction(provider_, threading_, std::move(keys));
      });
}

auto PlatformTakionCryptoProvider::create_remote(CryptoMaterial material)
    -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> {
  return create_local(std::move(material));
}

auto PlatformTakionCryptoProvider::export_receive_keys(
    const CryptoMaterial& material)
    -> Expected<SourceReceiveKeys, CryptoError> {
  return derive_keys(provider_, material);
}

auto PlatformTakionCryptoProvider::create_receive_only(
    const SourceReceiveKeys& keys)
    -> Expected<std::unique_ptr<CryptoDirection>, CryptoError> {
  return create_direction(provider_, threading_, keys.clone());
}

}  // namespace libchiaking::net::protocol::takion
