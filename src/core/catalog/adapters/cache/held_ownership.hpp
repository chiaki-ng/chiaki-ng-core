/// @file
/// @brief Declares cache::HeldOwnership, what each account is known to own in
/// memory: its fetched library and PS5 scan, and the titles rechecked while
/// the library could not be read.
#pragma once

#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/catalog/model/library/ps5_entitlement.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/owned_title.hpp>
#include <libchiaking/detail/users/account_id.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace libchiaking::catalog::cache {

/// @brief Every account's ownership held in memory.
///
/// Not synchronised: its owner holds one lock around every call, so that a
/// change here and a change to the owner's other parts happen together.
class HeldOwnership {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  /// @brief A held library.
  using LibraryPtr = std::shared_ptr<const Library>;
  /// @brief A held PS5 entitlement scan.
  using Ps5Ptr = std::shared_ptr<const OwnedPs5Games>;

  /// @brief What an account owns, as far as is held.
  struct Owned {
    /// @brief The library; never null.
    LibraryPtr titles;
    /// @brief LibraryState::fetched when the whole library was read,
    /// LibraryState::unavailable when only rechecked titles are known.
    LibraryState state{LibraryState::fetched};
    /// @brief The PS5 scan; null when none is held.
    Ps5Ptr ps5{};
  };

  /// @brief Whether a library is within its lifetime.
  ///
  /// @param library The library.
  /// @param now The wall-clock time now.
  /// @return True when it was fetched no more than 24 hours before now, and
  ///     not after it.
  static auto is_fresh(const Library& library, Time now) -> bool;

  /// @brief The account's fetched library, when it is within its lifetime.
  ///
  /// @param account The account.
  /// @param now The wall-clock time now.
  /// @return The library, or nullptr when none is held or it is too old.
  auto fresh(AccountId account, Time now) const -> LibraryPtr;
  /// @brief The fetched library's PS5 scan.
  ///
  /// @param account The account.
  /// @return The scan, or nullptr when none is held.
  auto ps5_of(AccountId account) const -> Ps5Ptr;
  /// @brief Holds a library read back from disk with its PS5 scan.
  ///
  /// Anything learned while the library was unavailable is left as it is.
  ///
  /// @param account The account.
  /// @param library The library.
  /// @param ps5 Its PS5 scan; may be null.
  void hold_stored(AccountId account, LibraryPtr library, Ps5Ptr ps5);
  /// @brief Holds a freshly read library and its PS5 scan.
  ///
  /// Clears anything learned while the library was unavailable.
  ///
  /// @param owned The library; must not be null.
  /// @param ps5 Its PS5 scan; may be null.
  void hold_read(const LibraryPtr& owned, Ps5Ptr ps5);
  /// @brief Adds or replaces one LegacyCloudStream title after a recheck.
  ///
  /// When no library is held the title goes into a library of rechecked titles
  /// only, marked unavailable.
  ///
  /// @param account The account.
  /// @param title The rechecked title.
  /// @param now The wall-clock time now.
  /// @return What the account is now known to own.
  auto with(AccountId account, OwnedTitle title, Time now) -> Owned;
  /// @brief Adds or replaces one PS5 entitlement after a recheck.
  ///
  /// The record replaces its entry in the PS5 scan, and its title (when it is a
  /// game) the matching library title. Without a held library both go into the
  /// rechecked-only state.
  ///
  /// @param account The account.
  /// @param record The rechecked entitlement.
  /// @param now The wall-clock time now.
  /// @return What the account is now known to own.
  auto with(AccountId account, Ps5Entitlement record, Time now) -> Owned;
  /// @brief What the account owns after a library read.
  ///
  /// @param account The account.
  /// @param fetched The library read, or nullptr when it could not be read.
  /// @return The fetched library as LibraryState::fetched; or, when it could
  ///     not be read, the titles rechecked so far (possibly none) as
  ///     LibraryState::unavailable.
  auto after_read(AccountId account, LibraryPtr fetched) -> Owned;
  /// @brief Holds a PS5 scan read while the commerce library could not be.
  ///
  /// Its games become the PS5 part of the rechecked-only library, marked
  /// unavailable.
  ///
  /// @param account The account.
  /// @param ps5 The scan; must not be null.
  void hold_ps5_alone(AccountId account, Ps5Ptr ps5);
  /// @brief The rechecked-only library after a failed read.
  ///
  /// @param account The account.
  /// @return The rechecked titles as unavailable, or std::nullopt when the
  ///     latest read did not fail or nothing was rechecked.
  auto rechecked_alone(AccountId account) const -> std::optional<Owned>;
  /// @brief Drops the account's fetched library and PS5 scan.
  ///
  /// @param account The account.
  void drop_library(AccountId account);
  /// @brief Drops every account's fetched library and PS5 scan.
  void drop_every_library();
  /// @brief Forgets everything held for an account.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief What is held for one account.
  struct Account {
    /// @brief The fetched library; null when none is held.
    LibraryPtr library{};
    /// @brief The fetched library's PS5 scan.
    Ps5Ptr ps5{};
    /// @brief Titles rechecked while the library could not be read.
    LibraryPtr checked_alone{};
    /// @brief PS5 records read while the library could not be read.
    Ps5Ptr ps5_checked_alone{};
    /// @brief Whether the latest library read failed.
    bool library_unavailable{};
  };

  /// @brief What is held, by account id.
  std::map<std::uint64_t, Account> accounts_;
};

}  // namespace libchiaking::catalog::cache
