/// @file
/// @brief Declares InMemoryCredentialStore, a credential store kept in memory.
#pragma once

#include "libchiaking/detail/users/credential_store.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for users::CredentialStore without a keychain or database.
///
/// Saved sign-ins and home users are kept in memory for the life of the object.
/// Seed it by calling save() directly before handing it to the library, run the
/// code under test, then read back saved(), saved_home_user() and the counters.
/// Setting unavailable makes every users::CredentialStore operation fail, which
/// plays a locked or missing secure store.
///
/// An account is held either as a signed-in user or as a home user, never both:
/// saving one form drops the other for the same account.
///
/// @note The store's own functions lock internally. The public data members are
///     plain fields; set them before the code under test runs and read them
///     after it has stopped. No call blocks beyond taking that lock.
/// @ingroup api_testing
class InMemoryCredentialStore final : public users::CredentialStore {
 public:
  /// @brief Creates an empty store.
  ///
  /// @param threading Makes the lock that guards the store; must outlive it.
  explicit InMemoryCredentialStore(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Returns every saved sign-in.
  ///
  /// @return Independent copies of the saved sign-ins, in the order they were
  ///     last saved.
  /// @retval Error::unavailable unavailable is set.
  [[nodiscard]] auto load_saved_users()
      -> Expected<std::vector<users::SavedSignIn>, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    std::vector<users::SavedSignIn> loaded;
    for (const auto& user : saved_) loaded.push_back(user.clone());
    return loaded;
  }

  /// @brief Returns every saved home user.
  ///
  /// @return Copies of the saved home users, in the order they were last
  ///     saved.
  /// @retval Error::unavailable unavailable is set.
  [[nodiscard]] auto load_home_users()
      -> Expected<std::vector<users::SavedHomeUser>, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    return home_;
  }

  /// @brief Saves a sign-in, replacing whatever was held for its account.
  ///
  /// Any earlier sign-in or home user for the same account is dropped first.
  ///
  /// @param user The sign-in to keep; a copy is stored.
  /// @return Nothing on success, after adding one to saves.
  /// @retval Error::unavailable unavailable is set. Nothing changes.
  [[nodiscard]] auto save(const users::SavedSignIn& user)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    ++saves;
    forget(user.account);
    saved_.push_back(user.clone());
    return {};
  }

  /// @brief Saves a home user, replacing whatever was held for its account.
  ///
  /// Any earlier sign-in or home user for the same account is dropped first.
  ///
  /// @param user The home user to keep; a copy is stored.
  /// @return Nothing on success, after adding one to saves.
  /// @retval Error::unavailable unavailable is set. Nothing changes.
  [[nodiscard]] auto save(const users::SavedHomeUser& user)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    ++saves;
    forget(user.account);
    home_.push_back(user);
    return {};
  }

  /// @brief Forgets an account in whichever form it is held.
  ///
  /// @param account The account to forget.
  /// @return Nothing on success, after adding one to removals. Removing an
  ///     account that was never saved also succeeds and is also counted.
  /// @retval Error::unavailable unavailable is set. Nothing changes.
  [[nodiscard]] auto remove(AccountId account)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    ++removals;
    forget(account);
    return {};
  }

  /// @brief Reads back the sign-in held for an account, for assertions.
  ///
  /// @param account The account to look up.
  /// @return A copy of the saved sign-in, or nothing when the account is not
  ///     held as a signed-in user. Not affected by unavailable.
  [[nodiscard]] auto saved(AccountId account)
      -> std::optional<users::SavedSignIn> {
    const platform::Lock lock{mutex_};
    const auto found =
        std::ranges::find(saved_, account, &users::SavedSignIn::account);
    if (found == saved_.end()) return std::nullopt;
    return found->clone();
  }

  /// @brief Reads back the home user held for an account, for assertions.
  ///
  /// @param account The account to look up.
  /// @return A copy of the saved home user, or nothing when the account is
  ///     not held as a home user. Not affected by unavailable.
  [[nodiscard]] auto saved_home_user(AccountId account)
      -> std::optional<users::SavedHomeUser> {
    const platform::Lock lock{mutex_};
    const auto found =
        std::ranges::find(home_, account, &users::SavedHomeUser::account);
    if (found == home_.end()) return std::nullopt;
    return *found;
  }

  /// @brief When true, every users::CredentialStore operation fails with
  /// Error::unavailable and changes nothing.
  bool unavailable{};
  /// @brief Number of successful save() calls, sign-ins and home users
  /// together.
  std::size_t saves{};
  /// @brief Number of successful remove() calls.
  std::size_t removals{};

 private:
  void forget(AccountId account) {
    std::erase_if(saved_, [account](const users::SavedSignIn& held) {
      return held.account == account;
    });
    std::erase_if(home_, [account](const users::SavedHomeUser& held) {
      return held.account == account;
    });
  }

  platform::Mutex mutex_;
  std::vector<users::SavedSignIn> saved_;
  std::vector<users::SavedHomeUser> home_;
};

}  // namespace libchiaking::testing
