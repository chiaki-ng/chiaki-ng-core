#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"

#include "src/core/utils/bytes/endian.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {
Expected<RegistrationMaterial, MaterialError>
RegistrationMaterial::derive_local(RegistrationTarget target,
                                   std::span<const std::byte> ambassador,
                                   std::uint32_t pin) {
  if (target != RegistrationTarget::ps4_unknown &&
      target != RegistrationTarget::ps4_8 &&
      target != RegistrationTarget::ps4_9 &&
      target != RegistrationTarget::ps4_10 &&
      target != RegistrationTarget::ps5_1)
    return unexpected(MaterialError::invalid_target);
  if (ambassador.size() != rp_key_size)
    return unexpected(MaterialError::invalid_ambassador);

  RegistrationMaterial m;
  std::array<std::byte, sizeof(pin)> pin_bytes{};
  bytes::store_be<std::uint32_t>(pin_bytes, pin);
  std::ranges::copy(ambassador, m.ambassador.begin());

  if (target == RegistrationTarget::ps4_unknown ||
      target == RegistrationTarget::ps4_8 ||
      target == RegistrationTarget::ps4_9) {
    std::ranges::transform(pre10_key, m.bright.begin(), to_byte);
    std::ranges::transform(m.bright.span().first(pin_bytes.size()), pin_bytes,
                           m.bright.begin(), std::bit_xor<>{});
    for (std::size_t i = 0; i < rp_key_size; ++i) {
      const std::uint8_t x = static_cast<std::uint8_t>(
          static_cast<std::uint8_t>(
              std::to_integer<std::uint8_t>(ambassador[i])) -
          static_cast<std::uint8_t>(i) - ps4_aeropause_add);
      m.aeropause[i] = std::byte{static_cast<std::uint8_t>(x ^ echo_b[i])};
    }
    return m;
  }

  const bool ps5 = target == RegistrationTarget::ps5_1;
  const auto& key0 = ps5 ? ps5_key_0 : ps4_key_0;
  const auto& key1 = ps5 ? ps5_key_1 : ps4_key_1;
  const std::uint8_t add = ps5 ? ps5_aeropause_add : ps4_aeropause_add;

  std::ranges::transform(key0, m.bright.begin(), to_byte);
  for (std::size_t i = 0; i < rp_key_size; ++i) {
    const std::uint8_t amb = std::to_integer<std::uint8_t>(ambassador[i]);
    const std::uint8_t t = static_cast<std::uint8_t>(amb ^ key1[i]);
    m.aeropause[i] = std::byte{
        static_cast<std::uint8_t>(t + add + static_cast<std::uint8_t>(i))};
  }
  const auto pin_tail = m.bright.span().last(pin_bytes.size());
  std::ranges::transform(pin_tail, pin_bytes, pin_tail.begin(),
                         std::bit_xor<>{});
  return m;
}

Expected<RegistrationMaterial, MaterialError> RegistrationMaterial::derive_psn(
    platform::CryptoProvider& provider, RegistrationTarget target,
    std::span<const std::byte> ambassador,
    std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
    std::span<const std::byte> data2) {
  if (target != RegistrationTarget::ps4_10 &&
      target != RegistrationTarget::ps5_1)
    return unexpected(MaterialError::invalid_target);
  if (ambassador.size() != rp_key_size)
    return unexpected(MaterialError::invalid_ambassador);
  if (custom_data1.size() != rp_key_size ||
      data1.size() != platform::aes128_key_size ||
      data2.size() != rp_nonce_size)
    return unexpected(MaterialError::invalid_psn_data);
  auto base = derive_local(target, ambassador, 0);
  if (!base) return unexpected(base.error());
  SecretArray<rp_key_size> encrypted;
  auto transformed = RegistrationCipher::transform(
      provider, target, data1, data2, 0, CipherDirection::encrypt, custom_data1,
      {encrypted.data(), encrypted.size()});
  if (!transformed) return unexpected(MaterialError::provider_failure);
  const auto& key1 =
      target == RegistrationTarget::ps5_1 ? ps5_key_1 : ps4_key_1;
  const std::uint8_t add = target == RegistrationTarget::ps5_1
                               ? psn_ps5_aeropause_add
                               : psn_ps4_aeropause_add;
  for (std::size_t i = 0; i < encrypted.size(); ++i) {
    base.value().bright[i] ^= encrypted[i];
    const auto adjusted = static_cast<std::uint8_t>(
        std::to_integer<std::uint8_t>(ambassador[i]) - i + add);
    base.value().aeropause[i] =
        std::byte{static_cast<std::uint8_t>(adjusted ^ key1[i])};
  }
  return std::move(base.value());
}

}  // namespace libchiaking::net::protocol::remote_play
