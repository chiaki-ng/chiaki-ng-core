/// @file
/// @brief Declares Endpoint and Retry, the description of a remote HTTP
/// operation and its retry policy, with the defaults every request starts from.
#pragma once

#include <libchiaking/platform/http/provider.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace libchiaking::net::http {

/// @brief The HTTP method a request uses.
using Method = platform::HttpMethod;

/// @brief Largest response body accepted unless an endpoint says otherwise: 16
/// MiB.
inline constexpr std::size_t default_maximum_response_bytes =
    16U * 1024U * 1024U;
/// @brief Default limit on establishing the connection: 10 seconds.
inline constexpr std::chrono::milliseconds default_connect_timeout{
    std::chrono::seconds{10}};
/// @brief Default limit on a whole attempt, connection to last byte: 30
/// seconds.
inline constexpr std::chrono::milliseconds default_total_timeout{
    std::chrono::seconds{30}};
/// @brief Most retries any endpoint may ask for; larger values are clamped.
inline constexpr int max_retries = 8;

/// @brief When a request is sent again: how many times, after what pause, and
/// for which failures.
struct Retry {
  /// @brief Extra attempts after the first, clamped to 0 through max_retries;
  /// 0 sends once.
  int retries{};
  /// @brief Pause before each retry; 0 retries at once. The pause is cut
  /// short by the client's stop token.
  std::chrono::milliseconds delay{};
  /// @brief Whether a 5xx status is retried.
  bool on_server_error{true};
  /// @brief Whether a 429 status is retried.
  bool on_too_many_requests{true};
  /// @brief Whether a failure to get any response, other than cancellation,
  /// is retried.
  bool on_transport_error{true};
};

/// @brief A remote operation the library calls: its log label, method,
/// retry policy, timeouts and size limit.
///
/// Usually a constant next to the client that calls it; requests copy it. The
/// label must outlive every request built from it, so it is normally a string
/// literal.
struct Endpoint {
  /// @brief Short name of the operation in logs and diagnostics, such as
  /// `psn.token`.
  std::string_view label;
  /// @brief The HTTP method.
  Method method{Method::get};
  /// @brief When to retry; by default never.
  Retry retry{};
  /// @brief Limit on establishing the connection, per attempt; at least 1 ms
  /// and at most the total timeout.
  std::chrono::milliseconds connect_timeout{default_connect_timeout};
  /// @brief Limit on a whole attempt; at least 1 ms.
  std::chrono::milliseconds total_timeout{default_total_timeout};
  /// @brief Whether the query string appears in logs; off by default because
  /// queries often carry secrets.
  bool log_query{};
  /// @brief Whether a response that arrived after the stop token fired is
  /// still returned; by default it is discarded as cancelled.
  bool keep_reply_after_stop{};
  /// @brief Statuses outside 2xx that are ordinary answers of the operation
  /// rather than errors, so that diagnostics neither flag nor count them. The
  /// codes must outlive every request built from the endpoint; by default
  /// none.
  std::span<const std::uint16_t> expected_statuses{};
  /// @brief Largest response body accepted, in bytes; the client may lower it
  /// further.
  std::size_t maximum_response_bytes{default_maximum_response_bytes};
};

}  // namespace libchiaking::net::http
