/// @file
/// @brief Declares MessageCodec, the writer and reader of complete hole-punch
/// session messages.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/detail/expected.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/net/nat/holepunch/holepunch_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::net::nat::holepunch {

/// @brief Why a message with a connection request could not be written.
enum class FullMessageError : std::uint8_t {
  /// @brief Only OFFER and ACCEPT carry a connection request.
  invalid_action,
  /// @brief A candidate could not be encoded.
  invalid_candidate,
  /// @brief The connection request or message exceeds its size limit.
  too_large,
};

/// @brief A decoded session message.
struct SessionMessage {
  /// @brief The action.
  Action action{Action::unknown};
  /// @brief The request number.
  std::uint16_t req_id{};
  /// @brief The error code; 0 for none.
  std::uint16_t error{};
  /// @brief The connection request, when the message carries one.
  std::optional<ConnRequest> conn_request;
};

/// @brief Writes and reads hole-punch session messages: JSON objects
/// `{"action":…,"reqId":…,"error":…,"connRequest":…}`. Stateless.
class MessageCodec {
 public:
  /// @brief Writes a message without a connection request (`connRequest` is
  /// `{}`).
  ///
  /// @param action The action.
  /// @param req_id The request number.
  /// @param error The error code.
  /// @return The JSON text; empty when writing failed.
  static auto format_short(Action action, std::uint16_t req_id,
                           std::uint16_t error) -> std::string;

  /// @brief Largest message with a connection request: 32 KiB.
  static constexpr std::size_t max_full_message_bytes = 32U * 1024U;

  /// @brief Writes an OFFER or ACCEPT with a connection request.
  ///
  /// @param action OFFER or ACCEPT.
  /// @param req_id The request number.
  /// @param error The error code.
  /// @param request The connection request.
  /// @param account_id This user's PSN account id, written as the local peer.
  /// @return The JSON text, secret because it carries the session key.
  /// @retval FullMessageError::invalid_action The action is neither.
  /// @retval FullMessageError::invalid_candidate A candidate is invalid.
  /// @retval FullMessageError::too_large A size limit was exceeded.
  [[nodiscard]] static auto format_full(Action action, std::uint16_t req_id,
                                        std::uint16_t error,
                                        const ConnRequest& request,
                                        std::int64_t account_id)
      -> Expected<SecretString, FullMessageError>;

  /// @brief Reads a session message.
  ///
  /// @param text The JSON text.
  /// @return The message.
  /// @retval MessageErrorKind::json The text is not acceptable JSON.
  /// @retval MessageErrorKind::invalid_schema A field or the connection
  ///     request
  ///     is invalid.
  [[nodiscard]] static auto decode_session(std::string_view text)
      -> Expected<SessionMessage, MessageError>;

 private:
  /// @brief The empty `connRequest` of a short message.
  struct Unrequested {};

  /// @brief The message object as written.
  ///
  /// @tparam ConnRequest The type written as `connRequest`.
  template <class ConnRequest>
  struct Envelope {
    /// @brief Members map to camelCase JSON keys.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The action text.
    std::string_view action;
    /// @brief The request number.
    std::uint16_t req_id{};
    /// @brief The error code.
    std::uint16_t error{};
    /// @brief The connection request.
    ConnRequest conn_request;
  };

  /// @brief The wire text of an action.
  ///
  /// @param action The action.
  /// @return OFFER, ACCEPT, TERMINATE, RESULT or UNKNOWN.
  static auto action_name(Action action) noexcept -> std::string_view;
};

}  // namespace libchiaking::net::nat::holepunch
