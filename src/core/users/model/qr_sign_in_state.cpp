#include "libchiaking/detail/users/qr_sign_in.hpp"

#include <utility>

namespace libchiaking::users {

QrSignInState::QrSignInState(
    std::uint32_t attempt, QrSignInStep step, SecretString approval_url,
    SecretString user_code,
    std::optional<std::chrono::system_clock::time_point> expires_at,
    std::optional<SignInErrorKind> failure)
    : attempt_(attempt),
      step_(step),
      approval_url_(std::move(approval_url)),
      user_code_(std::move(user_code)),
      expires_at_(expires_at),
      failure_(failure) {}

auto QrSignInState::clone() const -> QrSignInState {
  return QrSignInState{attempt_,           step_,       approval_url_.clone(),
                       user_code_.clone(), expires_at_, failure_};
}

}  // namespace libchiaking::users
