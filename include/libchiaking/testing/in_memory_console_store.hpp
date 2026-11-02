/// @file
/// @brief Declares InMemoryConsoleStore, a console store kept in memory.
#pragma once

#include "libchiaking/detail/consoles/console_store.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for consoles::ConsoleStore without a database.
///
/// Saved consoles and pairings are kept in memory for the life of the object.
/// Put starting state in with seed(), hand the store to the library, run the
/// code under test, then read back console(), pairing(), the counts and the
/// counters. The public flags script failures: unavailable fails everything,
/// and the refuse flags fail one kind of write while loading keeps working.
///
/// A console is identified by its hardware id and a pairing by its account and
/// console together; saving replaces the entry with the same identity.
///
/// @note The store's own functions lock internally. The public data members are
///     plain fields; set them before the code under test runs and read them
///     after it has stopped. No call blocks beyond taking that lock.
/// @ingroup api_testing
class InMemoryConsoleStore final : public consoles::ConsoleStore {
 public:
  /// @brief Creates an empty store.
  ///
  /// @param threading Makes the lock that guards the store; must outlive it.
  explicit InMemoryConsoleStore(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Returns everything the store holds.
  ///
  /// @return The saved consoles and independent copies of the saved pairings.
  /// @retval Error::unavailable unavailable is set.
  [[nodiscard]] auto load_saved_consoles() -> Expected<Saved, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable) return unexpected(Error::unavailable);
    Saved saved;
    saved.consoles = consoles_;
    for (const auto& pairing : pairings_)
      saved.pairings.push_back(pairing.clone());
    return saved;
  }

  /// @brief Saves a console, replacing one with the same hardware id.
  ///
  /// @param console The console to keep; a copy is stored.
  /// @return Nothing on success, after adding one to console_saves.
  /// @retval Error::unavailable unavailable or refuse_console_saves is set.
  ///     Nothing changes.
  [[nodiscard]] auto save(const consoles::SavedConsole& console)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable || refuse_console_saves)
      return unexpected(Error::unavailable);
    ++console_saves;
    std::erase_if(consoles_, [&console](const consoles::SavedConsole& held) {
      return held.hardware_id == console.hardware_id;
    });
    consoles_.push_back(console);
    return {};
  }

  /// @brief Saves a pairing, replacing one for the same account and console.
  ///
  /// @param pairing The pairing to keep; a copy is stored.
  /// @return Nothing on success, after adding one to pairing_saves.
  /// @retval Error::unavailable unavailable or refuse_pairing_saves is set.
  ///     Nothing changes.
  [[nodiscard]] auto save(const consoles::SavedPairing& pairing)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable || refuse_pairing_saves)
      return unexpected(Error::unavailable);
    ++pairing_saves;
    std::erase_if(pairings_, [&pairing](const consoles::SavedPairing& held) {
      return held.account == pairing.account && held.console == pairing.console;
    });
    pairings_.push_back(pairing.clone());
    return {};
  }

  /// @brief Removes one account's pairing with one console.
  ///
  /// @param account The paired account.
  /// @param console The paired console's hardware id.
  /// @return Nothing on success, after adding one to removals. Removing a
  ///     pairing that is not held also succeeds and is also counted.
  /// @retval Error::unavailable unavailable or refuse_removals is set.
  ///     Nothing changes.
  [[nodiscard]] auto remove_pairing(AccountId account,
                                    consoles::HardwareId console)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable || refuse_removals) return unexpected(Error::unavailable);
    ++removals;
    std::erase_if(pairings_,
                  [account, console](const consoles::SavedPairing& held) {
                    return held.account == account && held.console == console;
                  });
    return {};
  }

  /// @brief Removes a console and every pairing with it.
  ///
  /// @param console The console's hardware id.
  /// @return Nothing on success, after adding one to removals. Removing a
  ///     console that is not held also succeeds and is also counted.
  /// @retval Error::unavailable unavailable or refuse_removals is set.
  ///     Nothing changes.
  [[nodiscard]] auto remove_console(consoles::HardwareId console)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable || refuse_removals) return unexpected(Error::unavailable);
    ++removals;
    std::erase_if(consoles_, [console](const consoles::SavedConsole& held) {
      return held.hardware_id == console;
    });
    std::erase_if(pairings_, [console](const consoles::SavedPairing& held) {
      return held.console == console;
    });
    return {};
  }

  /// @brief Reads back a saved console, for assertions.
  ///
  /// @param hardware_id The console to look up.
  /// @return A copy of the saved console, or nothing when it is not held. Not
  ///     affected by unavailable.
  [[nodiscard]] auto console(consoles::HardwareId hardware_id)
      -> std::optional<consoles::SavedConsole> {
    const platform::Lock lock{mutex_};
    const auto found = std::ranges::find(consoles_, hardware_id,
                                         &consoles::SavedConsole::hardware_id);
    if (found == consoles_.end()) return std::nullopt;
    return *found;
  }

  /// @brief Reads back a saved pairing, for assertions.
  ///
  /// @param account The paired account.
  /// @param console The paired console's hardware id.
  /// @return A copy of the saved pairing, or nothing when it is not held. Not
  ///     affected by unavailable.
  [[nodiscard]] auto pairing(AccountId account, consoles::HardwareId console)
      -> std::optional<consoles::SavedPairing> {
    const platform::Lock lock{mutex_};
    const auto found = std::ranges::find_if(
        pairings_, [account, console](const consoles::SavedPairing& held) {
          return held.account == account && held.console == console;
        });
    if (found == pairings_.end()) return std::nullopt;
    return found->clone();
  }

  /// @brief Puts a console in the store as starting state.
  ///
  /// Unlike save() this ignores the failure flags, does not count as a save
  /// and does not replace an entry with the same hardware id, so seeding the
  /// same console twice leaves two entries.
  ///
  /// @param console The console to hold.
  void seed(consoles::SavedConsole console) {
    const platform::Lock lock{mutex_};
    consoles_.push_back(std::move(console));
  }

  /// @brief Puts a pairing in the store as starting state.
  ///
  /// Unlike save() this ignores the failure flags, does not count as a save
  /// and does not replace an entry for the same account and console.
  ///
  /// @param pairing The pairing to hold.
  void seed(consoles::SavedPairing pairing) {
    const platform::Lock lock{mutex_};
    pairings_.push_back(std::move(pairing));
  }

  /// @brief Reports how many consoles are held, seeded and saved together.
  ///
  /// Takes the store's lock, so it is safe beside calls from library
  /// threads.
  ///
  /// @return The number of consoles held now.
  auto console_count() -> std::size_t {
    const platform::Lock lock{mutex_};
    return consoles_.size();
  }

  /// @brief Reports how many pairings are held, seeded and saved together.
  ///
  /// Takes the store's lock, so it is safe beside calls from library
  /// threads.
  ///
  /// @return The number of pairings held now.
  auto pairing_count() -> std::size_t {
    const platform::Lock lock{mutex_};
    return pairings_.size();
  }

  /// @brief When true, loading, saving and removing all fail with
  /// Error::unavailable.
  bool unavailable{};
  /// @brief When true, saving a console fails with Error::unavailable.
  bool refuse_console_saves{};
  /// @brief When true, saving a pairing fails with Error::unavailable.
  bool refuse_pairing_saves{};
  /// @brief When true, remove_pairing() and remove_console() fail with
  /// Error::unavailable.
  bool refuse_removals{};
  /// @brief Number of consoles saved successfully through save().
  std::size_t console_saves{};
  /// @brief Number of pairings saved successfully through save().
  std::size_t pairing_saves{};
  /// @brief Number of successful remove_pairing() and remove_console() calls
  /// together.
  std::size_t removals{};

 private:
  platform::Mutex mutex_;
  std::vector<consoles::SavedConsole> consoles_;
  std::vector<consoles::SavedPairing> pairings_;
};

}  // namespace libchiaking::testing
