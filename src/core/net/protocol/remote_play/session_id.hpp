/// @file
/// @brief Declares SessionId, the identifier a console assigns a Remote Play
/// session over the control channel.
///
/// After the control request is accepted the console sends control message
/// 0x33 carrying the session id. The stream connection's BANG handshake must
/// repeat it, so the client keeps it for the life of the session. When the
/// message is unusable the client synthesises one instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a session id could not be read or synthesised.
enum class SessionIdError : std::uint8_t {
  /// @brief The message is shorter than a marker byte and 24 characters.
  too_short,
  /// @brief The id is 79 characters or longer, or the synthesised id did not
  /// fit.
  too_long,
  /// @brief A character of the id is not an ASCII letter or digit.
  invalid_character,
  /// @brief The random input to fallback() is not 48 bytes.
  invalid_random_length,
};

/// @brief The session id a console assigned, or one the client made up when
/// the console's was unusable.
///
/// Held as up to 79 ASCII alphanumeric characters (synthesised ids also hold
/// base64 `+`, `/` and `=`) in wiped storage. Move-only; a default-constructed
/// id is empty. Not shared between threads.
class SessionId {
 public:
  /// @brief Makes an empty id.
  SessionId() noexcept = default;
  SessionId(const SessionId&) = delete;
  SessionId& operator=(const SessionId&) = delete;
  /// @brief Takes over another id.
  SessionId(SessionId&&) noexcept = default;
  /// @brief Takes over another id, wiping what this held.
  SessionId& operator=(SessionId&&) noexcept = default;

  /// @brief The id's characters, without the marker byte.
  ///
  /// @return A view into this object; empty for a default-constructed id.
  std::string_view view() const noexcept { return {bytes_.data(), size_}; }
  /// @brief Whether the console's message began with a byte other than the
  /// expected `J` marker. The id is still used.
  ///
  /// @return True when the marker was unexpected; false for a synthesised
  ///     id.
  bool unexpected_marker() const noexcept { return unexpected_marker_; }
  /// @brief Whether the client made this id up with fallback() rather than
  /// receiving it. A synthesised id makes the stream accept a BANG reply whose
  /// session key does not match.
  ///
  /// @return True for a synthesised id.
  bool synthesized() const noexcept { return synthesized_; }

  /// @brief Reads the session id from a session-id control message payload.
  ///
  /// The first byte is the marker (`J`, recorded but not required); the rest
  /// must be 24 to 78 ASCII letters or digits.
  ///
  /// @param payload The control message payload.
  /// @return The id, or why the payload was refused.
  /// @retval SessionIdError::too_short Fewer than 25 bytes.
  /// @retval SessionIdError::too_long 80 bytes or more.
  /// @retval SessionIdError::invalid_character A byte after the marker is not
  ///     alphanumeric.
  [[nodiscard]] static auto parse(std::span<const std::byte> payload) noexcept
      -> Expected<SessionId, SessionIdError>;

  /// @brief Makes up a session id from the clock and random bytes, for when the
  /// console sends none that can be used.
  ///
  /// The id is the decimal seconds followed by the standard base64 of the 48
  /// random bytes (64 characters).
  ///
  /// @param monotonic_seconds Seconds of the monotonic clock.
  /// @param random48 48 random bytes.
  /// @return The synthesised id, marked synthesized().
  /// @retval SessionIdError::invalid_random_length `random48` is not 48 bytes.
  /// @retval SessionIdError::too_long The digits and base64 do not fit.
  [[nodiscard]] static auto fallback(
      std::uint64_t monotonic_seconds,
      std::span<const std::byte> random48) noexcept
      -> Expected<SessionId, SessionIdError>;

 private:
  /// @brief The id's characters; only the first size_ are meaningful.
  SecretArray<session_id_size_max, char> bytes_;
  /// @brief How many characters the id has.
  std::size_t size_{};
  /// @brief Set when the console's marker byte was not `J`.
  bool unexpected_marker_{false};
  /// @brief Set when the id was made by fallback().
  bool synthesized_{false};
};

}  // namespace libchiaking::net::protocol::remote_play
