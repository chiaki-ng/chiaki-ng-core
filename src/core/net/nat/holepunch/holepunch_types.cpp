#include "src/core/net/nat/holepunch/holepunch_types.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace libchiaking::net::nat::holepunch {

Action MessageSchemaCodec::classify_action(std::string_view action) {
  if (action == "OFFER") return Action::offer;
  if (action == "ACCEPT") return Action::accept;
  if (action == "TERMINATE") return Action::terminate;
  if (action == "RESULT") return Action::result;
  return Action::unknown;
}

auto MessageSchemaCodec::int32(const json::ps::Number& value) noexcept
    -> std::optional<std::int32_t> {
  constexpr auto low = std::numeric_limits<std::int32_t>::min();
  constexpr auto high = std::numeric_limits<std::int32_t>::max();
  constexpr double overflowed = 9.2e18;
  if (const auto* number = std::get_if<std::uint64_t>(&value.value()))
    return *number >= static_cast<std::uint64_t>(high)
               ? high
               : static_cast<std::int32_t>(*number);
  if (const auto* number = std::get_if<std::int64_t>(&value.value()))
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(*number, low, high));
  const auto real = std::get<double>(value.value());
  if (std::fabs(real) >= overflowed) return real > 0 ? high : low;
  return std::nullopt;
}

auto MessageSchemaCodec::decode_schema(const MessageWire& root)
    -> std::optional<MessageSchema> {
  if (!root.action || !root.req_id || !root.error || !root.conn_request ||
      !root.conn_request->str.starts_with('{'))
    return std::nullopt;
  const auto req_id_value = wrapped<std::uint16_t>(root.req_id);
  const auto error_value = wrapped<std::uint16_t>(root.error);
  if (!req_id_value || !error_value) return std::nullopt;
  const std::string_view action_text = *root.action;
  return MessageSchema{
      classify_action(action_text.substr(0, action_text.find('\0'))),
      *req_id_value, *error_value, root.conn_request->str != "{}"};
}

}  // namespace libchiaking::net::nat::holepunch
