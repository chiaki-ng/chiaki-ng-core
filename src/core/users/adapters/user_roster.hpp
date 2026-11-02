/// @file
/// @brief Declares UserRoster, the set of users one registry currently holds,
/// with at most one user per account.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/credential_store.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"

#include <memory>
#include <vector>

namespace libchiaking::users {

/// @brief The users a SignedInUsers registry holds, keyed by account.
///
/// The roster is the only place users are created: admit() validates a saved
/// row and builds the PsUser from the registry's providers. It keeps the
/// invariant that no two held users share an account; admitting a second user
/// for an account replaces the first, which is wiped (PsUser::Core::leave) and
/// reports SignInState::signed_out from then on. Users are handed out as
/// shared pointers, so a replaced or forgotten user stays alive for whoever
/// still holds it, just signed out.
///
/// Owned by SignedInUsers::Core. Every member locks the roster's mutex and is
/// safe to call from several threads; none of them talks to PSN. Destroying the
/// roster wipes every user still held.
class UserRoster {
 public:
  /// @brief A newly held user, or why the saved row was not valid.
  using Admitted = Expected<std::shared_ptr<PsUser>, PsUser::Invalid>;

  /// @brief Builds an empty roster.
  ///
  /// @param providers The registry's providers, copied by reference into
  ///     every user admitted; their threading provider also makes the
  ///     roster's mutex. What they refer to must outlive the roster and
  ///     every user it hands out.
  explicit UserRoster(const PsUser::Core::Providers& providers);
  /// @brief Not copyable; the roster owns its users.
  UserRoster(const UserRoster&) = delete;
  /// @brief Not copy-assignable; the roster owns its users.
  auto operator=(const UserRoster&) -> UserRoster& = delete;
  /// @brief Wipes the secrets of every user still held. Nothing is removed
  /// from the store.
  ~UserRoster();

  /// @brief Validates a saved PSN sign-in and holds it as a user.
  ///
  /// A saved sign-in with no device id is given a freshly generated one. Any
  /// user already held for the same account is wiped and replaced. Nothing is
  /// saved; the caller decides whether the row needs writing back.
  ///
  /// Never blocks on the network or the store; takes the roster's lock
  /// briefly.
  ///
  /// @param saved The row to admit; consumed.
  /// @return The newly held user.
  /// @retval PsUser::Invalid The row failed validation; see
  ///     PsUser::Core::admit. The roster is unchanged.
  [[nodiscard]] auto admit(SavedSignIn saved) -> Admitted;
  /// @brief Validates a saved home user and holds it as a user.
  ///
  /// Any user already held for the same account, home user or signed in, is
  /// wiped and replaced. Nothing is saved.
  ///
  /// Never blocks on the network or the store; takes the roster's lock
  /// briefly.
  ///
  /// @param saved The row to admit; consumed.
  /// @return The newly held user.
  /// @retval PsUser::Invalid::account_id The account id is zero.
  /// @retval PsUser::Invalid::online_id The name is empty, too long or holds
  ///     a control character.
  [[nodiscard]] auto admit(SavedHomeUser saved) -> Admitted;
  /// @brief Stops holding an account's user, wipes it, and deletes its saved
  /// row.
  ///
  /// The row is deleted even when no user for the account is held.
  ///
  /// Blocks on the store's deletion. Safe from any thread.
  ///
  /// @param account The account to forget.
  /// @return Nothing on success.
  /// @retval CredentialStore::Error The store refused the deletion. The user
  ///     has left the roster and been wiped all the same.
  [[nodiscard]] auto forget(AccountId account)
      -> Expected<void, CredentialStore::Error>;

  /// @brief Looks up the user held for an account.
  ///
  /// @param account The account's number.
  /// @return The held user, or null when none is held for that account.
  auto find(AccountId account) const -> std::shared_ptr<PsUser>;
  /// @brief Copies the list of held users.
  ///
  /// @return Every held user in the order they were first admitted, a
  ///     replaced user taking its predecessor's place; empty when none is
  ///     held. Later changes do not affect the copy.
  auto everyone() const -> std::vector<std::shared_ptr<PsUser>>;
  /// @brief Tells whether this exact object is the one held for its account.
  ///
  /// @param user Any user, held or not.
  /// @return True only when the roster holds `user` itself; false for a user
  ///     that was replaced, forgotten or came from another registry.
  auto holds(const PsUser& user) const -> bool;

 private:
  /// @brief Holds a validated user, replacing and wiping any user already
  /// held for its account.
  ///
  /// @param admitted The outcome of PsUser::Core::admit.
  /// @return `admitted` unchanged; a failure is passed through without
  ///     touching the roster.
  auto enrolled(Admitted admitted) -> Admitted;

  /// @brief The registry's providers that each admitted user is built from.
  PsUser::Core::Providers providers_;
  /// @brief Guards users_.
  mutable platform::Mutex mutex_;
  /// @brief The held users, at most one per account, in admission order.
  std::vector<std::shared_ptr<PsUser>> users_;
};

}  // namespace libchiaking::users
