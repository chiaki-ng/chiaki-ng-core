#include "src/core/net/nat/holepunch/holepunch_codec.hpp"

#include "src/core/utils/json/wire_dialect.hpp"

namespace libchiaking::net::nat::holepunch {

auto MessageCodec::action_name(Action action) noexcept -> std::string_view {
  switch (action) {
    case Action::offer:
      return "OFFER";
    case Action::accept:
      return "ACCEPT";
    case Action::terminate:
      return "TERMINATE";
    case Action::result:
      return "RESULT";
    case Action::unknown:
      break;
  }
  return "UNKNOWN";
}

auto MessageCodec::format_short(Action action, std::uint16_t req_id,
                                std::uint16_t error) -> std::string {
  const auto text = json::ps::write<json::ps::body_options_with_nulls>(
      Envelope<Unrequested>{action_name(action), req_id, error, {}});
  return text ? std::string{text.value().view()} : std::string{};
}

auto MessageCodec::format_full(Action action, std::uint16_t req_id,
                               std::uint16_t error, const ConnRequest& request,
                               std::int64_t account_id)
    -> Expected<SecretString, FullMessageError> {
  if (action != Action::offer && action != Action::accept)
    return unexpected(FullMessageError::invalid_action);
  auto conn_request = ConnRequestCodec::encode(request, account_id);
  if (!conn_request) {
    return unexpected(conn_request.error() == ConnRequestEncodeError::too_large
                          ? FullMessageError::too_large
                          : FullMessageError::invalid_candidate);
  }
  auto text = json::ps::write<json::ps::body_options_with_nulls>(
      Envelope<ConnRequestEntry>{action_name(action), req_id, error,
                                 std::move(conn_request.value())});
  if (!text || text.value().size() > max_full_message_bytes)
    return unexpected(FullMessageError::too_large);
  return std::move(text.value());
}

auto MessageCodec::decode_session(std::string_view text)
    -> Expected<SessionMessage, MessageError> {
  const auto canonical = json::WireDialect::representable_text(text);
  if (!canonical)
    return unexpected(MessageError{MessageErrorKind::json, canonical.error()});
  const auto invalid_schema =
      unexpected(MessageError{MessageErrorKind::invalid_schema, {}});
  const auto root =
      json::WireDialect::read_canonical<MessageWire>(canonical.value().view());
  if (!root) return invalid_schema;
  const auto schema = MessageSchemaCodec::decode_schema(root.value());
  if (!schema) return invalid_schema;

  SessionMessage out{};
  out.action = schema->action;
  out.req_id = schema->req_id;
  out.error = schema->error;
  if (schema->has_conn_request_payload) {
    const auto conn_request =
        json::WireDialect::read_canonical<ConnRequestWire>(
            root.value().conn_request->str);
    if (!conn_request) return invalid_schema;
    auto decoded = ConnRequestCodec::decode(conn_request.value());
    if (!decoded) return invalid_schema;
    out.conn_request.emplace(std::move(decoded.value()));
  }
  return out;
}

}  // namespace libchiaking::net::nat::holepunch
