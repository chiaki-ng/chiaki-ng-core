/// @file
/// @brief Declares Console::Core, the state behind a Console: what is known
/// about the console, which accounts are paired with it, and the rules for
/// changing and saving that knowledge.
///
/// A Console is the face handed to frontends; every query and change it
/// offers is answered here. The Roster makes one Core per console, and the
/// use cases change it only through ConsoleFacts.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
}

namespace libchiaking::consoles {

/// @brief One console's facts and pairings, kept in step with the
/// ConsoleStore while the console is saved.
///
/// The hardware id and generation never change. The firmware, name, last
/// address, presence, running title and pairings change as the console is
/// seen, renamed, paired and unpaired. A saved console writes each change of
/// its facts or pairings to the store, and a change the store refuses is not
/// made in memory, except where noted. Once forgotten (by forget() or leave())
/// the core holds no pairings, accepts no further facts and stays forgotten.
///
/// Owned by its Console through a unique pointer, so it lives exactly as long
/// as that Console. Every member may be called from any thread: one lock
/// serialises changes together with their store writes, and another guards
/// the values, so readers never wait for the store.
class Console::Core {
 public:
  /// @brief Makes the state for a console from its facts, with no pairings
  /// and Presence::unknown.
  ///
  /// @param home The registry the console belongs to, whose threading
  ///     provider makes the console's locks; must outlive this object.
  /// @param store Where the console is saved; must outlive this object.
  /// @param log Where failed background saves are logged; null logs nothing.
  ///     Must outlive this object.
  /// @param facts The console's hardware id, firmware, name and last address.
  /// @param saved Whether the console already has a row in the store.
  Core(KnownConsoles::Core& home, ConsoleStore& store,
       platform::DiagnosticsProvider* log, SavedConsole facts, bool saved);
  /// @brief Not copyable: one core per console.
  Core(const Core&) = delete;
  /// @brief Not copyable.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief The registry the console belongs to, whose use cases its actions
  /// run through.
  ///
  /// @return The registry's core.
  auto home() const noexcept -> KnownConsoles::Core& { return home_; }
  /// @brief The console's MAC address, which identifies it.
  ///
  /// @return The hardware id; fixed for the core's life.
  auto hardware_id() const noexcept -> HardwareId { return hardware_id_; }
  /// @brief Whether the console is a PS4 or a PS5.
  ///
  /// @return The generation; fixed for the core's life.
  auto generation() const noexcept -> Generation { return generation_; }
  /// @brief Whether the console has a row in the store.
  ///
  /// @return True once it is paired or loaded from the store, false for a
  ///     console only seen by a search and after it is forgotten.
  auto is_saved() const noexcept -> bool { return saved_; }
  /// @brief Whether the console was forgotten or its registry is gone.
  ///
  /// @return True once forget() or leave() has run; it never becomes false
  ///     again.
  auto is_forgotten() const noexcept -> bool { return forgotten_; }

  /// @brief The firmware family last learned.
  ///
  /// @return The firmware.
  auto firmware() const -> Firmware;
  /// @brief The display name.
  ///
  /// @return The name's text.
  auto name() const -> std::string;
  /// @brief Who supplied the display name.
  ///
  /// @return The name's source.
  auto name_source() const -> ConsoleName::Source;
  /// @brief The address the console was last reached at.
  ///
  /// @return The address, or nothing when none is known.
  auto last_address() const -> std::optional<Host>;
  /// @brief Whether the console was last seen on this network, and how.
  ///
  /// @return The presence; Presence::unknown until a search or check settles
  ///     it.
  auto presence() const -> Presence;
  /// @brief The title the console reported running when it last answered.
  ///
  /// @return The title, or nothing when it reported none or was not seen.
  auto running_title() const -> std::optional<RunningTitle>;
  /// @brief Whether any account holds a pairing with the console.
  ///
  /// @return True when at least one pairing is held.
  auto is_paired() const -> bool;
  /// @brief The accounts that hold a pairing with the console.
  ///
  /// @return The account ids, in the order their pairings were added.
  auto paired_accounts() const -> std::vector<AccountId>;
  /// @brief The console's facts as the store keeps them.
  ///
  /// @return The hardware id, firmware, name and last address now held.
  auto facts() const -> SavedConsole;
  /// @brief Whether an account holds a pairing with the console.
  ///
  /// @param account The account.
  /// @return True when a pairing is held for `account`.
  auto holds(AccountId account) const -> bool;
  /// @brief Copies the keys of an account's pairing.
  ///
  /// @param account The account.
  /// @return A deliberate copy of the keys, or nothing when `account` holds
  ///     no pairing.
  auto keys_of(AccountId account) const -> std::optional<PairingKeys>;
  /// @brief Reads the PSN device uid linked to an account's pairing.
  ///
  /// @param account The account.
  /// @return The device uid, or nothing when `account` holds no pairing or
  ///     the pairing was never linked.
  auto device_uid_of(AccountId account) const -> std::optional<DeviceUid>;

  /// @brief Gives the console a name chosen by the user, saving it first when
  /// the console is saved.
  ///
  /// Blocks while the store is written.
  ///
  /// @param name The new name.
  /// @return Nothing on success; the name's source becomes
  ///     ConsoleName::Source::renamed.
  /// @retval Unchanged::invalid_name ConsoleName::create refused the name.
  /// @retval Unchanged::forgotten The console was forgotten.
  /// @retval Unchanged::not_saved The store refused the write.
  [[nodiscard]] auto rename(std::string name) -> Expected<void, Unchanged>;
  /// @brief Records a new address for the console, saving it first when the
  /// console is saved.
  ///
  /// Blocks while the store is written.
  ///
  /// @param host The new address.
  /// @return Nothing on success.
  /// @retval Unchanged::address_not_for_generation The address cannot reach
  ///     this generation (an IPv6 literal for a PS4).
  /// @retval Unchanged::forgotten The console was forgotten.
  /// @retval Unchanged::not_saved The store refused the write.
  [[nodiscard]] auto moved_to(Host host) -> Expected<void, Unchanged>;
  /// @brief Records that the console answered, in memory only.
  ///
  /// Firmware of the console's generation, a name that ranks at least as high
  /// as the held one, and an address this generation can use replace what is
  /// held. The presence and running title are always replaced. Does nothing
  /// for a forgotten console.
  ///
  /// @param firmware The firmware the console reported.
  /// @param name The name it stated, if any.
  /// @param address Where it answered from.
  /// @param presence Whether it is switched on or in rest mode.
  /// @param running_title The title it said it is running, if any.
  /// @return True when the firmware, name or address changed, so the facts
  ///     need saving with keep_facts().
  auto seen(Firmware firmware, const std::optional<ConsoleName>& name,
            const Host& address, Presence presence,
            std::optional<RunningTitle> running_title) -> bool;
  /// @brief Takes a name learned elsewhere, in memory only, when it ranks at
  /// least as high as the held one.
  ///
  /// @param name The name.
  /// @return True when the name was taken (it differed and the console is not
  ///     forgotten), so the facts need saving with keep_facts().
  auto learn(const ConsoleName& name) -> bool;
  /// @brief Marks a console found by a search as saved after its row was
  /// loaded from the store, taking the saved name when it ranks at least as
  /// high as the held one.
  ///
  /// @param saved_name The name the store holds.
  void remembered(const ConsoleName& saved_name);
  /// @brief Records that the console did not answer, and clears its running
  /// title.
  ///
  /// @param presence Presence::unknown or Presence::not_nearby.
  void lost(Presence presence);
  /// @brief Holds a pairing in memory only, replacing any held for the same
  /// account.
  ///
  /// Used for pairings loaded from the store and, after their writes, by
  /// keep_pairing() and link().
  ///
  /// @param account The account.
  /// @param keys The keys the console issued.
  /// @param device_uid The linked PSN device uid, if known.
  void pair(AccountId account, PairingKeys keys,
            std::optional<DeviceUid> device_uid);
  /// @brief Saves the facts now held, when the console is saved and not
  /// forgotten.
  ///
  /// Blocks while the store is written. A refused write is logged as a
  /// warning and not reported; memory keeps the new facts.
  void keep_facts();
  /// @brief Saves a new pairing with the console and then holds it.
  ///
  /// The console row is written first with the pairing's firmware, a name
  /// that ranks at least as high as the held one and `at` when this
  /// generation can use it; then the pairing row. When the pairing row is
  /// refused, the console row is put back as it was, or removed when the
  /// console was not saved before; a failure to undo is logged. Only after
  /// both writes succeed are the facts and the pairing held in memory and the
  /// console marked saved. Blocks while the store is written.
  ///
  /// @param account The account the pairing belongs to.
  /// @param paired The pairing the console granted.
  /// @param at Where the console answered, if known.
  /// @param device_uid The console's PSN device uid; when empty, the one
  ///     already linked for `account` is kept.
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error The store refused a write; nothing changed
  ///     in memory.
  [[nodiscard]] auto keep_pairing(AccountId account, PairedConsole paired,
                                  const std::optional<Host>& at,
                                  std::optional<DeviceUid> device_uid)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Links an account's pairing to the console's PSN device uid,
  /// saving the pairing first when the console is saved.
  ///
  /// Blocks while the store is written.
  ///
  /// @param account The account.
  /// @param device_uid The device uid PSN lists for the console.
  /// @return Nothing when linked, or when the same device uid was already
  ///     linked.
  /// @retval Unchanged::forgotten The console was forgotten.
  /// @retval Unchanged::not_paired `account` holds no pairing.
  /// @retval Unchanged::not_saved The store refused the write.
  [[nodiscard]] auto link(AccountId account, DeviceUid device_uid)
      -> Expected<void, Unchanged>;
  /// @brief Forgets the console: removes its row (and with it its pairings)
  /// from the store when saved, then drops its pairings, presence and running
  /// title and marks it forgotten and unsaved.
  ///
  /// Forgetting a forgotten console does nothing. Blocks while the store is
  /// written.
  ///
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error The store refused the removal; nothing
  ///     changed.
  [[nodiscard]] auto forget() -> Expected<void, ConsoleStore::Error>;
  /// @brief Drops an account's pairing, from the store first when the console
  /// is saved.
  ///
  /// Does nothing when the console is forgotten or `account` holds no
  /// pairing. Blocks while the store is written.
  ///
  /// @param account The account.
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error The store refused the removal; the pairing
  ///     is kept.
  [[nodiscard]] auto unpair(AccountId account)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Detaches the console from a registry that is going away: drops
  /// its pairings, presence and running title and marks it forgotten, without
  /// touching the store.
  void leave();

 private:
  /// @brief One account's pairing with the console.
  struct Pairing {
    /// @brief The account the pairing belongs to; unique among #pairings_.
    AccountId account;
    /// @brief The secret keys the console issued for the account.
    PairingKeys keys;
    /// @brief PSN's device uid for the console on that account, once
    /// linked.
    std::optional<DeviceUid> device_uid;
  };

  /// @brief The registry the console belongs to.
  KnownConsoles::Core& home_;
  /// @brief Where the console is saved.
  ConsoleStore& store_;
  /// @brief Where failed background saves are logged; null when nothing is
  /// logged.
  platform::DiagnosticsProvider* log_;
  /// @brief The console's MAC address.
  const HardwareId hardware_id_;
  /// @brief The console's generation, fixed from the firmware it was made
  /// with.
  const Generation generation_;

  /// @brief Serialises changes together with their store writes; held across
  /// store calls.
  platform::Mutex changes_;
  /// @brief Guards the values below; never held across a store call.
  mutable platform::Mutex mutex_;
  /// @brief The firmware family last learned; always of #generation_.
  Firmware firmware_;
  /// @brief The display name and its source.
  ConsoleName name_;
  /// @brief Where the console was last reached; always an address this
  /// generation can use.
  std::optional<Host> last_address_;
  /// @brief The presence set by the last search, check or wake.
  Presence presence_{Presence::unknown};
  /// @brief The title reported in the last answer; cleared when the console
  /// is lost.
  std::optional<RunningTitle> running_title_;
  /// @brief The pairings held, in the order they were added; empty once
  /// forgotten.
  std::vector<Pairing> pairings_;
  /// @brief Whether the console has a row in the store.
  std::atomic<bool> saved_;
  /// @brief Whether the console was forgotten or its registry is gone.
  std::atomic<bool> forgotten_{};
};

}  // namespace libchiaking::consoles
