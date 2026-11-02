/// @file
/// @brief Declares Console, one PS4 or PS5 the library knows about, and the
/// outcomes of looking for it and waking it.
#pragma once

#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/console_store.hpp"
#include "libchiaking/detail/consoles/console_titles.hpp"
#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/consoles/home_network.hpp"
#include "libchiaking/detail/consoles/host.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/consoles/starting.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class CryptoProvider;
class DiagnosticsProvider;
class HttpProvider;
class NetworkProvider;
class SecureRandomProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles {

class ConsoleFacts;
class ConsoleLibrary;
class InternetPairing;
class KnownConsoles;
class PairedPlay;
class SearchReply;

/// @brief Where a console stood the last time the library looked for it.
///
/// A Presence is a remembered observation, not a live probe. It changes when
/// KnownConsoles::find_on_my_network runs, and when Console::check or
/// Console::wake_and_wait is called.
enum class Presence : std::uint8_t {
  /// @brief Nothing has settled whether the console is on this network.
  ///
  /// This is the state of a console that has not been searched for, of one
  /// that was forgotten, and of every console that did not answer a network
  /// search in which no console answered at all.
  unknown,
  /// @brief The console answered and is switched on.
  ready,
  /// @brief The console answered and is in rest mode. It can be woken.
  rest_mode,
  /// @brief The console did not answer a search or check that could have seen
  /// it.
  not_nearby,
};

/// @brief The title a console reported as running when it last answered.
/// @ingroup api_consoles
struct RunningTitle {
  /// @brief The title's identifier as the console reports it.
  std::string title_id;
  /// @brief The title's display name as the console reports it.
  std::string name;

  /// @brief Compares both fields.
  friend auto operator==(const RunningTitle&, const RunningTitle&) noexcept
      -> bool = default;
};

/// @brief Why Console::check could not say where a console is.
enum class Unreachable : std::uint8_t {
  /// @brief The console has no last known address to ask.
  no_address,
  /// @brief The search could not be sent or its replies could not be read.
  network,
  /// @brief The console did not answer within Console::check_deadline.
  timed_out,
  /// @brief The stop token was triggered.
  cancelled,
  /// @brief The console has been forgotten, or its KnownConsoles is gone.
  forgotten,
  /// @brief The library is not set up to do this.
  ///
  /// Console::check does not return it. Console::start and Console::route_for
  /// report it, wrapped in a play::StartRefusal, when no Starting has been
  /// attached with KnownConsoles::start_through or a platform provider needed
  /// for the route is missing.
  not_available,
};

/// @brief Why a console was not woken.
enum class WakeRefusal : std::uint8_t {
  /// @brief The user holds no usable pairing with the console.
  not_paired,
  /// @brief The console has no last known address to send the wake packet to.
  no_address,
  /// @brief The search or the wake packet could not be sent, or the operating
  /// system denied access to the local network.
  network,
  /// @brief The console did not answer the search within
  /// Console::find_deadline.
  not_found,
  /// @brief The wake packet was sent but the console was still in rest mode
  /// after Console::ready_deadline.
  still_in_rest_mode,
  /// @brief The stop token was triggered.
  cancelled,
  /// @brief The console has been forgotten, or its KnownConsoles is gone.
  forgotten,
};

/// @brief One PS4 or PS5 the library knows about, whether paired, saved or only
/// seen on the network.
///
/// Consoles are created and owned by KnownConsoles and handed out as
/// `std::shared_ptr<Console>`. A console is identified by its HardwareId and
/// its Generation, which never change. Everything else (name, firmware,
/// address, presence, pairings) is updated as the console is seen, paired or
/// edited, so the accessors return snapshots.
///
/// All members are safe to call from several threads. The calls that reach the
/// network or PSN (check, wake_and_wait, titles, both start overloads,
/// route_for) block the calling thread until they finish or their stop token is
/// triggered.
///
/// Once KnownConsoles::forget has been called for a console, or the
/// KnownConsoles that owns it has been destroyed, the object stays valid for
/// whoever still holds it but is_forgotten() is true, its pairings are gone and
/// its operations fail with the `forgotten` value of their error type.
///
/// @see KnownConsoles
/// @ingroup api_consoles
class Console {
 public:
  /// @brief Why Console::rename or Console::moved_to left the console as it
  /// was.
  enum class Unchanged : std::uint8_t {
    /// @brief The name is blank, longer than ConsoleName::limit or holds a
    /// control character.
    invalid_name,
    /// @brief The address is an IPv6 literal and the console is a PS4.
    address_not_for_generation,
    /// @brief The account holds no pairing with the console. Not returned
    /// by rename or moved_to.
    not_paired,
    /// @brief The console is saved and the ConsoleStore refused the write.
    /// The value in memory is unchanged.
    not_saved,
    /// @brief The console has been forgotten, or its KnownConsoles is gone.
    forgotten,
  };

  /// @brief How long check() waits for the console to answer.
  static constexpr std::chrono::milliseconds check_deadline{3000};
  /// @brief Longest single wait inside check() before the stop token is
  /// looked at again.
  static constexpr std::chrono::milliseconds check_wait_step{150};
  /// @brief How long wake_and_wait() searches for the console before giving
  /// up with WakeRefusal::not_found.
  static constexpr std::chrono::seconds find_deadline{15};
  /// @brief How long wake_and_wait() waits for a woken console to leave rest
  /// mode.
  static constexpr std::chrono::seconds ready_deadline{25};
  /// @brief How often wake_and_wait() resends the wakeup while waiting for a
  /// console to leave rest mode, since the wakeup is an unacknowledged UDP
  /// packet that a console in rest mode may miss.
  static constexpr std::chrono::seconds wake_resend_interval{1};

  /// @brief Not copyable; a console is one shared object owned by
  /// KnownConsoles.
  Console(const Console&) = delete;
  /// @brief Not copy-assignable; a console is one shared object owned by
  /// KnownConsoles.
  auto operator=(const Console&) -> Console& = delete;
  /// @brief Releases the console's state. Does not touch the ConsoleStore.
  ~Console();

  /// @brief The console's MAC address, which identifies it for its whole
  /// life.
  ///
  /// @return The MAC address as a HardwareId.
  auto hardware_id() const noexcept -> HardwareId { return hardware_id_; }
  /// @brief Whether the console is a PS4 or a PS5. Never changes.
  ///
  /// @return Generation::ps4 or Generation::ps5.
  auto generation() const noexcept -> Generation { return generation_; }
  /// @brief The firmware family last learned for the console.
  ///
  /// It is refreshed when the console answers a search and when it is paired.
  /// It decides which Remote Play protocol variant is spoken to the console.
  ///
  /// @return The firmware family last learned; the default family when none has
  /// been learned yet.
  auto firmware() const -> Firmware;
  /// @brief The console's current display name.
  ///
  /// @see name_source
  ///
  /// @return A copy of the current name.
  auto name() const -> std::string;
  /// @brief Where the current name came from, which decides what may replace
  /// it.
  ///
  /// @see ConsoleName::Source
  ///
  /// @return The source of the current name.
  auto name_source() const -> ConsoleName::Source;
  /// @brief The address the console was last seen at, paired at or moved to.
  ///
  /// @return The address, or nothing when the console has never been reached
  ///     at one, for example a console paired only over the internet.
  auto last_address() const -> std::optional<Host>;
  /// @brief Where the console stood the last time it was looked for.
  ///
  /// This reads the remembered value and sends nothing. Call check() to look
  /// now.
  ///
  /// @return The remembered presence; it can be stale until the console is
  /// searched for again.
  auto presence() const -> Presence;
  /// @brief The title the console reported running when it last answered.
  ///
  /// @return The title, or nothing when the console reported none, has not
  ///     answered, or has since gone out of sight.
  auto running_title() const -> std::optional<RunningTitle>;

  /// @brief Whether the console has a row in the ConsoleStore.
  ///
  /// A console becomes saved when it is paired or loaded by
  /// KnownConsoles::remember_saved. One that was only found on the network is
  /// not saved, and edits to it stay in memory.
  ///
  /// @return True when the console has a row in the ConsoleStore, false
  /// otherwise.
  auto is_saved() const noexcept -> bool;
  /// @brief Whether the console was forgotten or its KnownConsoles destroyed.
  ///
  /// @return True once the console has been forgotten or its KnownConsoles
  /// destroyed, false otherwise.
  auto is_forgotten() const noexcept -> bool;
  /// @brief Whether any account holds a pairing with the console.
  ///
  /// @return True when at least one account holds pairing keys for the console,
  /// false when none does.
  auto is_paired() const -> bool;
  /// @brief Whether the given user's account holds a pairing with the
  /// console.
  ///
  /// @param user The user to ask about. Matched by PsUser::account_id.
  /// @return True when pairing keys are held for that account.
  auto is_paired_with(const users::PsUser& user) const -> bool;
  /// @brief The accounts that hold a pairing with the console, as a snapshot.
  ///
  /// @return The ids of the accounts holding pairing keys, in no guaranteed
  /// order; empty when none does.
  auto paired_accounts() const -> std::vector<AccountId>;

  /// @brief Asks the console at its last known address whether it is on.
  ///
  /// Sends a search to last_address() only and waits up to check_deadline for
  /// the answer. An answer also refreshes the console's firmware, name,
  /// address and running title. The remembered presence() is updated either
  /// way; a timeout sets it to Presence::not_nearby.
  ///
  /// @param stop Cancels the wait.
  /// @param over The network provider the search goes through for this call
  ///     only, held until the call returns; by default the platform's own.
  /// @return Presence::ready or Presence::rest_mode, as the console answered.
  /// @retval Unreachable::forgotten The console has been forgotten.
  /// @retval Unreachable::no_address The console has no last known address.
  /// @retval Unreachable::network The search could not be sent or read.
  /// @retval Unreachable::cancelled The stop token was triggered.
  /// @retval Unreachable::timed_out No answer arrived within check_deadline.
  [[nodiscard]] auto check(const platform::StopToken& stop = {},
                           const HomeNetwork& over = {})
      -> Expected<Presence, Unreachable>;
  /// @brief Sends the wake packet to the console's last known address and
  /// returns.
  ///
  /// The call does not wait to see whether the console wakes. Use
  /// wake_and_wait() for that. The packet always goes to the console's own
  /// address, never to a broadcast address.
  ///
  /// @param as The user whose pairing with the console authorises the wake.
  /// @param over The network provider the packet is sent through for this
  ///     call only, held until the call returns; by default the platform's
  ///     own.
  /// @return Nothing on success, meaning only that the packet was sent.
  /// @retval WakeRefusal::forgotten The console has been forgotten.
  /// @retval WakeRefusal::not_paired The user holds no pairing, or its
  ///     registration key cannot be used to wake.
  /// @retval WakeRefusal::no_address The console has no last known address.
  /// @retval WakeRefusal::network The packet could not be sent.
  [[nodiscard]] auto wake(const users::PsUser& as, const HomeNetwork& over = {})
      -> Expected<void, WakeRefusal>;
  /// @brief Finds the console on the network, wakes it if it is in rest mode,
  /// and waits until it is ready.
  ///
  /// The call first searches the local network, and the last known address
  /// when that is IPv4, for up to find_deadline. A console that answers as
  /// ready ends the call at once. One that answers in rest mode is sent the
  /// wake packet and then given up to ready_deadline to report itself ready.
  /// Answers refresh the console's facts and presence(). Over a provider
  /// that cannot broadcast, only the last known IPv4 address is searched.
  ///
  /// @param as The user whose pairing with the console authorises the wake.
  /// @param stop Cancels the wait.
  /// @param over The network provider the search and the wake packet go
  ///     through for this call only, held until the call returns; by default
  ///     the platform's own.
  /// @return Nothing on success; the console answered as switched on.
  /// @retval WakeRefusal::forgotten The console has been forgotten.
  /// @retval WakeRefusal::not_paired The user holds no pairing, or its
  ///     registration key cannot be used to wake.
  /// @retval WakeRefusal::network The search or the wake packet could not be
  ///     sent, or local network access was denied.
  /// @retval WakeRefusal::cancelled The stop token was triggered.
  /// @retval WakeRefusal::not_found The console did not answer the search.
  ///     presence() becomes Presence::not_nearby.
  /// @retval WakeRefusal::no_address The console answered in rest mode but
  ///     has no address to wake it at, or `over` cannot broadcast and the
  ///     console has no IPv4 address to search at.
  /// @retval WakeRefusal::still_in_rest_mode The console was still in rest
  ///     mode after ready_deadline.
  [[nodiscard]] auto wake_and_wait(const users::PsUser& as,
                                   const platform::StopToken& stop,
                                   const HomeNetwork& over = {})
      -> Expected<void, WakeRefusal>;
  /// @brief Lists the games installed on a PS5, as PSN reports them for the
  /// user.
  ///
  /// The list comes from PSN, not from the console, so the console may be
  /// off. The answer for an account is held in memory after the first call
  /// and reused by later calls; KnownConsoles::recently_played refreshes it.
  /// Titles are ordered with recently played ones first, most recent first,
  /// then by name. Entries PSN categorises as applications are left out.
  ///
  /// @param of The signed-in user whose account the console is on.
  /// @param stop Cancels the PSN requests.
  /// @return The installed titles. May be empty.
  /// @retval TitlesUnavailable::Reason::forgotten The console has been
  ///     forgotten.
  /// @retval TitlesUnavailable::Reason::ps5_only The console is a PS4.
  /// @retval TitlesUnavailable::Reason::unanswered PSN did not answer;
  ///     TitlesUnavailable::unanswered says why, including
  ///     users::Unanswered::Reason::signed_out for a user with no PSN
  ///     sign-in.
  /// @retval TitlesUnavailable::Reason::not_paired The user holds no pairing
  ///     with the console.
  /// @retval TitlesUnavailable::Reason::not_on_account None of the account's
  ///     consoles could be matched to this one.
  [[nodiscard]] auto titles(users::PsUser& of, const platform::StopToken& stop)
      -> Expected<std::vector<InstalledTitle>, TitlesUnavailable>;
  /// @brief Asks PSN to start an installed title on a PS5 and waits for the
  /// outcome.
  ///
  /// The command goes through PSN, so no connection to the console is opened.
  /// The call then polls PSN until the start completes, fails, or a deadline
  /// of about ten seconds passes.
  ///
  /// @param title The title to start, from titles() or
  ///     KnownConsoles::recently_played.
  /// @param as The signed-in user to start it as; must be paired with the
  ///     console.
  /// @param watcher Told each state as it is reached, on the calling thread,
  ///     starting with StartState::pending once PSN accepts the command. May
  ///     be null.
  /// @param stop Cancels the wait.
  /// @return The state PSN reported last: StartState::completed, or
  ///     StartState::failed when PSN says the console could not start the
  ///     title. A failed start is a value here, not an error.
  /// @retval StartFailure::Reason::forgotten The console has been forgotten.
  /// @retval StartFailure::Reason::ps5_only The console is a PS4.
  /// @retval StartFailure::Reason::not_paired The user holds no pairing with
  ///     the console.
  /// @retval StartFailure::Reason::not_on_account The console could not be
  ///     matched to one on the account.
  /// @retval StartFailure::Reason::refused PSN rejected the command with a
  ///     client error other than an authorisation refusal.
  /// @retval StartFailure::Reason::unanswered PSN did not answer;
  ///     StartFailure::unanswered says why.
  /// @retval StartFailure::Reason::cancelled The stop token was triggered.
  /// @retval StartFailure::Reason::timed_out The deadline passed;
  ///     StartFailure::reached holds the last state seen.
  [[nodiscard]] auto start(const InstalledTitle& title, users::PsUser& as,
                           StartWatcher* watcher,
                           const platform::StopToken& stop)
      -> Expected<StartState, StartFailure>;
  /// @brief Starts a Remote Play session with the console.
  ///
  /// The work is done by the Starting attached with
  /// KnownConsoles::start_through; this call only forwards to it. With
  /// Route::automatic the console's presence and pairings decide between the
  /// home network and the internet, which may probe the network or ask PSN
  /// first.
  ///
  /// @param player The user to play as. Shared with the session for its
  ///     lifetime.
  /// @param preferences Video, audio and input preferences for the session.
  /// @param media Receives the session's video and audio. Must outlive the
  ///     session.
  /// @param events Receives the session's events. Must outlive the session.
  /// @param route Which way to reach the console.
  /// @param stop Cancels the start while the route is being worked out.
  /// @return The session, which is still connecting; its progress and final
  ///     outcome arrive through `events`.
  /// @retval play::StartRefusal Why no session was created. It reports
  ///     Unreachable::not_available when no Starting is attached and
  ///     Unreachable::forgotten when the console is no longer the one
  ///     KnownConsoles holds.
  /// @see Starting::start
  /// @see route_for
  [[nodiscard]] auto start(std::shared_ptr<users::PsUser> player,
                           const play::Preferences& preferences,
                           media::MediaSink& media, play::SessionEvents& events,
                           Route route = Route::automatic,
                           const platform::StopToken& stop = {})
      -> Expected<std::shared_ptr<play::Session>, play::StartRefusal>;
  /// @brief Works out which route Route::automatic would take for this
  /// console now.
  ///
  /// A paired console that is ready or in rest mode on this network is played
  /// at home, and one that is not nearby is played over the internet. When
  /// the presence is unknown the console is checked first, and a console the
  /// user is not paired with may be looked up on the account, so the call can
  /// block.
  ///
  /// @param player The user who would play.
  /// @param stop Cancels the lookups.
  /// @return Route::home or Route::internet, never Route::automatic.
  /// @retval play::StartRefusal The route could not be worked out: no
  ///     Starting is attached, the console is forgotten, or the stop token
  ///     was triggered.
  [[nodiscard]] auto route_for(users::PsUser& player,
                               const platform::StopToken& stop = {})
      -> Expected<Route, play::StartRefusal>;
  /// @brief Gives the console a name chosen by the user.
  ///
  /// The name takes ConsoleName::Source::renamed, which nothing learned later
  /// from the console or from PSN replaces. A saved console is written to the
  /// ConsoleStore before the name changes in memory.
  ///
  /// @param name The new name.
  /// @return Nothing on success.
  /// @retval Unchanged::invalid_name The name is blank, too long or holds a
  ///     control character.
  /// @retval Unchanged::forgotten The console has been forgotten.
  /// @retval Unchanged::not_saved The ConsoleStore refused the write; the old
  ///     name is kept.
  [[nodiscard]] auto rename(std::string name) -> Expected<void, Unchanged>;
  /// @brief Records a new address for the console, for example after the user
  /// types one in.
  ///
  /// A saved console is written to the ConsoleStore before the address
  /// changes in memory. The address is not probed.
  ///
  /// @param host The address to use from now on.
  /// @return Nothing on success.
  /// @retval Unchanged::address_not_for_generation The address is IPv6 and
  ///     the console is a PS4.
  /// @retval Unchanged::forgotten The console has been forgotten.
  /// @retval Unchanged::not_saved The ConsoleStore refused the write; the old
  ///     address is kept.
  [[nodiscard]] auto moved_to(Host host) -> Expected<void, Unchanged>;

  /// @brief The console's internal state. Defined inside the library; not for
  /// frontends.
  class Core;

  /// @brief Wraps internal state in a console. Used by KnownConsoles;
  /// frontends do not construct consoles.
  ///
  /// @param core The state to own. Must not be null.
  explicit Console(std::unique_ptr<Core> core);

  /// @brief Reaches the internal state. For the library's own use.
  ///
  /// @return The state, which lives as long as this console.
  auto core() const noexcept -> Core& { return *core_; }

 private:
  auto starting() const noexcept -> Starting*;

  const HardwareId hardware_id_;
  const Generation generation_;
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::consoles
