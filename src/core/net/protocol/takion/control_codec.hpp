/// @file
/// @brief Declares ControlCodec, which encodes control messages to and
/// decodes them from the protobuf `tkproto.TakionMessage` wire form through
/// the generated nanopb code.
///
/// nanopb keeps enum values the schema does not know as their numbers, so an
/// unknown message type, Senkusha command or controller kind is kept (as
/// UnsupportedControlMessage, UnknownSenkushaCommand or
/// UnknownControllerConnectionType) rather than read as a default. The message
/// type is read before the full decode, so an unmodelled type is reported as
/// unsupported whatever its payload holds.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/control_message.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace libchiaking::net::protocol::takion {

/// @brief Largest control message, in bytes, a codec accepts by default
/// (1 MiB).
inline constexpr std::size_t default_control_message_size_limit =
    default_maximum_message_size;

/// @brief Why a control message could not be encoded or decoded.
enum class ControlCodecErrorReason {
  /// @brief It exceeds the codec's size limit.
  message_too_large,
  /// @brief The bytes are not a valid protobuf.
  malformed_message,
  /// @brief A required field is missing.
  missing_required_field,
  /// @brief The type names a payload the message does not carry.
  missing_payload,
  /// @brief The message cannot be sent: an unmodelled type or an unknown audio
  /// state kind.
  unsupported_for_encoding,
  /// @brief Serialisation failed.
  serialization_failed,
};

/// @brief A control codec failure.
struct ControlCodecError {
  /// @brief Why.
  ControlCodecErrorReason reason;
  /// @brief The message's size, in bytes, where known.
  std::size_t size{};
  /// @brief The limit exceeded, for message_too_large.
  std::size_t limit{};

  /// @brief Compares every field.
  ///
  /// @return Whether both are the same.
  constexpr bool operator==(const ControlCodecError&) const noexcept = default;
};

/// @brief Converts control messages to and from their protobuf bytes.
///
/// A small value holding only its size limit; const and safe to use from any
/// thread. Secret fields are wiped from the protobuf objects it uses
/// (ScrubbedTakionMessage).
class ControlCodec {
 public:
  /// @brief Makes a codec.
  ///
  /// @param message_size_limit Largest message accepted, in bytes.
  explicit constexpr ControlCodec(
      std::size_t message_size_limit =
          default_control_message_size_limit) noexcept
      : message_size_limit_(message_size_limit) {}

  /// @brief Encodes a control message.
  ///
  /// @param message The message.
  /// @return The TakionMessage bytes, in a wiping buffer.
  /// @retval ControlCodecErrorReason::unsupported_for_encoding The message is
  ///     not one the library sends.
  /// @retval ControlCodecErrorReason::missing_required_field A required field
  /// is
  ///     missing.
  /// @retval ControlCodecErrorReason::message_too_large It exceeds the limit.
  /// @retval ControlCodecErrorReason::serialization_failed nanopb failed.
  [[nodiscard]] auto encode(const ControlMessage& message) const
      -> Expected<SecretBytes, ControlCodecError>;

  /// @brief Decodes a control message.
  ///
  /// @param bytes The TakionMessage bytes.
  /// @return The message; types above METRICSOVERLAY (35) and unmodelled types
  ///     become UnsupportedControlMessage.
  /// @retval ControlCodecErrorReason::message_too_large It exceeds the limit.
  /// @retval ControlCodecErrorReason::malformed_message It is not a valid
  ///     protobuf, or a closed enum value is out of range.
  /// @retval ControlCodecErrorReason::missing_required_field The type or
  /// another
  ///     required field is missing.
  /// @retval ControlCodecErrorReason::missing_payload The type's payload is
  ///     absent.
  [[nodiscard]] auto decode(std::span<const std::byte> bytes) const
      -> Expected<ControlMessage, ControlCodecError>;

  /// @brief Reads the configured size limit.
  ///
  /// @return Bytes.
  constexpr std::size_t message_size_limit() const noexcept {
    return message_size_limit_;
  }

  /// @brief Reads the size limit in force, which protobuf caps at `INT_MAX`.
  ///
  /// @return Bytes.
  constexpr std::size_t effective_message_size_limit() const noexcept {
    return std::min(message_size_limit_,
                    static_cast<std::size_t>(std::numeric_limits<int>::max()));
  }

 private:
  /// @brief The nanopb side of the codec: building and reading the
  /// generated message, kept out of this header.
  class Wire;

  /// @brief Largest message accepted, in bytes.
  std::size_t message_size_limit_;
};

}  // namespace libchiaking::net::protocol::takion
