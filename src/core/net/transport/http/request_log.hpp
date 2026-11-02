/// @file
/// @brief Declares RequestLog and Scrubber, which write the diagnostics of each
/// HTTP request with the secrets it sent removed.
#pragma once

#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/net/transport/http/failure.hpp"
#include "src/core/net/transport/http/response.hpp"
#include "src/core/net/transport/http/status.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::net::http {

/// @brief Most bytes of a failed response's body shown in its log line.
inline constexpr std::size_t log_excerpt_bytes = 160;

/// @brief Removes the secrets a request sent from the text logged about it.
///
/// The values of the `Authorization`, `Cookie`, `X-Gaikai-Session` and
/// `X-Gaikai-SessionId` headers are split at spaces, `;`, `=` and `,`; every
/// piece of 4 bytes or more is replaced by `***` wherever it appears, raw or
/// percent-encoded. Holds views into the request's headers, which must outlive
/// it. Immutable after construction.
class Scrubber {
 public:
  /// @brief Collects the secret pieces of a request's headers.
  ///
  /// @param sent The headers sent; must outlive the scrubber.
  explicit Scrubber(std::span<const platform::HttpHeader> sent);

  /// @brief The part of a URL that is logged: without scheme or fragment, and
  /// without the query unless asked.
  ///
  /// @param url The URL.
  /// @param with_query Whether to keep the query.
  /// @return A view into `url`.
  static auto log_target(std::string_view url, bool with_query) noexcept
      -> std::string_view;

  /// @brief The logged part of a URL with secrets scrubbed.
  ///
  /// @param url The URL.
  /// @param with_query Whether to keep the query.
  /// @return The scrubbed text.
  auto target(std::string_view url, bool with_query) const -> std::string;
  /// @brief The start of a body, scrubbed, for a log line.
  ///
  /// @param body The body.
  /// @return At most 160 bytes, unprintable bytes shown as `.`.
  auto excerpt(std::string_view body) const -> std::string;

 private:
  /// @brief What a secret is replaced with.
  static constexpr std::string_view scrubbed = "***";
  /// @brief Characters that split a secret header's value.
  static constexpr std::string_view separators = " ;=,";
  /// @brief Shortest piece treated as secret, in bytes.
  static constexpr std::size_t secret_segment_minimum = 4;
  /// @brief Headers whose values are secret, lower case, compared ignoring
  /// case.
  static constexpr std::array secret_header_names{
      std::string_view{"authorization"},
      std::string_view{"cookie"},
      std::string_view{"x-gaikai-session"},
      std::string_view{"x-gaikai-sessionid"},
  };

  /// @brief Whether a header's value is secret.
  ///
  /// @param name The header name.
  /// @return True for the names in secret_header_names, ignoring case.
  static auto secret_header(std::string_view name) noexcept -> bool;
  /// @brief Replaces every occurrence of a piece.
  ///
  /// @param text The text.
  /// @param segment The piece.
  /// @return The text with each occurrence replaced by `***`.
  static auto replaced(std::string_view text, std::string_view segment)
      -> SecretString;
  /// @brief Replaces every occurrence of a piece in place, if there is one.
  ///
  /// @param text The text.
  /// @param segment The piece.
  static void scrub(SecretString& text, std::string_view segment);

  /// @brief The secret pieces, views into the request's headers.
  std::vector<std::string_view> segments_;
};

/// @brief The log lines of one request through Client, from start to outcome.
///
/// Writes `[HTTP] <label> <METHOD> <target>` at debug level when made, an info
/// line for each retry, and on destruction the outcome recorded by finish(), or
/// `abandoned` at warning level if finish() never ran. Outcome lines name the
/// result (`ok`, `client_error`, `server_error`, `cancelled` or
/// `transport_error`), the status, the attempts and the elapsed milliseconds,
/// plus a scrubbed body excerpt for 4xx and 5xx. With no sink it writes
/// nothing. Lives in one send(); not copyable. Logging failures are swallowed.
class RequestLog {
 public:
  /// @brief Logs the start of a request.
  ///
  /// @param sink Where lines go; null for none.
  /// @param clock The monotonic clock the elapsed time is measured on; must
  ///     outlive the log.
  /// @param endpoint The operation; its label must outlive the log.
  /// @param request The request as sent; its headers must outlive the log.
  /// @param credential The credential's label, shown as `auth=`; empty for
  ///     none.
  RequestLog(platform::DiagnosticsProvider* sink,
             const platform::ClockProvider& clock, const Endpoint& endpoint,
             const platform::HttpRequest& request, std::string_view credential);
  /// @brief Not copyable: it writes one outcome.
  RequestLog(const RequestLog&) = delete;
  /// @brief Not copyable: it writes one outcome.
  ///
  /// @return Never returns.
  auto operator=(const RequestLog&) -> RequestLog& = delete;
  /// @brief Writes the outcome line, or `abandoned` when none was recorded.
  ~RequestLog();

  /// @brief Logs a request stopped before its first attempt.
  ///
  /// @param sink Where the line goes; null for none.
  /// @param label The operation's label.
  static void cancelled_before_start(platform::DiagnosticsProvider* sink,
                                     std::string_view label) noexcept;

  /// @brief Logs that an attempt will be retried.
  ///
  /// @param attempt The attempt that failed, from 1.
  /// @param reason Why, such as `HTTP 503`.
  /// @param delay The pause before the retry.
  void retry(int attempt, std::string_view reason,
             std::chrono::milliseconds delay) const noexcept;
  /// @brief Records the outcome to write on destruction.
  ///
  /// Severity is verbose for success, info for 4xx and cancellation, warning
  /// for 5xx and transport failures.
  ///
  /// @param outcome The request's result.
  void finish(const Expected<Response, Failure>& outcome) noexcept;

  /// @brief Logs one page of a paged walk, at verbose level.
  ///
  /// @param sink Where the line goes; null for none.
  /// @param label The operation's label.
  /// @param page The page number, from 1.
  /// @param items Items the page held.
  static void log_page(platform::DiagnosticsProvider* sink,
                       std::string_view label, int page, int items) noexcept;
  /// @brief Logs how a paged walk ended: info for `ok`, warning otherwise.
  ///
  /// @param sink Where the line goes; null for none.
  /// @param label The operation's label.
  /// @param outcome How it ended, such as `ok`, `truncated` or `cancelled`.
  /// @param requests Pages requested.
  /// @param last_status The last page's status; 0 when it got none.
  static void log_paging(platform::DiagnosticsProvider* sink,
                         std::string_view label, std::string_view outcome,
                         int requests, std::uint16_t last_status) noexcept;
  /// @brief Classifies a status for paging logs.
  ///
  /// @param status The status.
  /// @return `ok` for 2xx, `auth_refused` for 401 and 403, `not_found` for
  ///     404, `rejected` for other 4xx, `unavailable` for the rest.
  static auto status_class(HttpStatus status) noexcept -> std::string_view;

 private:
  /// @brief The prefix of every line.
  static constexpr std::string_view log_prefix = "[HTTP] ";

  /// @brief Written in place of a line that could not be built, so the loss
  /// is visible.
  static constexpr std::string_view unwritten_line =
      "[HTTP] a request log line could not be built";

  /// @brief Starts a line.
  ///
  /// @param label The operation's label.
  /// @return `[HTTP] ` followed by the label.
  static auto line(std::string_view label) -> std::string;
  /// @brief Writes a line if there is a sink.
  ///
  /// @param sink Where it goes; null for none.
  /// @param severity Its severity.
  /// @param message The line.
  static void write(platform::DiagnosticsProvider* sink,
                    platform::LogSeverity severity,
                    std::string_view message) noexcept;
  /// @brief Whether a status earns a body excerpt.
  ///
  /// @param status The status.
  /// @return True for 4xx and 5xx.
  static auto failed_status(HttpStatus status) noexcept -> bool;

  /// @brief Where lines go; null for none.
  platform::DiagnosticsProvider* sink_;
  /// @brief The operation's label.
  std::string_view label_;
  /// @brief Scrubs the request's secrets from lines.
  Scrubber scrubber_;
  /// @brief The monotonic clock the elapsed time is measured on.
  const platform::ClockProvider* clock_;
  /// @brief When the request started, in monotonic microseconds.
  std::uint64_t started_us_;
  /// @brief Severity of the outcome line.
  platform::LogSeverity severity_{platform::LogSeverity::warning};
  /// @brief The outcome line; empty until finish().
  std::string outcome_;
};

}  // namespace libchiaking::net::http
