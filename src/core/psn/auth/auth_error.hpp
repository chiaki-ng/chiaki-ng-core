/// @file
/// @brief Declares AuthError, the failure of a PSN authorisation step: an
/// OAuth token or code request to the authorisation server, or the lease of a
/// supplied app token.
#pragma once

#include <cstdint>
#include <string_view>

namespace libchiaking::psn::auth {

/// @brief Why an authorisation step failed.
enum class AuthErrorKind : std::uint8_t {
  /// @brief The HTTP client's stop token fired before or during the request.
  cancelled,
  /// @brief The request did not complete: connection, TLS, timeout or size
  /// limit.
  transport,
  /// @brief The authorisation server redirected with an `error` parameter,
  /// declining to authorise without an interactive sign-in (every request
  /// asks `prompt=none`), or a service refused the supplied app token.
  refused,
  /// @brief The authorisation server answered with a 5xx status instead of a
  /// redirect.
  server_error,
  /// @brief The server answered with a status other than 302 and not a server
  /// error (for an authorize request), or a non-success status (for
  /// authorizeCheck).
  rejected,
  /// @brief The redirect had no usable `Location`, or the `Location` carried
  /// no readable code or token.
  malformed,
  /// @brief The NPSSO is empty or unusable, so no request was sent.
  invalid_npsso,
  /// @brief The request could not be built: an unsupported service and
  /// platform, a bad DUID or client id, a URL too long to send, a code grant
  /// asked of the token cache, or an unknown token to refresh.
  invalid_request,
  /// @brief No app token has been supplied.
  missing,
  /// @brief The supplied app token expires within app_token_margin.
  expired,
};

/// @brief An authorisation failure: its kind and the HTTP status that came
/// with it, if any. A small immutable value.
class AuthError {
 public:
  /// @brief Makes a failure.
  ///
  /// @param kind Why the step failed.
  /// @param http_status The reply's status, or 0 when there was no reply.
  explicit constexpr AuthError(AuthErrorKind kind,
                               std::uint16_t http_status = 0) noexcept
      : kind_(kind), http_status_(http_status) {}

  /// @brief Reads why the step failed.
  ///
  /// @return The kind.
  constexpr auto kind() const noexcept -> AuthErrorKind { return kind_; }
  /// @brief Reads the status of the reply that failed.
  ///
  /// @return The HTTP status, or 0 when no reply was received.
  constexpr auto http_status() const noexcept -> std::uint16_t {
    return http_status_;
  }

  /// @brief Tells whether the failure means PSN no longer accepts the NPSSO,
  /// so the user must sign in again.
  ///
  /// @return True for refused, rejected and invalid_npsso.
  constexpr auto refuses_npsso() const noexcept -> bool {
    return kind_ == AuthErrorKind::refused ||
           kind_ == AuthErrorKind::rejected ||
           kind_ == AuthErrorKind::invalid_npsso;
  }

  /// @brief Tells whether the authorisation server answered and said no, as
  /// opposed to not being reached.
  ///
  /// @return True when refuses_npsso() is, and for malformed.
  constexpr auto denied() const noexcept -> bool {
    return refuses_npsso() || kind_ == AuthErrorKind::malformed;
  }

  /// @brief Names the kind for logs.
  ///
  /// @return The enumerator's name, such as `server_error`.
  constexpr auto name() const noexcept -> std::string_view {
    switch (kind_) {
      case AuthErrorKind::cancelled:
        return "cancelled";
      case AuthErrorKind::transport:
        return "transport";
      case AuthErrorKind::refused:
        return "refused";
      case AuthErrorKind::server_error:
        return "server_error";
      case AuthErrorKind::rejected:
        return "rejected";
      case AuthErrorKind::malformed:
        return "malformed";
      case AuthErrorKind::invalid_npsso:
        return "invalid_npsso";
      case AuthErrorKind::invalid_request:
        return "invalid_request";
      case AuthErrorKind::missing:
        return "missing";
      case AuthErrorKind::expired:
        return "expired";
    }
    return "transport";
  }

 private:
  /// @brief Why the step failed.
  AuthErrorKind kind_;
  /// @brief The reply's HTTP status; 0 when there was none.
  std::uint16_t http_status_;
};

}  // namespace libchiaking::psn::auth
