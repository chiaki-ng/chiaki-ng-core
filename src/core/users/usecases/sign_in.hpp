/// @file
/// @brief Declares usecases::SignIn, the use case that brings users into the
/// registry: by a fresh NPSSO, by renewing a known user, from the saved store,
/// or as a home user with no PSN sign-in.
#pragma once

#include "libchiaking/detail/users/signed_in_users.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "src/core/users/adapters/user_roster.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace libchiaking::users::usecases {

/// @brief Turns an NPSSO, a saved row or a bare account id into a user held by
/// the UserRoster and saved to the CredentialStore.
///
/// Every path ends the same way: a SavedSignIn or SavedHomeUser is admitted to
/// the roster, which validates it and replaces any user already held for that
/// account, and the new user is saved. The use case holds no state of its own
/// beyond references to the registry's providers and roster, all of which must
/// outlive it; SignedInUsers::Core owns it.
///
/// Members may be called from several threads at once. The roster is locked
/// for each admission, so two concurrent sign-ins for one account both succeed
/// and the one admitted last is the user the registry holds.
class SignIn {
 public:
  /// @brief Why again() did not renew a user.
  using Refusal = SignedInUsers::SignInAgainRefusal;
  /// @brief Cancels the exchange with PSN.
  using Stop = platform::StopToken;

  /// @brief The NPSSO a sign-in starts from, with when it stops being
  /// accepted if the application knows.
  ///
  /// Only borrowed for the length of one call: the NPSSO is cloned into the
  /// saved sign-in, never kept by reference.
  struct Seed {
    /// @brief The NPSSO cookie value from PSN's web sign-in. Must outlive
    /// the call it is passed to.
    const psn::auth::Npsso& npsso;
    /// @brief Wall-clock time at which PSN stops accepting the NPSSO, or
    /// nothing when the application does not know. Recorded with the
    /// sign-in and reported by PsUser::held_credentials.
    std::optional<SavedSignIn::Time> expires_at{};
  };

  /// @brief Builds the use case over a registry's providers and roster.
  ///
  /// @param http Carries the sign-in exchanges. Must outlive the use case.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param credentials The store welcome_back() loads from. Must outlive the
  ///     use case.
  /// @param wall Wall clock that stamps when each token was obtained and
  ///     computes when it expires. Must outlive the use case.
  /// @param roster Where admitted users are held. Must outlive the use case.
  /// @param log Receives diagnostics from the exchanges, or null to log
  ///     nothing. Must outlive the use case when given.
  SignIn(platform::HttpProvider& http, const platform::ClockProvider& clock,
         CredentialStore& credentials, const platform::ClockProvider& wall,
         UserRoster& roster, platform::DiagnosticsProvider* log) noexcept;
  /// @brief Not copyable; one use case serves one registry.
  SignIn(const SignIn&) = delete;
  /// @brief Not copy-assignable; one use case serves one registry.
  auto operator=(const SignIn&) -> SignIn& = delete;

  /// @brief Signs an account in from an NPSSO and holds it as a new user.
  ///
  /// Exchanges the NPSSO with PSN for app and Remote Play tokens and looks the
  /// account up (psn::auth::SignIn::sign_in), then admits and saves the user
  /// as kept() does. A user already held for the same account is replaced and
  /// wiped. A failed save does not fail the sign-in; it shows as
  /// PsUser::is_saved being false.
  ///
  /// Blocks the calling thread for the several requests of the exchange,
  /// each bounded by the HTTP provider's timeouts, and then for the store
  /// write. Safe from any thread.
  ///
  /// @param seed The NPSSO and its known expiry.
  /// @param device The device id to sign in as; cloned into the new user.
  /// @param correlation_id Identifier for the authorisation request, in UUID
  ///     form.
  /// @param stop Cancels the exchange.
  /// @return The newly held user.
  /// @retval psn::auth::SignInError The exchange failed or was cancelled; the
  ///     error says how. Nothing in the registry changes.
  /// @retval psn::auth::SignInErrorKind::malformed PSN's tokens or account
  ///     did not pass the roster's validation.
  [[nodiscard]] auto first_time(Seed seed, const DeviceId& device,
                                std::string_view correlation_id,
                                const Stop& stop)
      -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError>;
  /// @brief Renews a held user's sign-in from a fresh NPSSO.
  ///
  /// Works as first_time() does, but first checks that `user` is the object
  /// the roster holds, and after the exchange checks that the NPSSO belongs
  /// to the same account and that `user` is still held, since it may have
  /// been signed out or replaced while the exchange ran. On success `user` is
  /// wiped and replaced by the returned object.
  ///
  /// Blocks as first_time() does. Safe from any thread.
  ///
  /// @param user The user to renew.
  /// @param seed The fresh NPSSO and its known expiry.
  /// @param device The device id to sign in as.
  /// @param correlation_id Identifier for the authorisation request, in UUID
  ///     form.
  /// @param stop Cancels the exchange.
  /// @return The renewed user, a different object from `user`.
  /// @retval Refusal::Kind::unknown_user `user` was not held before the
  ///     exchange, or stopped being held while it ran. No request is sent in
  ///     the first case.
  /// @retval Refusal::Kind::sign_in_failed The exchange failed, or what PSN
  ///     issued did not pass validation (carrying
  ///     psn::auth::SignInErrorKind::malformed).
  /// @retval Refusal::Kind::different_account The NPSSO belongs to another
  ///     account, which the refusal names. Nothing changes.
  [[nodiscard]] auto again(const PsUser& user, Seed seed,
                           const DeviceId& device,
                           std::string_view correlation_id, const Stop& stop)
      -> Expected<std::shared_ptr<PsUser>, Refusal>;
  /// @brief Loads every saved sign-in and then every saved home user into the
  /// roster.
  ///
  /// A user whose account is already held is returned as the object already
  /// held, not reloaded. A row that fails validation is skipped and counted.
  /// A saved sign-in with no device id is given a freshly generated one and
  /// saved again at once; every other loaded user is marked saved without
  /// writing. Nothing is sent to PSN.
  ///
  /// Blocks on the two store reads and any re-save. Safe from any thread.
  ///
  /// @return The users now known from the store, saved sign-ins first, and
  ///     the number of rows skipped.
  /// @retval CredentialStore::Error The store could not be read. When the
  ///     home users fail after the saved sign-ins were read, the sign-ins
  ///     already admitted stay in the roster although the call fails.
  [[nodiscard]] auto welcome_back()
      -> Expected<SignedInUsers::Returned, CredentialStore::Error>;
  /// @brief Holds a user who has no PSN sign-in, known only by account id and
  /// name, and saves them.
  ///
  /// When the account is already held with a PSN sign-in, that user is
  /// returned and nothing changes. A home user already held for the account
  /// is replaced by a new object and wiped.
  ///
  /// Blocks on the store write. Safe from any thread.
  ///
  /// @param account The PSN account id the user pairs consoles as.
  /// @param name The name to show for the user.
  /// @return The held user.
  /// @retval PsUser::Invalid::account_id The account id is zero.
  /// @retval PsUser::Invalid::online_id The name is empty, longer than
  ///     PsUser::online_id_limit or holds a control character.
  [[nodiscard]] auto home_user(AccountId account, std::string_view name)
      -> Expected<std::shared_ptr<PsUser>, PsUser::Invalid>;

 private:
  /// @brief Records what a sign-in exchange issued as a saved sign-in, admits
  /// it to the roster and saves it.
  ///
  /// Every token is stamped as obtained at the wall clock's current time, and
  /// its expiry is that time plus the lifetime PSN stated. The NPSSO and the
  /// device id are cloned in.
  ///
  /// Blocks on the store write.
  ///
  /// @param issued The account and tokens the exchange produced; consumed.
  /// @param seed The NPSSO the exchange started from and its known expiry.
  /// @param device The device id the exchange signed in as.
  /// @return The newly held user, saved when the store accepted it.
  /// @retval psn::auth::SignInErrorKind::malformed What PSN issued did not
  ///     pass the roster's validation, for example an empty token or an
  ///     online id with a control character.
  [[nodiscard]] auto kept(psn::auth::SignedIn issued, Seed seed,
                          const DeviceId& device)
      -> Expected<std::shared_ptr<PsUser>, psn::auth::SignInError>;

  /// @brief Carries the sign-in exchanges.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The store saved users and home users are loaded from.
  CredentialStore& credentials_;
  /// @brief Wall clock that stamps issued tokens.
  const platform::ClockProvider& wall_;
  /// @brief The registry's set of users, which every path writes into.
  UserRoster& roster_;
  /// @brief Receives diagnostics from the exchanges; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::users::usecases
