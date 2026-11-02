#include "libchiaking/detail/consoles/pairing.hpp"

#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::consoles {

auto PairingPin::parse(std::string_view digits) noexcept
    -> std::optional<PairingPin> {
  const auto value = text::parse_integer<std::uint32_t>(digits);
  if (digits.size() != digit_count || !value) return std::nullopt;
  return PairingPin{*value};
}

auto PairingPin::create(std::uint32_t digits) noexcept
    -> std::optional<PairingPin> {
  if (digits > maximum) return std::nullopt;
  return PairingPin{digits};
}

PairingPin::PairingPin(std::uint32_t digits) noexcept {
  bytes::store_be<std::uint32_t>({digits_.data(), digits_.size()}, digits);
}

auto PairingPin::value() const noexcept -> std::uint32_t {
  return bytes::load_be<std::uint32_t>({digits_.data(), digits_.size()});
}

auto PairingKeys::create(
    std::span<const std::byte, registration_key_size> registration_key,
    std::span<const std::byte, remote_play_key_size> remote_play_key,
    std::uint32_t key_type) noexcept -> std::optional<PairingKeys> {
  const auto blank = [](std::byte value) { return value == std::byte{0}; };
  if (std::ranges::all_of(registration_key, blank) ||
      std::ranges::all_of(remote_play_key, blank))
    return std::nullopt;
  PairingKeys keys;
  std::ranges::copy(registration_key, keys.registration_key_.begin());
  std::ranges::copy(remote_play_key, keys.remote_play_key_.begin());
  keys.key_type_ = key_type;
  return keys;
}

auto PairingKeys::clone() const noexcept -> PairingKeys {
  PairingKeys copy;
  std::ranges::copy(registration_key_, copy.registration_key_.begin());
  std::ranges::copy(remote_play_key_, copy.remote_play_key_.begin());
  copy.key_type_ = key_type_;
  return copy;
}

auto PairingKeys::registration_key() const noexcept
    -> std::span<const std::byte, registration_key_size> {
  return std::span<const std::byte, registration_key_size>{
      registration_key_.data(), registration_key_size};
}

auto PairingKeys::remote_play_key() const noexcept
    -> std::span<const std::byte, remote_play_key_size> {
  return std::span<const std::byte, remote_play_key_size>{
      remote_play_key_.data(), remote_play_key_size};
}

}  // namespace libchiaking::consoles
