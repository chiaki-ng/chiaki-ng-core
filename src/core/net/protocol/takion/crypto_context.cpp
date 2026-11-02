#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/numeric/checked.hpp"

#include "src/core/net/protocol/takion/limits.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace libchiaking::net::protocol::takion {
bool CryptoContext::range_fits(std::size_t offset, std::size_t size,
                               std::size_t total) noexcept {
  return offset <= total && size <= total - offset;
}

auto CryptoContext::checked_advance(std::uint64_t position,
                                    std::uint64_t input_size)
    -> Expected<std::uint64_t, CryptoError> {
  return checked_addition(input_size, input_size % takion_crypto_block_size)
      .and_then([position](std::uint64_t advance) {
        return checked_addition(position, advance);
      });
}

auto CryptoContext::checked_addition(std::uint64_t position,
                                     std::uint64_t addition)
    -> Expected<std::uint64_t, CryptoError> {
  if (const auto sum = numeric::checked_add(position, addition)) return *sum;
  return unexpected(CryptoError::counter_overflow);
}

auto CryptoContext::validate_coverage(std::span<const std::byte> wire,
                                      PacketCryptoCoverage coverage,
                                      PacketCryptoOffsets offsets)
    -> Expected<void, CryptoError> {
  if (!range_fits(coverage.authenticated_offset, coverage.authenticated_size,
                  wire.size()) ||
      !range_fits(coverage.payload_offset, coverage.payload_size, wire.size()))
    return unexpected(CryptoError::invalid_coverage);
  const auto authenticated_end =
      coverage.authenticated_offset + coverage.authenticated_size;
  if (coverage.payload_offset < coverage.authenticated_offset ||
      coverage.payload_offset > authenticated_end ||
      coverage.payload_size > authenticated_end - coverage.payload_offset)
    return unexpected(CryptoError::invalid_coverage);
  if (!range_fits(offsets.mac, takion_mac_size, coverage.authenticated_size) ||
      !range_fits(offsets.key_position_low, sizeof(std::uint32_t),
                  coverage.authenticated_size))
    return unexpected(CryptoError::packet_too_short);
  return {};
}

auto CryptoContext::reconstruct_position(std::uint64_t previous,
                                         std::uint32_t low)
    -> Expected<std::uint64_t, CryptoError> {
  const auto previous_low = static_cast<std::uint32_t>(previous);
  auto high = static_cast<std::uint32_t>(previous >> 32);
  const auto forward_distance = static_cast<std::uint32_t>(low - previous_low);
  const bool ahead =
      forward_distance != 0 && forward_distance < serial_half_range;
  const auto backward_distance = static_cast<std::uint32_t>(previous_low - low);
  const bool behind =
      backward_distance != 0 && backward_distance < serial_half_range;

  if (ahead && low < previous_low) {
    if (high == std::numeric_limits<std::uint32_t>::max())
      return unexpected(CryptoError::counter_overflow);
    ++high;
  } else if (behind && low > previous_low && high != 0) {
    --high;
  }
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

auto CryptoContext::authenticated_copy(std::span<const std::byte> wire,
                                       PacketCryptoCoverage coverage,
                                       PacketCryptoOffsets offsets)
    -> std::vector<std::byte> {
  const auto packet =
      wire.subspan(coverage.authenticated_offset, coverage.authenticated_size);
  std::vector<std::byte> authenticated(packet.begin(), packet.end());
  const auto view = std::span<std::byte>{authenticated};
  std::ranges::fill(view.subspan(offsets.mac, takion_mac_size), std::byte{});
  if (offsets.zero_key_position_for_authentication)
    std::ranges::fill(
        view.subspan(offsets.key_position_low, sizeof(std::uint32_t)),
        std::byte{});
  return authenticated;
}

auto CryptoContext::packet_offsets(PacketFamily family,
                                   const WireProfile& profile)
    -> Expected<PacketCryptoOffsets, CryptoError> {
  std::size_t mac{};
  std::size_t key_position{};
  bool zero_key_position{};
  std::uint64_t cipher_key_position_addition{};
  switch (family) {
    case PacketFamily::control:
      mac = 5;
      key_position = 9;
      zero_key_position = true;
      break;
    case PacketFamily::video:
    case PacketFamily::audio_and_haptics:
      mac = 0x0a;
      key_position = 0x0e;
      cipher_key_position_addition = takion_crypto_block_size;
      break;
    case PacketFamily::congestion:
      mac = 7;
      key_position = 0x0b;
      zero_key_position = true;
      break;
    case PacketFamily::extended_message:
      mac = 1;
      key_position = 5;
      cipher_key_position_addition = takion_crypto_block_size;
      break;
    case PacketFamily::generic_data:
      if (profile.version() < generic_data_minimum_version)
        return unexpected(CryptoError::unsupported_packet_family);
      mac = 1;
      key_position = 5;
      cipher_key_position_addition = takion_crypto_block_size;
      break;
    case PacketFamily::feedback_history:
    case PacketFamily::feedback_state:
      mac = 8;
      key_position = 4;
      cipher_key_position_addition = takion_crypto_block_size;
      break;
    case PacketFamily::length_prefixed:
    case PacketFamily::client_information:
    case PacketFamily::trace:
      return unexpected(CryptoError::unsupported_packet_family);
  }
  const PacketType type{static_cast<std::uint8_t>(family)};
  const auto shift = profile.packet_header_size(type) - 1;
  return PacketCryptoOffsets{mac + shift, key_position + shift,
                             zero_key_position, cipher_key_position_addition};
}

CryptoContext::LocalReservation::LocalReservation(
    CryptoContext* owner, std::uint64_t key_position,
    std::uint64_t next_position, std::uint64_t generation) noexcept
    : owner_(owner),
      key_position_(key_position),
      next_position_(next_position),
      generation_(generation),
      no_op_(false) {}

CryptoContext::LocalReservation::~LocalReservation() { rollback(); }

CryptoContext::LocalReservation::LocalReservation(
    LocalReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      key_position_(other.key_position_),
      next_position_(other.next_position_),
      generation_(other.generation_),
      no_op_(std::exchange(other.no_op_, false)) {}

auto CryptoContext::LocalReservation::operator=(
    LocalReservation&& other) noexcept -> LocalReservation& {
  if (this != &other) {
    rollback();
    owner_ = std::exchange(other.owner_, nullptr);
    key_position_ = other.key_position_;
    next_position_ = other.next_position_;
    generation_ = other.generation_;
    no_op_ = std::exchange(other.no_op_, false);
  }
  return *this;
}

auto CryptoContext::LocalReservation::commit() noexcept
    -> Expected<void, CryptoError> {
  if (no_op_) {
    no_op_ = false;
    return {};
  }
  if (!owner_) return unexpected(CryptoError::stale_reservation);
  auto* owner = std::exchange(owner_, nullptr);
  return owner->commit(generation_, next_position_);
}

void CryptoContext::LocalReservation::rollback() noexcept {
  if (owner_) {
    owner_->rollback(generation_);
    owner_ = nullptr;
  }
}

CryptoContext::CryptoContext(CryptoPolicy policy) noexcept : policy_(policy) {}

auto CryptoContext::install_local(CryptoProvider& provider,
                                  CryptoMaterial material)
    -> Expected<void, CryptoError> {
  if (local_reservation_active_)
    return unexpected(CryptoError::reservation_in_progress);
  auto direction = provider.create_local(std::move(material));
  if (!direction) return unexpected(direction.error());
  if (!direction.value()) return unexpected(CryptoError::provider_failure);
  local_ = std::move(direction).value();
  return {};
}

auto CryptoContext::install_remote(CryptoProvider& provider,
                                   CryptoMaterial material)
    -> Expected<void, CryptoError> {
  auto direction = provider.create_remote(std::move(material));
  if (!direction) return unexpected(direction.error());
  if (!direction.value()) return unexpected(CryptoError::provider_failure);
  remote_ = std::move(direction).value();
  return {};
}

auto CryptoContext::protect_local(std::span<std::byte> wire,
                                  PacketFamily family,
                                  const WireProfile& profile,
                                  PacketCryptoCoverage coverage,
                                  std::uint64_t key_advance_input_size)
    -> Expected<LocalReservation, CryptoError> {
  if (local_reservation_active_)
    return unexpected(CryptoError::reservation_in_progress);
  if (policy_ == CryptoPolicy::disabled) return LocalReservation{};
  if (!local_) return unexpected(CryptoError::key_material_missing);
  const auto offsets_result = packet_offsets(family, profile);
  if (!offsets_result) return unexpected(offsets_result.error());
  const auto offsets = offsets_result.value();
  const auto coverage_result = validate_coverage(wire, coverage, offsets);
  if (!coverage_result) return unexpected(coverage_result.error());
  const auto next_result =
      checked_advance(local_position_, key_advance_input_size);
  if (!next_result) return unexpected(next_result.error());
  const auto cipher_position_result = checked_addition(
      local_position_, coverage.cipher_key_position_addition.value_or(
                           offsets.cipher_key_position_addition));
  if (!cipher_position_result)
    return unexpected(cipher_position_result.error());

  SecretBytes protected_wire{wire};
  const auto protected_view = protected_wire.span();
  const auto packet = protected_view.subspan(coverage.authenticated_offset);
  bytes::store_be<std::uint32_t>(
      packet.subspan(offsets.key_position_low, sizeof(std::uint32_t)),
      static_cast<std::uint32_t>(local_position_));
  const auto payload =
      protected_view.subspan(coverage.payload_offset, coverage.payload_size);
  const auto encrypted =
      local_->apply_keystream(cipher_position_result.value(), payload, payload);
  if (!encrypted) return unexpected(encrypted.error());

  const auto mac = local_->authenticate(
      local_position_, authenticated_copy(protected_view, coverage, offsets));
  if (!mac) return unexpected(mac.error());
  std::ranges::copy(mac.value(), packet.subspan(offsets.mac).begin());
  std::ranges::copy(protected_view, wire.begin());

  local_reservation_active_ = true;
  ++reservation_generation_;
  return LocalReservation{this, local_position_, next_result.value(),
                          reservation_generation_};
}

auto CryptoContext::authenticate_and_decrypt_remote(
    std::span<std::byte> wire, PacketFamily family, const WireProfile& profile,
    PacketCryptoCoverage coverage) -> Expected<std::uint64_t, CryptoError> {
  if (policy_ == CryptoPolicy::disabled) return std::uint64_t{};
  if (!remote_) return unexpected(CryptoError::key_material_missing);
  const auto offsets_result = packet_offsets(family, profile);
  if (!offsets_result) return unexpected(offsets_result.error());
  const auto offsets = offsets_result.value();
  const auto coverage_result = validate_coverage(wire, coverage, offsets);
  if (!coverage_result) return unexpected(coverage_result.error());

  const auto packet_start = coverage.authenticated_offset;
  const auto low =
      bytes::load_be<std::uint32_t>(std::span<const std::byte>{wire}.subspan(
          packet_start + offsets.key_position_low, sizeof(std::uint32_t)));
  const auto position_result = reconstruct_position(remote_position_, low);
  if (!position_result) return unexpected(position_result.error());
  const auto position = position_result.value();
  const auto cipher_position_result =
      checked_addition(position, offsets.cipher_key_position_addition);
  if (!cipher_position_result)
    return unexpected(cipher_position_result.error());

  const auto packet = wire.subspan(packet_start, coverage.authenticated_size);
  const auto mac_field = packet.subspan(offsets.mac, takion_mac_size);
  const auto position_field =
      packet.subspan(offsets.key_position_low, sizeof(std::uint32_t));
  Mac received_mac{};
  std::ranges::copy(mac_field, received_mac.begin());
  std::array<std::byte, sizeof(std::uint32_t)> received_position{};
  std::ranges::copy(position_field, received_position.begin());
  std::ranges::fill(mac_field, std::byte{});
  if (offsets.zero_key_position_for_authentication)
    std::ranges::fill(position_field, std::byte{});
  const auto expected_mac = remote_->authenticate(position, packet);
  std::ranges::copy(received_mac, mac_field.begin());
  std::ranges::copy(received_position, position_field.begin());
  if (!expected_mac) return unexpected(expected_mac.error());
  if (!memory::constant_time_equal(received_mac, expected_mac.value()))
    return unexpected(CryptoError::authentication_failed);

  const auto payload =
      wire.subspan(coverage.payload_offset, coverage.payload_size);
  const auto decrypted = remote_->apply_keystream(
      cipher_position_result.value(), payload, payload);
  if (!decrypted) return unexpected(decrypted.error());
  remote_position_ = position;
  return position;
}

auto CryptoContext::authenticate_local_detached(
    std::span<const std::byte> authenticated,
    std::uint64_t key_advance_input_size)
    -> Expected<DetachedLocalAuthentication, CryptoError> {
  if (local_reservation_active_)
    return unexpected(CryptoError::reservation_in_progress);
  if (policy_ == CryptoPolicy::disabled) return DetachedLocalAuthentication{};
  if (!local_) return unexpected(CryptoError::key_material_missing);
  const auto next_result =
      checked_advance(local_position_, key_advance_input_size);
  if (!next_result) return unexpected(next_result.error());
  const auto mac = local_->authenticate(local_position_, authenticated);
  if (!mac) return unexpected(mac.error());
  local_reservation_active_ = true;
  ++reservation_generation_;
  return DetachedLocalAuthentication{
      mac.value(), LocalReservation{this, local_position_, next_result.value(),
                                    reservation_generation_}};
}

auto CryptoContext::verify_remote_detached(
    std::span<const std::byte> authenticated, std::uint32_t key_position_low,
    const Mac& received_mac) -> Expected<std::uint64_t, CryptoError> {
  if (policy_ == CryptoPolicy::disabled) return std::uint64_t{};
  if (!remote_) return unexpected(CryptoError::key_material_missing);
  const auto position =
      reconstruct_position(remote_position_, key_position_low);
  if (!position) return unexpected(position.error());
  const auto expected_mac =
      remote_->authenticate(position.value(), authenticated);
  if (!expected_mac) return unexpected(expected_mac.error());
  if (!memory::constant_time_equal(received_mac, expected_mac.value()))
    return unexpected(CryptoError::authentication_failed);
  remote_position_ = position.value();
  return position.value();
}

auto CryptoContext::decrypt_remote_payload(std::span<const std::byte> payload,
                                           std::span<std::byte> output,
                                           PacketFamily family,
                                           const WireProfile& profile,
                                           std::uint64_t key_position)
    -> Expected<void, CryptoError> {
  if (output.size() != payload.size())
    return unexpected(CryptoError::invalid_coverage);
  if (policy_ == CryptoPolicy::disabled) {
    if (output.data() != payload.data())
      std::ranges::copy(payload, output.begin());
    return {};
  }
  if (!remote_) return unexpected(CryptoError::key_material_missing);
  const auto offsets = packet_offsets(family, profile);
  if (!offsets) return unexpected(offsets.error());
  const auto cipher_position = checked_addition(
      key_position, offsets.value().cipher_key_position_addition);
  if (!cipher_position) return unexpected(cipher_position.error());
  return remote_->apply_keystream(cipher_position.value(), payload, output);
}

auto CryptoContext::apply_remote_keystream(std::uint64_t key_position,
                                           std::span<std::byte> bytes)
    -> Expected<void, CryptoError> {
  if (!remote_) return unexpected(CryptoError::key_material_missing);
  return remote_->apply_keystream(key_position, bytes, bytes);
}

auto CryptoContext::set_local_key_position(std::uint64_t position) noexcept
    -> Expected<void, CryptoError> {
  if (local_reservation_active_)
    return unexpected(CryptoError::reservation_in_progress);
  local_position_ = position;
  return {};
}

void CryptoContext::rollback(std::uint64_t generation) noexcept {
  if (local_reservation_active_ && generation == reservation_generation_)
    local_reservation_active_ = false;
}

auto CryptoContext::commit(std::uint64_t generation,
                           std::uint64_t next_position) noexcept
    -> Expected<void, CryptoError> {
  if (!local_reservation_active_ || generation != reservation_generation_)
    return unexpected(CryptoError::stale_reservation);
  local_position_ = next_position;
  local_reservation_active_ = false;
  return {};
}

}  // namespace libchiaking::net::protocol::takion
