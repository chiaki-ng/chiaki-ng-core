/// @file
/// @brief Declares Roster, the one list of Console objects a KnownConsoles
/// holds, and the operations that add consoles to it, remove them and load
/// them from the ConsoleStore.
///
/// Every console the library hands out, whether found on the network, paired
/// or loaded from the store, comes from this list, so each hardware id is
/// represented by one Console object for the life of the registry.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
}

namespace libchiaking::consoles {

/// @brief The consoles one KnownConsoles knows, keyed by hardware id.
///
/// A console enters the roster when a search first sees it (admit), when it
/// is paired (keep) or when it is loaded from the store (remember_saved); it
/// leaves when it is forgotten. A console that is admitted but never paired
/// stays unsaved and lives only in memory.
///
/// The roster owns its consoles through shared pointers that it also hands
/// out; on destruction it tells every console it still holds to leave, so a
/// Console a frontend keeps afterwards reports itself forgotten. All members
/// may be called from any thread: changes to the list (and the store writes
/// that go with them) are serialised by one lock, and reads of the list by
/// another, so readers never wait for a store write.
class Roster {
 public:
  /// @brief Makes an empty roster.
  ///
  /// @param home The registry the consoles belong to, whose threading
  ///     provider makes the roster's locks; must outlive the roster.
  /// @param store Where saved consoles and pairings are kept; must outlive
  ///     the roster.
  /// @param log Where the consoles log store failures; null logs nothing.
  ///     Must outlive the roster.
  Roster(KnownConsoles::Core& home, ConsoleStore& store,
         platform::DiagnosticsProvider* log);
  /// @brief Not copyable: the consoles refer back to one registry.
  Roster(const Roster&) = delete;
  /// @brief Not copyable.
  auto operator=(const Roster&) -> Roster& = delete;
  /// @brief Tells every console still held to leave: each drops its pairings
  /// and reports itself forgotten. Nothing is removed from the store.
  ~Roster();

  /// @brief Loads the saved consoles and their pairings from the store into
  /// the roster.
  ///
  /// A saved console already in the roster (found by an earlier search) is
  /// kept, marked saved and given the saved name when that name ranks at
  /// least as high as the one it holds. A pairing is loaded only when its
  /// console has none for that account yet. Rows that cannot be used are
  /// skipped and counted, as KnownConsoles::Remembered::rejected describes.
  /// Blocks the calling thread while the store is read and while other
  /// changes to the roster finish.
  ///
  /// @return The consoles loaded, in stored order, and the count of skipped
  ///     rows.
  /// @retval ConsoleStore::Error The store could not be read; the roster is
  ///     unchanged.
  [[nodiscard]] auto remember_saved()
      -> Expected<KnownConsoles::Remembered, ConsoleStore::Error>;
  /// @brief Forgets a console: removes it and its pairings from the store
  /// when it is saved, then drops it from the roster.
  ///
  /// A console that is not in this roster, or was already forgotten, is left
  /// alone and reported as forgotten. Blocks while the store is written.
  ///
  /// @param console The console.
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error The store refused the removal; the console
  ///     is still known.
  [[nodiscard]] auto forget(Console& console)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Drops one account's pairing with a console, from the store when
  /// the console is saved and then from memory.
  ///
  /// A console that is not in this roster, is forgotten, or holds no pairing
  /// for `account` is left alone and reported as done. Blocks while the store
  /// is written.
  ///
  /// @param account The account whose pairing is dropped.
  /// @param console The console.
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error The store refused the removal; the pairing
  ///     is kept.
  [[nodiscard]] auto unpair(AccountId account, Console& console)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Looks a console up by its hardware id.
  ///
  /// @param hardware_id The console's hardware id.
  /// @return The console, or null when the roster holds none with that id.
  auto find(HardwareId hardware_id) const -> std::shared_ptr<Console>;
  /// @brief Lists every console in the roster, in the order they entered it.
  ///
  /// @return A snapshot of the list; consoles added or forgotten afterwards
  ///     are not reflected in it.
  auto everything() const -> std::vector<std::shared_ptr<Console>>;

  /// @brief Returns the console with `hardware_id`, adding an unsaved one
  /// when the roster holds none.
  ///
  /// A new console takes `firmware` and `name`, or its generation's
  /// placeholder name when `name` is empty. An existing console is returned
  /// unchanged; the caller records the sighting on it.
  ///
  /// @param hardware_id The console's hardware id.
  /// @param firmware The firmware the console reported, which fixes its
  ///     generation.
  /// @param name The name the console reported, if any.
  /// @return The console; never null.
  auto admit(HardwareId hardware_id, Firmware firmware,
             const std::optional<ConsoleName>& name)
      -> std::shared_ptr<Console>;
  /// @brief Records a new pairing: adds the console when the roster lacks it,
  /// saves it and the pairing, and holds the keys in memory.
  ///
  /// A console already held with the same hardware id but another generation
  /// is forgotten first and replaced. Blocks while the store is written; see
  /// Console::Core::keep_pairing for what is written and how a failed write
  /// is undone.
  ///
  /// @param as The user the console was paired for.
  /// @param paired The pairing the console granted.
  /// @param at Where the console answered; ignored when it cannot reach the
  ///     console's generation.
  /// @param device_uid The PSN device uid of the console, when known; the one
  ///     held for the account is kept otherwise.
  /// @return The paired console, now saved.
  /// @retval ConsoleStore::Error The store refused a write; a console that
  ///     was new is not added.
  [[nodiscard]] auto keep(const users::PsUser& as, PairedConsole paired,
                          const std::optional<Host>& at,
                          std::optional<DeviceUid> device_uid)
      -> Expected<std::shared_ptr<Console>, ConsoleStore::Error>;

 private:
  /// @brief Makes a console belonging to this registry from its facts.
  ///
  /// @param facts What is known about the console.
  /// @param saved Whether the console already has a row in the store.
  /// @return The new console; it is not yet in the list.
  auto made(SavedConsole facts, bool saved) const -> std::shared_ptr<Console>;
  /// @brief Finds the shared pointer the roster holds for a console object.
  ///
  /// @param console The console.
  /// @return The roster's pointer to it, or null when the object is not in
  ///     the list.
  auto held(const Console& console) const -> std::shared_ptr<Console>;

  /// @brief The registry the consoles belong to.
  KnownConsoles::Core& home_;
  /// @brief Where saved consoles and pairings are kept.
  ConsoleStore& store_;
  /// @brief Where the consoles log store failures; null when nothing is
  /// logged.
  platform::DiagnosticsProvider* log_;
  /// @brief Serialises changes to the list and the store writes that go with
  /// them; held across store calls.
  platform::Mutex changes_;
  /// @brief Guards #consoles_; never held across a store call.
  mutable platform::Mutex mutex_;
  /// @brief The consoles, in the order they entered the roster; at most one
  /// per hardware id.
  std::vector<std::shared_ptr<Console>> consoles_;
};

}  // namespace libchiaking::consoles
