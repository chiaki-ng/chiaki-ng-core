/// @file
/// @brief Declares RegisteredConsole, a console's raw reply to a successful
/// registration, and the registration vocabulary under the protocol's names.
#pragma once

#include "libchiaking/detail/consoles/pairing_target.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace libchiaking::net::protocol::remote_play {

/// @brief The registration protocol variant, under the protocol's name.
using RegistrationTarget = consoles::RegistrationTarget;

/// @brief The raw contents of a console's reply to a successful registration.
///
/// Its secrets (the access point key and the two pairing keys) live in wiping
/// storage, so the record is move-only and wipes them member by member.
/// Frontends receive the same facts in refined form as
/// consoles::PairedConsole.
struct RegisteredConsole {
  /// @brief The protocol variant the console answered as.
  RegistrationTarget target{};
  /// @brief The access point SSID the console states in its reply, NUL
  /// terminated; left empty unless a stated value fits.
  std::array<char, 48> ap_ssid{};
  /// @brief The access point BSSID the console states in its reply, as NUL
  /// terminated text; left empty unless a stated value fits.
  std::array<char, 32> ap_bssid{};
  /// @brief The access point name the console states in its reply, NUL
  /// terminated; left empty unless a stated value fits.
  std::array<char, 32> ap_name{};
  /// @brief The console's nickname, NUL terminated; left empty unless a
  /// stated value fits.
  std::array<char, 32> nickname{};
  /// @brief The access point key the console states in its reply, NUL
  /// terminated. Secret; wiped on destruction.
  SecretArray<80, char> ap_key{};
  /// @brief The console's MAC address.
  std::array<std::byte, mac_address_size> mac{};
  /// @brief The registration key. Secret; wiped on destruction.
  SecretArray<rp_registration_key_size> registration_key{};
  /// @brief The Remote Play key. Secret; wiped on destruction.
  SecretArray<rp_key_size> key{};
  /// @brief The key type number the console stated.
  std::uint32_t key_type{};
  /// @brief The console PIN value carried with the registration.
  std::uint32_t console_pin{};
};

}  // namespace libchiaking::net::protocol::remote_play
