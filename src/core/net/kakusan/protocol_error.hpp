/// @file
/// @brief Declares the error vocabulary of the Kakusan protocol.
#pragma once

#include "libchiaking/detail/expected.hpp"

#include <cstdint>
#include <string_view>

namespace libchiaking::net::kakusan {

/// @brief A failure as the Kakusan protocol names it.
///
/// Frontends meet these as libchiaking::kakusan::Refusal, which
/// libchiaking::kakusan::ProtocolRefusal builds from an Error; Refusal adds the
/// failures of the session classes themselves. The note on each value names
/// the Refusal kind it becomes.
enum class Error : std::uint8_t {
  /// @brief An invitation or reply text could not be read, or does not belong
  /// here. Becomes Refusal::Kind::invalid_text.
  invalid_block,
  /// @brief The invitation is past its expiry time. Becomes
  /// Refusal::Kind::expired.
  expired,
  /// @brief The other side speaks a protocol version this one does not.
  /// Becomes Refusal::Kind::unsupported_version.
  version,
  /// @brief A device presented a different identity from the one expected.
  /// Becomes Refusal::Kind::identity_changed.
  identity_changed,
  /// @brief The two sides do not agree on what was exchanged. Becomes
  /// Refusal::Kind::tampered.
  transcript,
  /// @brief The verification codes did not match, or too many attempts failed
  /// recently. Becomes Refusal::Kind::verification_failed.
  pairing_failed,
  /// @brief No network path between the two devices could be established.
  /// Becomes Refusal::Kind::unreachable.
  no_path,
  /// @brief There is no free seat, or the host is tracking too many
  /// invitations. Becomes Refusal::Kind::no_free_seat.
  no_capacity,
  /// @brief The console did not let the guest take the seat. Becomes
  /// Refusal::Kind::console_refused_seat.
  source_join_failed,
  /// @brief A message could not be encoded or decoded. Becomes
  /// Refusal::Kind::malformed.
  codec,
  /// @brief A step did not complete in time. Becomes
  /// Refusal::Kind::timed_out.
  timeout,
  /// @brief The other side broke the protocol's rules. Becomes
  /// Refusal::Kind::protocol.
  protocol,
  /// @brief The host's stream is in a form that cannot be forwarded to
  /// guests. Becomes Refusal::Kind::stream_not_shareable.
  source_format,
  /// @brief The connection could not keep up with the stream. Becomes
  /// Refusal::Kind::connection_too_slow.
  congestion,
  /// @brief The session being shared has ended. Becomes
  /// Refusal::Kind::session_ended.
  source_ended,
  /// @brief A cryptographic operation the protocol needs was not available.
  /// Becomes Refusal::Kind::internal.
  crypto_unavailable,
  /// @brief Something went wrong inside the library, or a call named a guest
  /// or a state that does not apply. Becomes Refusal::Kind::internal.
  internal,
  /// @brief A caller passed a value the protocol does not accept. Becomes
  /// Refusal::Kind::invalid_argument.
  invalid_argument,
};

/// @brief An Expected whose error is a Kakusan Error.
/// @tparam T The value type on success.
template <class T>
using Result = Expected<T, Error>;

/// @brief The protocol's upper-case name for an error, for logs.
/// @param error The error to name.
/// @return A string with static lifetime, such as "NO_PATH".
constexpr auto error_name(Error error) noexcept -> std::string_view {
  switch (error) {
    case Error::invalid_block:
      return "INVALID_BLOCK";
    case Error::expired:
      return "EXPIRED";
    case Error::version:
      return "VERSION";
    case Error::identity_changed:
      return "IDENTITY_CHANGED";
    case Error::transcript:
      return "TRANSCRIPT";
    case Error::pairing_failed:
      return "PAIRING_FAILED";
    case Error::no_path:
      return "NO_PATH";
    case Error::no_capacity:
      return "NO_CAPACITY";
    case Error::source_join_failed:
      return "SOURCE_JOIN_FAILED";
    case Error::codec:
      return "CODEC";
    case Error::timeout:
      return "TIMEOUT";
    case Error::protocol:
      return "PROTOCOL";
    case Error::source_format:
      return "SOURCE_FORMAT";
    case Error::congestion:
      return "CONGESTION";
    case Error::source_ended:
      return "SOURCE_ENDED";
    case Error::crypto_unavailable:
      return "CRYPTO_UNAVAILABLE";
    case Error::internal:
      return "INTERNAL";
    case Error::invalid_argument:
      return "INVALID_ARGUMENT";
  }
  return "INTERNAL";
}

/// @brief The protocol's number for an error.
/// @param error The error to number.
/// @return 1 for Error::invalid_argument; for every other error, 3 plus its
///     position in the enumeration, so Error::invalid_block is 3 and
///     Error::internal is 19. Where a Refusal kind corresponds, Refusal::code
///     gives the same number.
constexpr auto error_code(Error error) noexcept -> int {
  constexpr int invalid_argument_code = 1;
  constexpr int first_protocol_code = 3;
  if (error == Error::invalid_argument) return invalid_argument_code;
  return first_protocol_code + static_cast<int>(error);
}

}  // namespace libchiaking::net::kakusan
