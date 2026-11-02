/// @file
/// @brief Declares ConsoleFacts, through which the consoles context's use
/// cases record what they learn about a console: sightings, names, losses,
/// pairings and account links.
///
/// The use cases speak in these domain events; ConsoleFacts turns each into
/// the right change on the Roster or the Console::Core, and saves the facts
/// when the console is saved.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "src/core/consoles/model/console/sighting.hpp"

#include <memory>
#include <optional>

namespace libchiaking::consoles {

class Roster;

/// @brief The record keeper the find, wake and pair use cases tell what they
/// learned about consoles.
///
/// It only refers to a Roster and holds no state of its own, so it may be
/// copied freely and used from several threads at once; the roster and the
/// consoles provide the locking. It must not outlive the roster.
class ConsoleFacts {
 public:
  /// @brief What one search reply showed about a console.
  using Sighting = consoles::Sighting;

  /// @brief Records facts in a registry's roster.
  ///
  /// @param consoles The registry; its roster must outlive this object.
  explicit ConsoleFacts(KnownConsoles& consoles) noexcept;
  /// @brief Records facts in a roster.
  ///
  /// @param roster The roster; must outlive this object.
  explicit ConsoleFacts(Roster& roster) noexcept : roster_(roster) {}

  /// @brief The roster the facts are recorded in.
  ///
  /// @return The roster.
  auto roster() const noexcept -> Roster& { return roster_; }

  /// @brief Records a search reply from a console that may not be known yet.
  ///
  /// The console is admitted to the roster when it is new, then the sighting
  /// is recorded on it as recognised() does.
  ///
  /// @param sighting What the reply showed.
  /// @return The console that answered; never null.
  auto sighted(Sighting sighting) -> std::shared_ptr<Console>;
  /// @brief Records that a known console answered.
  ///
  /// The firmware, a name that ranks at least as high as the held one, and an
  /// address the console's generation can use replace what is held; the
  /// presence and running title are always replaced. A saved console whose
  /// firmware, name or address changed is saved again, blocking while the
  /// store is written; a failed write is logged, not reported. A forgotten
  /// console is left alone.
  ///
  /// @param console The console that answered.
  /// @param sighting What the reply showed; empty records nothing.
  void recognised(Console& console, std::optional<Sighting> sighting);
  /// @brief Records that a console did not answer and clears its running
  /// title.
  ///
  /// @param console The console.
  /// @param search_still_looking True when nothing settled whether the
  ///     console is nearby (the console becomes Presence::unknown), false
  ///     when a search or check that could have seen it heard nothing
  ///     (Presence::not_nearby).
  void out_of_sight(Console& console, bool search_still_looking);
  /// @brief Records a name learned for a console, such as the one PSN holds.
  ///
  /// The name is taken only when it ranks at least as high as the held one
  /// and differs from it; a saved console is then saved again, blocking while
  /// the store is written, and a failed write is logged, not reported.
  ///
  /// @param console The console.
  /// @param name The name learned.
  void named(Console& console, const ConsoleName& name);

  /// @brief Records a pairing a console granted, saving the console and the
  /// pairing; see Roster::keep.
  ///
  /// @param as The user the console was paired for.
  /// @param paired The pairing the console granted.
  /// @param at Where the console answered, when known.
  /// @param device_uid The console's PSN device uid, when known.
  /// @return The paired console.
  /// @retval ConsoleStore::Error The store refused a write.
  [[nodiscard]] auto paired(const users::PsUser& as, PairedConsole paired,
                            const std::optional<Host>& at = {},
                            std::optional<DeviceUid> device_uid = {})
      -> Expected<std::shared_ptr<Console>, ConsoleStore::Error>;
  /// @brief Records which console on the user's PSN account a paired console
  /// is, by its device uid; see Console::Core::link.
  ///
  /// @param console The paired console.
  /// @param user The user whose pairing is linked.
  /// @param device_uid The device uid PSN lists for the console.
  /// @return Nothing when the link is recorded or was already held.
  /// @retval Console::Unchanged::forgotten The console was forgotten.
  /// @retval Console::Unchanged::not_paired The user holds no pairing with
  ///     the console.
  /// @retval Console::Unchanged::not_saved The store refused the write.
  [[nodiscard]] auto linked(Console& console, const users::PsUser& user,
                            DeviceUid device_uid)
      -> Expected<void, Console::Unchanged>;

  /// @brief Reads the keys of the user's pairing with a console.
  ///
  /// @param console The console.
  /// @param user The user.
  /// @return A copy of the keys, or nothing when the user holds no pairing
  ///     with the console.
  auto keys_of(const Console& console, const users::PsUser& user) const
      -> std::optional<PairingKeys>;
  /// @brief Reads the PSN device uid linked to the user's pairing with a
  /// console.
  ///
  /// @param console The console.
  /// @param user The user.
  /// @return The device uid, or nothing when the user holds no pairing with
  ///     the console or it was never linked.
  auto device_uid_of(const Console& console, const users::PsUser& user) const
      -> std::optional<DeviceUid>;

 private:
  /// @brief The roster facts are recorded in.
  Roster& roster_;
};

}  // namespace libchiaking::consoles
