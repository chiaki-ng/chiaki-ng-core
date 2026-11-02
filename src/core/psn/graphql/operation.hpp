/// @file
/// @brief Declares Operation, one PSN GraphQL persisted query: its name, the
/// SHA-256 of its query text, the endpoint it is sent to and whether it needs
/// the app token.
///
/// PSN's GraphQL gateway only runs queries it already knows; a client names
/// one by operation name and hash (Apollo persisted queries) and sends its
/// variables in the URL of a GET.
#pragma once

#include <cstdint>
#include <string_view>

namespace libchiaking::psn::graphql {

/// @brief The GraphQL gateway the store web site uses, anonymously.
inline constexpr std::string_view web_endpoint =
    "https://web.np.playstation.com/api/graphql/v1/op";
/// @brief The GraphQL gateway used with the app token.
inline constexpr std::string_view app_endpoint =
    "https://m.np.playstation.com/api/graphql/v1/op";

/// @brief Whether an operation is sent with the app token.
enum class Auth : std::uint8_t {
  /// @brief Anonymous; Sender::send_public() only.
  none,
  /// @brief With the app token; Sender::send_authenticated() only.
  required
};

/// @brief A persisted query. Plain data, defined as constants by the public and
/// authenticated operation lists.
struct Operation {
  /// @brief The `operationName`: letters, digits and `_`.
  std::string_view name;
  /// @brief The query's SHA-256, 64 lowercase hex digits.
  std::string_view sha256;
  /// @brief The gateway: web_endpoint or app_endpoint.
  std::string_view endpoint;
  /// @brief The diagnostic label; the name is used when empty.
  std::string_view label;
  /// @brief Whether the app token is sent.
  Auth auth{Auth::none};
  /// @brief The persisted query protocol version; at least 1.
  int version{1};
};

}  // namespace libchiaking::psn::graphql
