/// @file
/// @brief Declares AuthorisationRefusal, why PSN's authorisation server would
/// not let a user's device stream from the cloud.
#pragma once

#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/auth/auth_error.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::users {

/// @brief Why a cloud streaming authorisation step on a user's behalf failed:
/// the user had no PSN sign-in, or PSN's authorisation server answered with an
/// error.
///
/// Returned by PlayCalls::cloud_device, cloud_device_check and
/// cloud_streaming_codes, and shared among concurrent callers through
/// InFlight::cloud_device_check. A refusal that means the sign-in itself is no
/// longer accepted (expires_sign_in()) has already marked the user's sign-in as
/// refused by the time the caller sees it, so the user reports
/// SignInState::expired.
///
/// A small value type; copy it freely and read it from any thread.
class AuthorisationRefusal {
 public:
  /// @brief Wraps what the authorisation server answered.
  ///
  /// @param error The failure, including the HTTP status when there was a
  ///     reply.
  explicit AuthorisationRefusal(psn::auth::AuthError error) noexcept
      : error_(error) {}

  /// @brief Makes the refusal for a user with no PSN sign-in, given before
  /// any request is sent.
  ///
  /// @return A refusal for which is_signed_out() is true.
  [[nodiscard]] static auto signed_out() noexcept -> AuthorisationRefusal {
    return AuthorisationRefusal{};
  }
  /// @brief Makes the refusal reported when the caller's stop token fired,
  /// which concurrency::SharedFlight hands to a waiter that gives up.
  ///
  /// @return A refusal for which is_cancelled() is true.
  [[nodiscard]] static auto cancelled() noexcept -> AuthorisationRefusal {
    return AuthorisationRefusal{
        psn::auth::AuthError{psn::auth::AuthErrorKind::cancelled}};
  }

  /// @brief Tells whether an authorisation failure means PSN no longer
  /// accepts the user's NPSSO.
  ///
  /// @param error The failure to judge.
  /// @return True when PSN refused the request or the NPSSO outright, or
  ///     rejected it with a status other than a 5xx server error; false for
  ///     transport failures, server errors, cancellation and the rest, which
  ///     may pass if tried again.
  static auto expires_sign_in(const psn::auth::AuthError& error) noexcept
      -> bool {
    switch (error.kind()) {
      case psn::auth::AuthErrorKind::refused:
      case psn::auth::AuthErrorKind::invalid_npsso:
        return true;
      case psn::auth::AuthErrorKind::rejected:
        return !net::HttpStatus{error.http_status()}.is_server_error();
      default:
        break;
    }
    return false;
  }

  /// @brief Whether nothing was asked because the user has no PSN sign-in.
  ///
  /// @return True for a refusal made by signed_out().
  auto is_signed_out() const noexcept -> bool { return !error_.has_value(); }
  /// @brief Whether the step was abandoned because the caller's stop token
  /// fired.
  ///
  /// @return True when the failure is psn::auth::AuthErrorKind::cancelled.
  auto is_cancelled() const noexcept -> bool {
    return error_ && error_->kind() == psn::auth::AuthErrorKind::cancelled;
  }
  /// @brief Whether this refusal means the user must sign in again.
  ///
  /// @return True when expires_sign_in(const psn::auth::AuthError&) holds for
  ///     the failure; false for a signed-out refusal.
  auto expires_sign_in() const noexcept -> bool {
    return error_ && expires_sign_in(*error_);
  }
  /// @brief The authorisation server's failure.
  ///
  /// @return The failure, or nothing for a signed-out refusal. The reference
  ///     lives as long as this refusal.
  auto authorisation() const noexcept
      -> const std::optional<psn::auth::AuthError>& {
    return error_;
  }
  /// @brief The HTTP status of the reply behind the refusal.
  ///
  /// @return The status, or 0 when there was no reply or the user was signed
  ///     out.
  auto http_status() const noexcept -> std::uint16_t {
    return error_ ? error_->http_status() : std::uint16_t{};
  }

 private:
  /// @brief Builds the signed-out refusal, which carries no failure.
  AuthorisationRefusal() noexcept = default;

  /// @brief What the authorisation server answered; empty when the user was
  /// signed out and nothing was asked.
  std::optional<psn::auth::AuthError> error_;
};

}  // namespace libchiaking::users
