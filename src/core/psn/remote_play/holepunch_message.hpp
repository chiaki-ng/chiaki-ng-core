/// @file
/// @brief Declares HolepunchMessage, which takes a holepunch message out of a
/// session message push notification.
///
/// The console and the client negotiate their UDP routes by session messages
/// whose payload is `ver=1.0, type=text, body=<message>`; the body is the
/// holepunch message net::nat::holepunch::MessageCodec decodes.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/utils/json/json.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace libchiaking::psn::remote_play {

/// @brief Reads holepunch messages from push notifications. Stateless.
class HolepunchMessage {
 public:
  /// @brief Why a payload has no usable body.
  enum class BodyError : std::uint8_t {
    /// @brief No `body=`, or nothing after it.
    missing_body,
    /// @brief The payload holds a NUL.
    invalid_payload,
  };

  /// @brief Why a notification has no usable payload.
  enum class PayloadErrorKind : std::uint8_t {
    /// @brief It is not a session-message notification.
    invalid_type,
    /// @brief Its body is not readable JSON.
    json,
    /// @brief It lacks `body.data.sessionMessage.payload`.
    invalid_schema,
  };

  /// @brief A notification without a usable payload. Plain data.
  struct PayloadError {
    /// @brief Why.
    PayloadErrorKind kind{};
    /// @brief The parse failure, for PayloadErrorKind::json.
    json::ParseError json_error{};
  };

  /// @brief Any failure of decode(): of the payload, the body, or the
  /// message.
  using Error =
      std::variant<PayloadError, BodyError, net::nat::holepunch::MessageError>;

  /// @brief Reads the holepunch message a notification carries.
  ///
  /// @param notification A push notification.
  /// @return The message.
  /// @retval PayloadError It is not a session message or has no payload.
  /// @retval BodyError The payload has no body.
  /// @retval net::nat::holepunch::MessageError The body is not a holepunch
  ///     message.
  [[nodiscard]] static auto decode(const psn::rest::Notification& notification)
      -> Expected<net::nat::holepunch::SessionMessage, Error>;
  /// @brief Reads a session message's payload.
  ///
  /// @param notification A push notification.
  /// @return The payload; secret.
  /// @retval PayloadErrorKind::invalid_type It is not a session message.
  /// @retval PayloadErrorKind::json The body is too large, too deep or not
  ///     JSON.
  /// @retval PayloadErrorKind::invalid_schema The payload is missing.
  [[nodiscard]] static auto payload_of(
      const psn::rest::Notification& notification)
      -> Expected<SecretString, PayloadError>;
  /// @brief Takes the body out of a payload.
  ///
  /// @param payload The payload.
  /// @return Everything after the first `body=`; secret.
  /// @retval BodyError::invalid_payload The payload holds a NUL.
  /// @retval BodyError::missing_body No `body=`, or nothing after it.
  [[nodiscard]] static auto body_of(std::string_view payload)
      -> Expected<SecretString, BodyError>;

 private:
  /// @brief What precedes the body in a payload.
  static constexpr std::string_view body_marker = "body=";

  /// @brief Translates a notification read failure.
  ///
  /// @param error The failure.
  /// @return A json failure for too_large, too_deep and invalid_json;
  ///     invalid_schema otherwise.
  static auto refused(
      const psn::rest::Notification::MessageFault& error) noexcept
      -> PayloadError;
};

}  // namespace libchiaking::psn::remote_play
