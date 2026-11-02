/// @file
/// @brief Declares cache::HeldAccess, the cloud access last shown to each
/// account and the sign-in it was shown under.
#pragma once

#include <libchiaking/detail/catalog/cloud_access.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

namespace libchiaking::catalog::cache {

/// @brief Every account's last shown cloud access.
///
/// Not synchronised: its owner holds one lock around every call, so that a
/// change here and a change to the owner's other parts happen together.
class HeldAccess {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief The access last shown, while still fresh and for the same
  /// sign-in.
  ///
  /// @param account The account.
  /// @param signed_in_as The sign-in state now.
  /// @param now The wall-clock time now.
  /// @return The access, or std::nullopt when none is held, the sign-in state
  ///     changed, it is for another account, or it is no longer fresh.
  auto fresh(AccountId account, users::SignInState signed_in_as, Time now) const
      -> std::optional<CloudAccess>;
  /// @brief Holds the access shown under a sign-in state.
  ///
  /// @param account The account.
  /// @param signed_in_as The sign-in state it was shown under.
  /// @param access The access.
  void note(AccountId account, users::SignInState signed_in_as,
            const CloudAccess& access);
  /// @brief Forgets the access held for an account.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief Cloud access shown and the sign-in it was shown under.
  struct Shown {
    /// @brief The access shown.
    CloudAccess access;
    /// @brief The sign-in state at the time; a change makes it stale.
    users::SignInState signed_in_as{};
  };

  /// @brief The access shown, by account id.
  std::map<std::uint64_t, Shown> shown_;
};

}  // namespace libchiaking::catalog::cache
