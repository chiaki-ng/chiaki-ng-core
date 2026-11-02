// The PMULL multiply and reduction follow Mbed TLS library/aesce.c.
// Copyright The Mbed TLS Contributors
// SPDX-License-Identifier: Apache-2.0

#include "src/platform/crypto/armv8/gmac.hpp"

#include "src/core/utils/bytes/endian.hpp"

#include <arm_neon.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::platform::armv8 {

PmullGmac::PmullGmac(
    std::span<const std::byte, aes128_block_size> hash_key) noexcept {
  std::ranges::copy(hash_key, hash_key_.begin());
}

auto PmullGmac::counter_block(std::span<const std::byte> iv) const noexcept
    -> Block {
  constexpr std::size_t short_nonce_size = 12;
  Block counter{};
  if (iv.size() == short_nonce_size) {
    std::ranges::copy(iv, counter.begin());
    counter.back() = std::byte{1};
    return counter;
  }
  absorb(counter, iv);
  fold(counter, lengths_block(0, std::uint64_t{iv.size()} * 8U));
  return multiply(counter, hash_key_.span());
}

void PmullGmac::authenticate(
    std::span<const std::byte> aad,
    std::span<const std::byte, aes128_block_size> encrypted_counter,
    std::span<std::byte> tag) const noexcept {
  Block state{};
  absorb(state, aad);
  fold(state, lengths_block(std::uint64_t{aad.size()} * 8U, 0));
  state = multiply(state, hash_key_.span());
  fold(state, encrypted_counter);
  const auto written = std::min(tag.size(), state.size());
  std::ranges::copy(std::span<const std::byte>(state).first(written),
                    tag.begin());
}

auto PmullGmac::multiply(
    const Block& left,
    std::span<const std::byte, aes128_block_size> right) noexcept -> Block {
  const uint8x16_t a =
      vrbitq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(left.data())));
  const uint8x16_t b =
      vrbitq_u8(vld1q_u8(reinterpret_cast<const std::uint8_t*>(right.data())));

  const uint8x16_t low = vreinterpretq_u8_p128(
      vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u8(a), 0),
                vgetq_lane_p64(vreinterpretq_p64_u8(b), 0)));
  const uint8x16_t high = vreinterpretq_u8_p128(
      vmull_high_p64(vreinterpretq_p64_u8(a), vreinterpretq_p64_u8(b)));
  const uint8x16_t a_halves = veorq_u8(a, vextq_u8(a, a, 8));
  const uint8x16_t b_halves = veorq_u8(b, vextq_u8(b, b, 8));
  const uint8x16_t cross = vreinterpretq_u8_p128(
      vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u8(a_halves), 0),
                vgetq_lane_p64(vreinterpretq_p64_u8(b_halves), 0)));
  const uint8x16_t middle = veorq_u8(veorq_u8(cross, low), high);

  const uint8x16_t modulus = vreinterpretq_u8_u64(vdupq_n_u64(0x87));
  const uint8x16_t high_folded_high = vreinterpretq_u8_p128(vmull_high_p64(
      vreinterpretq_p64_u8(high), vreinterpretq_p64_u8(modulus)));
  const uint8x16_t high_folded_low = vreinterpretq_u8_p128(
      vmull_p64(vgetq_lane_p64(vreinterpretq_p64_u8(high), 0),
                vgetq_lane_p64(vreinterpretq_p64_u8(modulus), 0)));
  const uint8x16_t carried = veorq_u8(high_folded_high, middle);
  const uint8x16_t carried_folded = vreinterpretq_u8_p128(vmull_high_p64(
      vreinterpretq_p64_u8(carried), vreinterpretq_p64_u8(modulus)));
  const uint8x16_t carried_low = vextq_u8(vdupq_n_u8(0), carried, 8);
  const uint8x16_t reduced = veorq_u8(
      veorq_u8(veorq_u8(high_folded_low, low), carried_folded), carried_low);

  Block product{};
  vst1q_u8(reinterpret_cast<std::uint8_t*>(product.data()), vrbitq_u8(reduced));
  return product;
}

auto PmullGmac::lengths_block(std::uint64_t first_bits,
                              std::uint64_t second_bits) noexcept -> Block {
  Block lengths{};
  bytes::store_be(std::span<std::byte>(lengths).first(sizeof(first_bits)),
                  first_bits);
  bytes::store_be(std::span<std::byte>(lengths).last(sizeof(second_bits)),
                  second_bits);
  return lengths;
}

void PmullGmac::fold(Block& state, std::span<const std::byte> block) noexcept {
  std::ranges::transform(
      block, state, state.begin(),
      [](std::byte data, std::byte current) { return current ^ data; });
}

void PmullGmac::absorb(Block& state,
                       std::span<const std::byte> data) const noexcept {
  while (!data.empty()) {
    const auto chunk = std::min(data.size(), state.size());
    fold(state, data.first(chunk));
    state = multiply(state, hash_key_.span());
    data = data.subspan(chunk);
  }
}

}  // namespace libchiaking::platform::armv8
