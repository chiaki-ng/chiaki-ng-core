/// @file
/// @brief Declares RegistrationRequestCodec, the encoder of the Remote Play
/// registration request.
///
/// Registration is one HTTP POST, to port 9295 at home or over RUDP through
/// PSN. The body is a 480-byte cleartext prefix of filler with the aeropause
/// bytes planted in it, followed by `Client-Type` and `Np-AccountId` (or, for
/// old PS4s, `Np-Online-Id`) lines encrypted with the registration material.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include "limits.hpp"
#include "material.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief The address sent in the request's `HOST` header when the caller
/// gives none; what the Windows client sends.
inline constexpr std::string_view default_local_address = "10.0.2.15";

/// @brief Why the registration request head could not be encoded.
enum class HeaderError : std::uint8_t {
  /// @brief The target has no registration path.
  invalid_target,
  /// @brief The host is empty, longer than 45 characters, holds characters
  /// other
  /// than hex digits, `.` and `:`, or carries an invalid zone.
  invalid_host,
  /// @brief The payload size is 0 or above max_payload_size, or the head would
  /// not fit.
  invalid_payload_size,
};

/// @brief Why the registration request body could not be encoded.
enum class PayloadError : std::uint8_t {
  /// @brief The target is unknown, the account id is not 8 bytes, or the online
  /// id is too long or holds a NUL.
  invalid_argument,
  /// @brief The cipher failed.
  provider_failure,
};

/// @brief Encodes the head and body of a registration request.
///
/// Stateless; every member is static and may be called from any thread.
class RegistrationRequestCodec {
 public:
  /// @brief Encodes the request head for a body of the given size.
  ///
  /// Writes `POST <path> HTTP/1.1`, then a second ` HTTP/1.1` line (a quirk
  /// kept byte for byte from the reference client), `HOST`, `User-Agent:
  /// remoteplay Windows`, `Connection: close`, `Content-Length` and, except for
  /// PS4 RP 8.0, `RP-Version`. A `%zone` suffix on the host is dropped. The
  /// paths are
  /// `/sce/rp/regist` (PS4 RP 8.0/9.0), `/sie/ps4/rp/sess/rgst` (PS4 RP 10.0 or
  /// unknown) and `/sie/ps5/rp/sess/rgst`.
  ///
  /// @param target The console variant being registered.
  /// @param payload_size The body's size in bytes; must exceed the head's.
  /// @param host The client's local address as the console should see it.
  /// @return The head, or why it could not be made.
  /// @retval HeaderError::invalid_target The target has no path.
  /// @retval HeaderError::invalid_host The host is unusable.
  /// @retval HeaderError::invalid_payload_size The size is out of range.
  [[nodiscard]] static Expected<std::string, HeaderError> encode_header(
      RegistrationTarget target, std::size_t payload_size,
      std::string_view host = default_local_address);

  /// @brief Encodes the request body.
  ///
  /// The inner text is `Client-Type: <type>\r\nNp-AccountId: <base64>\r\n`,
  /// where the type is "Windows" for PS4 RP 8.0/9.0 and a fixed 64-character
  /// hex token otherwise. For PS4 RP 8.0/9.0 an online id may replace the
  /// account id (`Np-Online-Id`). The text is encrypted at counter 0 after the
  /// prefix. The aeropause bytes sit at offset 0x11c of the prefix for PS4 RP
  /// 8.0/9.0; for newer consoles the high half sits at 0xc7 and the low half at
  /// 0x191.
  ///
  /// @param provider The provider that runs the cipher.
  /// @param target The console variant being registered.
  /// @param material The attempt's registration material.
  /// @param account_id The 8-byte PSN account id; ignored when an online id is
  ///     used.
  /// @param online_id The PSN online id, honoured only for PS4 RP 8.0/9.0.
  /// @return The body, 480 bytes plus the inner text.
  /// @retval PayloadError::invalid_argument The target, account id or online id
  ///     is unusable.
  /// @retval PayloadError::provider_failure The cipher failed.
  [[nodiscard]] static Expected<SecretBytes, PayloadError> encode_payload(
      platform::CryptoProvider& provider, RegistrationTarget target,
      const RegistrationMaterial& material,
      std::span<const std::byte> account_id,
      std::optional<std::string_view> online_id = std::nullopt);

  /// @brief The capacity of the request head, in bytes.
  static constexpr std::size_t max_header_size = 256;
  /// @brief The largest body accepted, in bytes.
  static constexpr std::size_t max_payload_size = 1024;

 private:
  /// @brief Whether a host is a plausible IPv4 or IPv6 address of at most 45
  /// characters.
  ///
  /// @param host The host, without zone.
  /// @return True when acceptable.
  static bool host_is_valid(std::string_view host);
  /// @brief Removes an IPv6 `%zone` suffix.
  ///
  /// @param host The host.
  /// @return The host without its zone, or an empty view when the zone holds
  ///     characters other than letters, digits, `_`, `-` and `.`.
  static std::string_view without_zone(std::string_view host);
  /// @brief The registration path of a console variant.
  ///
  /// @param target The variant.
  /// @return The path, or nothing for a value outside the enumeration.
  static auto request_path(RegistrationTarget target) noexcept
      -> std::optional<std::string_view>;
  /// @brief The longest inner text, in bytes.
  static constexpr std::size_t inner_text_max = request_inner_size - 1;
  /// @brief The size of each half of the aeropause bytes split across the
  /// newer layout.
  static constexpr std::size_t aeropause_half_size = 8;
  /// @brief Where all 16 aeropause bytes go for PS4 RP 8.0/9.0.
  static constexpr std::size_t legacy_aeropause_offset = 0x11c;
  /// @brief Where aeropause bytes 8..15 go for PS4 RP 10.0 and PS5.
  static constexpr std::size_t modern_aeropause_high_offset = 0xc7;
  /// @brief Where aeropause bytes 0..7 go for PS4 RP 10.0 and PS5.
  static constexpr std::size_t modern_aeropause_low_offset = 0x191;
  /// @brief `Client-Type` for PS4 RP 8.0/9.0.
  static constexpr char client_type_legacy[] = "Windows";
  /// @brief `Client-Type` for PS4 RP 10.0 and PS5.
  static constexpr char client_type_modern[] =
      "dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f";
  /// @brief Whether an online id holds no NUL.
  ///
  /// @param online The online id.
  /// @return True when usable.
  static bool valid_online(std::string_view online);
  /// @brief Whether a variant uses the pre-RP-10 layout (PS4 RP 8.0, 9.0 or
  /// unknown).
  ///
  /// @param target The variant.
  /// @return True for the old layout.
  static bool legacy_target(RegistrationTarget target);
};

}  // namespace libchiaking::net::protocol::remote_play
