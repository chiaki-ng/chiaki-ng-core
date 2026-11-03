/// @file
/// @brief Declares the header of the hole-punch session messages a client and
/// console exchange through PSN, and its reader.
#pragma once

#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::net::nat::holepunch {

/// @brief The action of a hole-punch session message, sent as upper-case text.
enum class Action {
  /// @brief OFFER: the client's connection request.
  offer,
  /// @brief ACCEPT: the console's answering connection request.
  accept,
  /// @brief TERMINATE: one side abandons the attempt.
  terminate,
  /// @brief RESULT: the outcome of the attempt.
  result,
  /// @brief Any other action text.
  unknown
};

/// @brief Why a session message could not be read.
enum class MessageErrorKind : std::uint8_t {
  /// @brief The text is not JSON the wire dialect accepts.
  json,
  /// @brief The JSON lacks a field, has a wrong type or an invalid
  /// connection request.
  invalid_schema
};

/// @brief Why a session message could not be read, with the JSON error when
/// parsing failed.
struct MessageError {
  /// @brief The kind of failure.
  MessageErrorKind kind{MessageErrorKind::invalid_schema};
  /// @brief The parser's error; meaningful only for MessageErrorKind::json.
  json::ParseError json_error{};
};

/// @brief A session message as parsed: `action`, `reqId`, `error` and the
/// unparsed `connRequest` object, all optional until checked.
struct MessageWire {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief `action`: OFFER, ACCEPT, TERMINATE or RESULT.
  std::optional<std::string> action;
  /// @brief `reqId`, the request number.
  std::optional<json::ps::Number> req_id;
  /// @brief `error`, 0 for none.
  std::optional<json::ps::Number> error;
  /// @brief `connRequest`, kept as raw JSON; `{}` when the message carries
  /// none.
  std::optional<glz::raw_json> conn_request;
};

/// @brief A session message's checked header fields.
struct MessageSchema {
  /// @brief The action.
  Action action{Action::unknown};
  /// @brief The request number, truncated to 16 bits.
  std::uint16_t req_id{};
  /// @brief The error code, truncated to 16 bits.
  std::uint16_t error{};
  /// @brief Whether `connRequest` is a non-empty object.
  bool has_conn_request_payload{};
};

/// @brief Reads the header fields of hole-punch session messages and the
/// lenient
/// integers PS writes in them. Stateless.
class MessageSchemaCodec {
 public:
  /// @brief Names an action.
  ///
  /// @param action The text.
  /// @return The action; Action::unknown for anything else.
  static Action classify_action(std::string_view action);

  /// @brief Reads a JSON number as a 32-bit integer, saturating.
  ///
  /// @param value The number.
  /// @return The integer, clamped to the int32 range; a floating-point value
  ///     of
  ///     magnitude 9.2e18 or more saturates; nothing for any other
  ///     floating-point value.
  [[nodiscard]] static auto int32(const json::ps::Number& value) noexcept
      -> std::optional<std::int32_t>;

  /// @brief Reads an optional JSON number into an unsigned type, wrapping.
  ///
  /// @tparam Number The unsigned type.
  /// @param value The number, if present.
  /// @return The saturated int32 cast to `Number`; nothing when absent or not
  ///     an integer.
  template <std::unsigned_integral Number>
  [[nodiscard]] static auto wrapped(
      const std::optional<json::ps::Number>& value) noexcept
      -> std::optional<Number> {
    if (!value) return std::nullopt;
    return int32(*value).transform(
        [](std::int32_t number) { return static_cast<Number>(number); });
  }

  /// @brief Checks a parsed message's header.
  ///
  /// @param root The parsed message.
  /// @return The header; nothing when a field is missing, `connRequest` is
  ///     not
  ///     an object, or a number is not an integer. Text after a NUL in
  ///     `action` is ignored.
  [[nodiscard]] static auto decode_schema(const MessageWire& root)
      -> std::optional<MessageSchema>;
};

}  // namespace libchiaking::net::nat::holepunch
