#include "src/core/psn/remote_play/holepunch_message.hpp"

#include <utility>

namespace libchiaking::psn::remote_play {

auto HolepunchMessage::body_of(std::string_view payload)
    -> Expected<SecretString, BodyError> {
  if (payload.contains('\0')) return unexpected(BodyError::invalid_payload);
  const auto position = payload.find(body_marker);
  if (position == std::string_view::npos)
    return unexpected(BodyError::missing_body);
  const auto suffix = payload.substr(position + body_marker.size());
  if (suffix.empty()) return unexpected(BodyError::missing_body);
  return SecretString{suffix};
}

auto HolepunchMessage::refused(
    const psn::rest::Notification::MessageFault& error) noexcept
    -> PayloadError {
  using namespace psn::rest::bad_reply;
  if (error.is<TooLarge>())
    return {PayloadErrorKind::json, json::ParseError::too_large};
  if (error.is<TooDeep>())
    return {PayloadErrorKind::json, json::ParseError::too_deep};
  if (error.is<InvalidJson>())
    return {PayloadErrorKind::json, json::ParseError::invalid_json};
  return {PayloadErrorKind::invalid_schema, {}};
}

auto HolepunchMessage::payload_of(const psn::rest::Notification& notification)
    -> Expected<SecretString, PayloadError> {
  if (notification.type() !=
      psn::rest::NotificationType::session_message_created)
    return unexpected(PayloadError{PayloadErrorKind::invalid_type, {}});
  return notification.session_message_payload().transform_error(refused);
}

auto HolepunchMessage::decode(const psn::rest::Notification& notification)
    -> Expected<net::nat::holepunch::SessionMessage, Error> {
  auto payload = payload_of(notification);
  if (!payload) return unexpected(Error{payload.error()});
  auto body = body_of(payload.value().view());
  if (!body) return unexpected(Error{body.error()});
  auto message =
      net::nat::holepunch::MessageCodec::decode_session(body.value().view());
  if (!message) return unexpected(Error{message.error()});
  return std::move(message.value());
}

}  // namespace libchiaking::psn::remote_play
