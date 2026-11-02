/// @file
/// @brief Declares RemotePlayAccess, the Remote Play token PlayCalls lends out
/// for one internet start.
#pragma once

#include "src/core/psn/auth/authenticator.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

namespace libchiaking::users {

/// @brief A copy of a user's Remote Play token, lent for starting a session
/// with a console over the internet.
///
/// Only PlayCalls::remote_play_access makes one, after renewing the
/// token if it was within PsUser::remote_play_margin of expiry. The copy is
/// the caller's own; the user keeps the original. When PS answers a request
/// made with it as unauthorised, the caller reports that through
/// refused_by_service() so the user stops trusting the token and the next
/// PlayCalls::remote_play_access renews it.
///
/// Move-only. It refers to the user's core without owning it, so it must not
/// outlive the PsUser it came from. One object should be used from one thread
/// at a time; refused_by_service() itself is safe against the user's other
/// threads.
class RemotePlayAccess {
 public:
  /// @brief Takes over the token of another lent access.
  RemotePlayAccess(RemotePlayAccess&&) noexcept = default;
  /// @brief Not assignable; each lent access stays bound to its user.
  auto operator=(RemotePlayAccess&&) -> RemotePlayAccess& = delete;

  /// @brief The lent token, to send as the bearer of Remote Play requests.
  ///
  /// @return The token, which lives as long as this object. Its secret is
  ///     wiped when this object is destroyed.
  auto bearer() const noexcept -> const psn::auth::BearerToken& {
    return token_;
  }
  /// @brief Tells the user that PS refused this token.
  ///
  /// The user drops its Remote Play token if it is still this one, so that
  /// the next request renews it from the refresh token. A token the user has
  /// already replaced is left alone. Never blocks on the network; takes the
  /// user's lock briefly.
  void refused_by_service();

 private:
  friend class PlayCalls;

  /// @brief Lends a token on behalf of a user.
  ///
  /// @param user The user the token belongs to. Must outlive this object.
  /// @param token The copy of the user's Remote Play token to lend.
  RemotePlayAccess(PsUser::Core& user, psn::auth::BearerToken token) noexcept;

  /// @brief The user the token was lent from; never null.
  PsUser::Core* user_;
  /// @brief The lent copy of the Remote Play token.
  psn::auth::BearerToken token_;
};

}  // namespace libchiaking::users
