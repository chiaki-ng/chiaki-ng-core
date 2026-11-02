/// @file
/// @brief Declares RegistrationResponseCodec, the reader of a console's reply
/// to a registration request.
///
/// A console that accepts a registration answers 200 with an encrypted body.
/// Decrypted with the attempt's material, the body is a list of `Name: value`
/// lines: the console's MAC, registration key, morning key (`RP-Key`),
/// nickname and, for some consoles, access point details. A refusal carries
/// only a status and an `RP-Application-Reason`.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include "limits.hpp"
#include "src/core/net/transport/http/status.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why the reply head could not be read.
enum class ResponseHeaderError { incomplete, invalid, too_large };
/// @var ResponseHeaderError::incomplete
/// @brief The head has not fully arrived; read more.
/// @var ResponseHeaderError::invalid
/// @brief The bytes are not an HTTP head, or a 200 reply has no positive
/// `Content-Length`.
/// @var ResponseHeaderError::too_large
/// @brief The reply, head and declared body together, exceeds 1500 bytes.

/// @brief The parts of a registration reply head the caller needs.
///
/// Plain data.
struct ResponseHeader {
  /// @brief The HTTP status, or 0 when out of range.
  std::uint16_t status{};
  /// @brief The `RP-Application-Reason`, parsed as hexadecimal, when present.
  /// The last copy counts.
  std::optional<std::uint32_t> application_reason{};
  /// @brief How many bytes the head took; the body starts there.
  std::size_t header_size{};
  /// @brief The declared body size for a 200 reply; 0 otherwise.
  std::size_t content_size{};
};

/// @brief Why a decrypted reply body could not be read as a registered
/// console.
enum class ResponsePayloadError : std::uint8_t {
  /// @brief The target is not a known variant.
  invalid_target,
  /// @brief The body's lines are not valid header fields.
  malformed,
  /// @brief Reserved; the current reader does not produce it.
  truncated,
  /// @brief Reserved; the current reader does not produce it.
  nul_byte,
  /// @brief The body exceeds 1500 bytes.
  oversize,
  /// @brief The access point key does not fit its record with its
  /// terminator.
  invalid_value,
  /// @brief Reserved; the current reader does not produce it.
  duplicate_field,
  /// @brief The registration key is missing, empty, starts with a zero byte
  /// or is longer than 16 bytes, or it, the morning key or the MAC is not
  /// valid hex of the right length.
  missing_field,
};

/// @brief The status of a reply that could not be parsed properly, scanned
/// leniently for diagnostics.
struct ResponseStatus {
  /// @brief The status, or 0 when none could be read.
  std::uint16_t status{};
  /// @brief The `RP-Application-Reason` when a line carried one.
  std::optional<std::uint32_t> application_reason{};
};

/// @brief Reads registration reply heads and decrypted bodies.
///
/// Stateless; every member is static and may be called from any thread.
class RegistrationResponseCodec {
 public:
  /// @brief Reads the reply head and the body size it declares.
  ///
  /// @param available The bytes received so far.
  /// @return The head's status, reason, length and body size.
  /// @retval ResponseHeaderError::too_large More than 1500 bytes, or a head and
  ///     declared body that would exceed it.
  /// @retval ResponseHeaderError::invalid Not an HTTP head, or a 200 without a
  ///     positive `Content-Length`.
  /// @retval ResponseHeaderError::incomplete The head is not complete yet.
  [[nodiscard]] static Expected<ResponseHeader, ResponseHeaderError>
  parse_header(std::span<const std::byte> available) noexcept;

  /// @brief Reads the registered console from a decrypted reply body.
  ///
  /// Field names carry the generation: `PS5-Mac`, `PS5-RegistKey` and
  /// `PS5-Nickname` for a PS5, `PS4-...` otherwise; `RP-Key`, `RP-KeyType` and
  /// the `AP-*` fields are shared. Any trailing partial line or NUL padding is
  /// dropped. When a field repeats, the first copy wins. Text fields that do
  /// not fit their arrays are left empty.
  ///
  /// @param target The variant that was registered.
  /// @param payload The decrypted body.
  /// @return The console, with its secrets in wiped storage.
  /// @retval ResponsePayloadError::invalid_target The target is unknown.
  /// @retval ResponsePayloadError::oversize The body is too large.
  /// @retval ResponsePayloadError::malformed The lines are not header fields.
  /// @retval ResponsePayloadError::missing_field The registration key, morning
  ///     key or MAC is missing or unreadable, or the registration key is empty
  ///     or starts with a zero byte.
  [[nodiscard]] static Expected<RegisteredConsole, ResponsePayloadError>
  parse_payload(RegistrationTarget target,
                std::span<const std::byte> payload) noexcept;

  /// @brief Scans a reply that parse_header() refused for its status and
  /// `RP-Application-Reason`, tolerating truncation and NUL padding.
  ///
  /// @param available The bytes received.
  /// @return Whatever could be read; zeros when nothing could.
  [[nodiscard]] static auto scan_status(
      std::span<const std::byte> available) noexcept -> ResponseStatus;

 private:
  /// @brief Decodes hex that must fill a buffer exactly.
  ///
  /// @tparam Buffer A byte array.
  /// @param out The buffer; zeroed when decoding fails.
  /// @param hex The text.
  /// @return True when the text filled the buffer.
  template <class Buffer>
  [[nodiscard]] static auto parse_exact_hex(Buffer& out,
                                            std::string_view hex) noexcept
      -> bool;
  /// @brief Makes a NUL-terminated secret text field from a value.
  ///
  /// @tparam Size The field's size, terminator included.
  /// @param value The value.
  /// @return The field, zero after the value.
  /// @retval ResponsePayloadError::invalid_value The value does not fit with
  ///     its terminator.
  template <std::size_t Size>
  [[nodiscard]] static auto secret_field(std::string_view value) noexcept
      -> Expected<SecretArray<Size, char>, ResponsePayloadError>;
  /// @brief Makes a NUL-terminated text field from a value the console states
  /// for display.
  ///
  /// @tparam Size The field's size, terminator included.
  /// @param value The value.
  /// @return The field, zero after the value, or nothing when the value does
  ///     not fit with its terminator.
  template <std::size_t Size>
  [[nodiscard]] static auto text_field(std::string_view value) noexcept
      -> std::optional<std::array<char, Size>>;
  /// @brief The body up to and including its last newline before any NUL, so
  /// a partial final line is ignored.
  ///
  /// @param payload The decrypted body.
  /// @return The complete lines.
  static auto complete_lines(std::span<const std::byte> payload) noexcept
      -> std::span<const std::byte>;
  /// @brief The blank line appended to the body so it parses as a header block.
  static constexpr std::array<char, 2> payload_terminator{'\r', '\n'};
};

}  // namespace libchiaking::net::protocol::remote_play
