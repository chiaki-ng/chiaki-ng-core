/// @file
/// @brief Declares RegistrationMaterial, the keys that protect one Remote Play
/// registration request and its reply.
///
/// Registering a client with a console needs a shared secret before any key
/// has been exchanged. At home that secret is the 8-digit PIN the console
/// shows; over PSN it is the custom data the console and client swap through
/// the PSN session. Either is mixed with a fresh random 16-byte seed into the
/// cipher key ("bright"), the IV seed ("ambassador") and the "aeropause"
/// bytes planted in the request so the console can recover the seed.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why registration material could not be derived.
enum class MaterialError : std::uint8_t {
  /// @brief The target is not one the derivation supports (PSN registration
  /// needs PS4 RP 10.0 or PS5).
  invalid_target,
  /// @brief The random seed is not 16 bytes.
  invalid_ambassador,
  /// @brief A PSN custom-data value has the wrong size.
  invalid_psn_data,
  /// @brief The crypto provider failed while sealing the PSN custom data.
  provider_failure,
};

/// @brief The key, IV seed and aeropause bytes of one registration attempt.
///
/// All three are secrets: wiped on destruction, move-only. Made by
/// derive_local() or derive_psn(); immutable afterwards and safe to read from
/// any thread.
struct RegistrationMaterial {
  /// @brief The 16-byte AES-128 key that encrypts the request payload and
  /// decrypts the reply body.
  SecretArray<rp_key_size> bright{};
  /// @brief The client's random 16-byte IV seed.
  SecretArray<rp_key_size> ambassador{};
  /// @brief 16 bytes derived from the seed and planted in the request's
  /// cleartext prefix, from which the console recovers the seed.
  SecretArray<rp_key_size> aeropause{};

  /// @brief Makes all-zero material, to be overwritten by a derivation.
  RegistrationMaterial() = default;
  RegistrationMaterial(const RegistrationMaterial&) = delete;
  /// @brief Takes over another attempt's material.
  RegistrationMaterial(RegistrationMaterial&&) noexcept = default;
  RegistrationMaterial& operator=(const RegistrationMaterial&) = delete;
  /// @brief Takes over another attempt's material, wiping what this held.
  RegistrationMaterial& operator=(RegistrationMaterial&&) noexcept = default;

  /// @brief Wipes the material.
  ~RegistrationMaterial() noexcept = default;

  /// @brief Derives the material for registering at home with the console's
  /// PIN.
  ///
  /// For PS4 RP 8.0/9.0 (and an unknown PS4) the key is a fixed pad with the
  /// big-endian PIN XORed into its first four bytes, and aeropause byte i is
  /// `(seed[i] - i - 0x29) ^ echo_b[i]`. For PS4 RP 10.0 and PS5 the key is a
  /// per-generation constant with the PIN XORed into its last four bytes, and
  /// aeropause byte i is `(seed[i] ^ key1[i]) + add + i`.
  ///
  /// @param target The console variant being registered.
  /// @param ambassador A fresh random 16-byte seed.
  /// @param pin The PIN the console shows, as a number.
  /// @return The material, or why it could not be made.
  /// @retval MaterialError::invalid_target The target is not a known variant.
  /// @retval MaterialError::invalid_ambassador The seed is not 16 bytes.
  [[nodiscard]] static Expected<RegistrationMaterial, MaterialError>
  derive_local(RegistrationTarget target, std::span<const std::byte> ambassador,
               std::uint32_t pin);

  /// @brief Derives the material for registering over PSN, where no PIN is
  /// shown.
  ///
  /// Starts from the PIN-0 material, then XORs into the key the console's
  /// custom data encrypted with RegistrationCipher (key `data1`, seed `data2`,
  /// counter 0), and recomputes aeropause with the PSN offsets.
  ///
  /// @param provider The provider that runs the cipher.
  /// @param target PS4 RP 10.0 or PS5.
  /// @param ambassador A fresh random 16-byte seed.
  /// @param custom_data1 The 16 bytes the console published through the PSN
  ///     session.
  /// @param data1 The client's 16 random bytes sent through the session, used
  ///     as the AES key.
  /// @param data2 The client's 16 random bytes sent through the session, used
  ///     as the IV seed.
  /// @return The material, or why it could not be made.
  /// @retval MaterialError::invalid_target The target is not PS4 RP 10.0 or
  ///     PS5.
  /// @retval MaterialError::invalid_ambassador The seed is not 16 bytes.
  /// @retval MaterialError::invalid_psn_data A custom-data value has the wrong
  ///     size.
  /// @retval MaterialError::provider_failure The cipher failed.
  [[nodiscard]] static Expected<RegistrationMaterial, MaterialError> derive_psn(
      platform::CryptoProvider& provider, RegistrationTarget target,
      std::span<const std::byte> ambassador,
      std::span<const std::byte> custom_data1, std::span<const std::byte> data1,
      std::span<const std::byte> data2);

  /// @brief The fixed pad of the PS4 RP 8.0/9.0 derivations, shared with
  /// AuthMaterial.
  static constexpr std::array<std::uint8_t, rp_key_size> echo_b{
      0xe1, 0xec, 0x9c, 0x3a, 0xdd, 0xbd, 0x08, 0x85,
      0xfc, 0x0e, 0x1d, 0x78, 0x90, 0x32, 0xc0, 0x04};

 private:
  /// @brief PS4 RP 10.0 base key.
  static constexpr std::array<std::uint8_t, rp_key_size> ps4_key_0{
      0xce, 0xbc, 0xb6, 0x40, 0x08, 0x07, 0x76, 0x04,
      0x7b, 0x85, 0xe8, 0x5b, 0xf3, 0xec, 0x94, 0x63};
  /// @brief PS5 base key.
  static constexpr std::array<std::uint8_t, rp_key_size> ps5_key_0{
      0xd8, 0x60, 0xe1, 0x46, 0xbd, 0xb0, 0xbd, 0x94,
      0xb4, 0x60, 0x07, 0x0b, 0xb1, 0x4f, 0xe7, 0x23};
  /// @brief PS4 RP 10.0 aeropause mask.
  static constexpr std::array<std::uint8_t, rp_key_size> ps4_key_1{
      0x5f, 0xeb, 0xfe, 0xf8, 0x91, 0xb0, 0x42, 0x46,
      0xa6, 0x25, 0xa8, 0x7a, 0x6b, 0xcc, 0x49, 0x58};
  /// @brief PS5 aeropause mask.
  static constexpr std::array<std::uint8_t, rp_key_size> ps5_key_1{
      0xa8, 0x37, 0x61, 0xb5, 0x27, 0xfc, 0x07, 0x6e,
      0x62, 0x43, 0x3a, 0xa7, 0x01, 0x95, 0x8a, 0x85};
  /// @brief PS4 RP 8.0/9.0 base key.
  static constexpr std::array<std::uint8_t, rp_key_size> pre10_key{
      0x3f, 0x1c, 0xc4, 0xb6, 0xdc, 0xbb, 0x3e, 0xcc,
      0x50, 0xba, 0xed, 0xef, 0x97, 0x34, 0xc7, 0xc9};
  /// @brief The aeropause offset for PS4 at home (also subtracted for RP
  /// 8.0/9.0).
  static constexpr std::uint8_t ps4_aeropause_add = 0x29;
  /// @brief The aeropause offset for PS5 at home (-0x2d modulo 256).
  static constexpr std::uint8_t ps5_aeropause_add = 0x100u - 0x2du;
  /// @brief The aeropause offset for PS4 RP 10.0 over PSN.
  static constexpr std::uint8_t psn_ps4_aeropause_add = 0xd7;
  /// @brief The aeropause offset for PS5 over PSN.
  static constexpr std::uint8_t psn_ps5_aeropause_add = 0x2b;
  /// @brief Converts a table byte for copying into a secret array.
  ///
  /// @param value The byte.
  /// @return The same value as std::byte.
  static constexpr auto to_byte(std::uint8_t value) noexcept -> std::byte {
    return std::byte{value};
  }
};

}  // namespace libchiaking::net::protocol::remote_play
