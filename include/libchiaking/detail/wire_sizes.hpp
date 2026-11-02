/// @file
/// @brief Declares the fixed sizes of identifiers and keys in the Remote Play
/// and PlayStation Network protocols.
#pragma once

#include <cstddef>

namespace libchiaking {

/// @brief Size in bytes of a Remote Play device identifier (DID).
inline constexpr std::size_t rp_did_size = 32;
/// @brief Size in bytes of the Remote Play key a console issues when it is
/// paired.
inline constexpr std::size_t rp_key_size = 16;
/// @brief Size in bytes of a Remote Play nonce.
inline constexpr std::size_t rp_nonce_size = 16;
/// @brief Size in bytes of the registration key a console issues when it is
/// paired.
inline constexpr std::size_t rp_registration_key_size = 16;
/// @brief Size in bytes of a PlayStation Network account id in binary form.
inline constexpr std::size_t psn_account_id_size = 8;
/// @brief Size in bytes of a PlayStation Network device unique id.
inline constexpr std::size_t psn_device_uid_size = 32;
/// @brief Size in bytes of a PlayStation Network custom data block.
inline constexpr std::size_t psn_custom_data_size = 16;
/// @brief Largest size in bytes of a session id.
inline constexpr std::size_t session_id_size_max = 80;
/// @brief Length in characters of a device unique id (DUID) written as text.
inline constexpr std::size_t duid_text_length = 48;
/// @brief Size in bytes of a network hardware (MAC) address.
inline constexpr std::size_t mac_address_size = 6;

}  // namespace libchiaking
