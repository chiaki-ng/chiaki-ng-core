/// @file
/// @brief Declares Failure, the reasons an HTTP request through Client gets no
/// response.
#pragma once

#include <libchiaking/platform/http/provider.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::net::http {

/// @brief Names a platform HTTP error for logs.
///
/// @param error The error.
/// @return Its snake_case name, such as `timed_out`; `backend_failure` for an
///     unknown value.
auto error_name(platform::HttpError error) noexcept -> std::string_view;

/// @brief Why an HTTP request produced no response, in the library's terms.
///
/// Carries the kind, the platform error behind it when there was one, and how
/// many attempts were made. Any HTTP status is a Response, not a Failure. Only
/// made by the named constructors. A value type, safe to copy and share
/// between threads.
class Failure {
 public:
  /// @brief What kind of failure it is.
  enum class Kind : std::uint8_t {
    /// @brief The stop token fired before or during the request, or the
    /// platform reported cancellation.
    cancelled,
    /// @brief No response arrived: timeout, TLS trust, proxy, disconnection
    /// or a backend failure.
    transport,
    /// @brief The response body exceeded the endpoint's size limit.
    too_large,
    /// @brief The request could not be sent as built, such as an invalid
    /// URL.
    invalid_request,
  };

  /// @brief A cancellation.
  ///
  /// @param attempts Attempts made before it; 0 when none started.
  /// @param error The platform error reported, if the cancellation came from
  ///     the platform.
  /// @return The failure.
  static auto cancelled(int attempts = 0,
                        std::optional<platform::HttpError> error =
                            std::nullopt) noexcept -> Failure;
  /// @brief Classifies a platform error.
  ///
  /// @param error The error.
  /// @param attempts Attempts made.
  /// @return A cancellation for `cancelled`, too_large for
  ///     `response_too_large`, invalid_request for `invalid_url`, transport
  ///     for
  ///     the rest.
  static auto transport(platform::HttpError error, int attempts = 1) noexcept
      -> Failure;
  /// @brief A request rejected before any attempt.
  ///
  /// @return The failure, with no platform error and 0 attempts.
  static auto invalid_request() noexcept -> Failure;

  /// @brief What kind of failure it is.
  ///
  /// @return The kind.
  auto kind() const noexcept -> Kind { return kind_; }
  /// @brief The platform error behind the failure.
  ///
  /// @return The error; nothing when the library decided on its own.
  auto error() const noexcept -> std::optional<platform::HttpError> {
    return error_;
  }
  /// @brief How many attempts were made.
  ///
  /// @return From 0 to max_retries plus one.
  auto attempts() const noexcept -> int { return attempts_; }
  /// @brief Whether this is a cancellation.
  ///
  /// @return True for Kind::cancelled.
  auto is_cancelled() const noexcept -> bool {
    return kind_ == Kind::cancelled;
  }
  /// @brief Names the failure for logs.
  ///
  /// @return `cancelled`, `too_large` or `invalid_request` by kind; for a
  ///     transport failure the platform error's name, or `failure` without
  ///     one.
  auto name() const noexcept -> std::string_view;

 private:
  /// @brief Holds a failure.
  ///
  /// @param kind The kind.
  /// @param error The platform error, if any.
  /// @param attempts Attempts made.
  Failure(Kind kind, std::optional<platform::HttpError> error,
          int attempts) noexcept;

  /// @brief The kind.
  Kind kind_;
  /// @brief The platform error, if any.
  std::optional<platform::HttpError> error_;
  /// @brief Attempts made.
  int attempts_;
};

}  // namespace libchiaking::net::http
