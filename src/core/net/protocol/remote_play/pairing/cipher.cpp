#include "cipher.hpp"
#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/detail/wiping_allocator.hpp>
#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace libchiaking::net::protocol::remote_play {
using platform::aes128_block_size;
using platform::aes128_key_size;

auto RegistrationCipher::target_key(RegistrationTarget target) noexcept
    -> std::optional<std::span<const std::byte, aes128_key_size>> {
  static constexpr std::array<std::array<std::uint8_t, aes128_key_size>, 3>
      keys{{
          {0xac, 0x07, 0x88, 0x83, 0xc8, 0x3a, 0x1f, 0xe8, 0x11, 0x46, 0x3a,
           0xf3, 0x9e, 0xe3, 0xe3, 0x77},
          {0x20, 0xd6, 0x6f, 0x59, 0x04, 0xea, 0x7c, 0x14, 0xe5, 0x57, 0xff,
           0xc5, 0x2e, 0x48, 0x8a, 0xc8},
          {0x46, 0x46, 0x87, 0xb3, 0x49, 0xca, 0x8c, 0xe8, 0x59, 0xc5, 0x27,
           0x0f, 0x5d, 0x7a, 0x69, 0xd6},
      }};
  const auto key = [](const std::array<std::uint8_t, aes128_key_size>& row) {
    return std::as_bytes(std::span{row});
  };
  switch (target) {
    case RegistrationTarget::ps4_unknown:
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
      return key(keys[0]);
    case RegistrationTarget::ps4_10:
      return key(keys[1]);
    case RegistrationTarget::ps5_1:
      return key(keys[2]);
  }
  return std::nullopt;
}

Expected<void, CipherError> RegistrationCipher::transform(
    platform::CryptoProvider& provider, RegistrationTarget target,
    std::span<const std::byte> key, std::span<const std::byte> ambassador,
    std::uint64_t counter, CipherDirection direction,
    std::span<const std::byte> input, std::span<std::byte> output) {
  const auto hmac_key = target_key(target);
  if ((direction != CipherDirection::encrypt &&
       direction != CipherDirection::decrypt) ||
      !hmac_key || key.size() != aes128_key_size ||
      ambassador.size() != rp_nonce_size || output.size() != input.size())
    return unexpected(CipherError::invalid_argument);

  SecretArray<rp_nonce_size + sizeof(counter)> hmac_input;
  std::ranges::copy(ambassador, hmac_input.begin());
  bytes::store_be<std::uint64_t>(hmac_input.span().subspan(rp_nonce_size),
                                 counter);

  SecretArray<platform::hmac_sha256_digest_size> digest;
  std::vector<std::byte, memory::WipingAllocator<std::byte>> staging(
      input.size());

  if (!provider.hmac_sha256(*hmac_key, hmac_input.span(), digest.span()))
    return unexpected(CipherError::provider_failure);

  SecretArray<aes128_block_size> feedback;
  SecretArray<aes128_block_size> keystream;
  std::ranges::copy(digest.span().first(feedback.size()), feedback.begin());

  for (std::size_t offset = 0; offset < input.size();
       offset += aes128_block_size) {
    if (!provider.aes128_encrypt_block(key, feedback.span(), keystream.span()))
      return unexpected(CipherError::provider_failure);
    const std::size_t n = std::min(aes128_block_size, input.size() - offset);
    const auto block = input.subspan(offset, n);
    std::ranges::transform(
        block, keystream.span().first(n),
        staging.begin() + static_cast<std::ptrdiff_t>(offset),
        std::bit_xor<>{});
    if (offset + n < input.size()) {
      const auto segment =
          direction == CipherDirection::encrypt
              ? std::span<const std::byte>{staging}.subspan(offset, n)
              : block;
      std::ranges::copy(segment, feedback.begin());
    }
  }

  std::ranges::copy(staging, output.begin());
  return {};
}

}  // namespace libchiaking::net::protocol::remote_play
