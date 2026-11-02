/// @file
/// @brief Declares TracedHttpProvider, the HTTP provider wrapper that writes a
/// start and a result line to the diagnostics log for every request, and
/// feeds the optional API capture.
#pragma once

#include "libchiaking/detail/diagnostics/api_capture.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace libchiaking::diagnostics {

/// @brief Logs each HTTP request by its ApiName, method and timeout, then its
/// outcome, status, transport error, body size and duration.
///
/// Logs no URL, header or body, so no secret reaches the log. A result is
/// logged as a warning when the transport failed or the status is 4xx or
/// 5xx, otherwise as info. The wrapped provider, the log, the clock and the
/// capture must outlive the wrapper. Safe to call from several threads when the
/// wrapped provider, log and capture are.
class TracedHttpProvider final : public platform::HttpProvider {
 public:
  /// @brief The longest log line written, in bytes; longer lines are cut.
  static constexpr std::size_t message_limit = 383;

  /// @brief Wraps an HTTP provider.
  ///
  /// @param provider The provider that performs the requests.
  /// @param log Where the lines are written.
  /// @param clock The monotonic clock each request's duration is measured
  ///     on.
  /// @param capture Records each exchange in full, or null for none.
  /// @param attempt Identifies the library instance making the calls, as
  ///     LogQueue::attempt gives it; passed to the capture.
  TracedHttpProvider(platform::HttpProvider& provider,
                     platform::DiagnosticsProvider& log,
                     const platform::ClockProvider& clock,
                     ApiCapture* capture = nullptr,
                     std::uint64_t attempt = 0) noexcept;

  /// @brief Performs a request, logging its start and its result.
  ///
  /// Blocks for as long as the wrapped provider does.
  ///
  /// @param request The request.
  /// @param stop Cancels the request; passed to the wrapped provider.
  /// @return The wrapped provider's response or error, unchanged.
  auto perform(const platform::HttpRequest& request,
               const platform::StopToken& stop)
      -> Expected<platform::HttpResponse, platform::HttpError> override;

  /// @brief Starts a request through the wrapped provider, logging its start
  /// now and its result when it is collected.
  ///
  /// A request dropped before it is collected is logged, and captured, as
  /// cancelled.
  ///
  /// @param request The request.
  /// @param stop Cancels the request; passed to the wrapped provider.
  /// @return The wrapped provider's pending response, which logs as it is
  ///     collected.
  auto start(const platform::HttpRequest& request,
             const platform::StopToken& stop)
      -> platform::PendingHttpResponse override;

 private:
  /// @brief What a request's start line recorded, kept for its result line.
  /// Plain data.
  struct Started {
    /// @brief When the request started, in the clock's microseconds.
    std::uint64_t begin;
    /// @brief The method's name, in static storage.
    const char* method;
    /// @brief The request's ApiName, in static storage.
    const char* api;
    /// @brief The capture's call identifier, or 0 without a capture.
    std::uint64_t call;
  };

  /// @brief A started request that writes its result line when collected.
  class Traced;

  /// @brief Writes a request's start line and opens its capture.
  ///
  /// @param request The request.
  /// @return What the result line needs.
  auto started(const platform::HttpRequest& request) noexcept -> Started;
  /// @brief Closes a request's capture and writes its result line.
  ///
  /// @param from What the start line recorded.
  /// @param result The wrapped provider's answer.
  void finished(const Started& from,
                const Expected<platform::HttpResponse, platform::HttpError>&
                    result) noexcept;

  /// @brief A log line's buffer, with room for the terminator.
  using Message = std::array<char, message_limit + 1>;

  /// @brief Writes a formatted line to the log, cut to message_limit.
  ///
  /// @param log Where the line goes.
  /// @param severity The line's severity.
  /// @param message The formatted line.
  /// @param size What `snprintf` returned; nothing is written when it is
  ///     zero or negative.
  static void write(platform::DiagnosticsProvider& log,
                    platform::LogSeverity severity, const Message& message,
                    int size) noexcept;

  /// @brief The provider that performs the requests.
  platform::HttpProvider& provider_;
  /// @brief Where the lines are written.
  platform::DiagnosticsProvider& log_;
  /// @brief The monotonic clock durations are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The full-exchange capture, or null.
  ApiCapture* capture_;
  /// @brief The library instance's identifier, passed to the capture.
  std::uint64_t attempt_;
};

}  // namespace libchiaking::diagnostics
