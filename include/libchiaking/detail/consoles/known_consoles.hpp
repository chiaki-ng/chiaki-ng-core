/// @file
/// @brief Declares KnownConsoles, the registry that finds, pairs, remembers and
/// forgets consoles.
#pragma once

#include "libchiaking/detail/consoles/account_console.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/console_store.hpp"
#include "libchiaking/detail/consoles/console_titles.hpp"
#include "libchiaking/detail/consoles/game_invitation.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/consoles/host.hpp"
#include "libchiaking/detail/consoles/internet_pairing.hpp"
#include "libchiaking/detail/consoles/network_search.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/consoles/session_sharing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class ClockProvider;
}

namespace libchiaking::platform {
class ThreadingProvider;
}

namespace libchiaking::platform {
class CryptoProvider;
class DiagnosticsProvider;
class HttpProvider;
class NetworkProvider;
class SecureRandomProvider;
class WebSocketProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles {

/// @brief The set of consoles the library knows about, and the entry point for
/// finding and pairing them.
///
/// One KnownConsoles is created by the application and lives as long as
/// consoles are in use. It owns every Console and hands them out as
/// `std::shared_ptr<Console>`; there is exactly one Console per HardwareId.
/// Consoles enter the set by being loaded from the ConsoleStore, seen by a
/// network search, or paired.
///
/// The providers and the store passed to the constructor are referenced, not
/// owned, and must outlive this object. All members are safe to call from
/// several threads. Searching, pairing and the PSN calls block the calling
/// thread.
///
/// Destroying the object marks every console it handed out as forgotten, so
/// consoles still held elsewhere fail their operations cleanly. Nothing is
/// removed from the store by destruction.
///
/// @see Console
/// @see ConsoleStore
/// @ingroup api_consoles
class KnownConsoles {
 public:
  /// @brief What remember_saved() loaded.
  struct Remembered {
    /// @brief The consoles now known from the store, in stored order.
    std::vector<std::shared_ptr<Console>> consoles;
    /// @brief Number of stored rows that were skipped as unusable.
    ///
    /// A console row is skipped when its hardware id repeats, its address
    /// cannot reach its generation, or it contradicts the generation of a
    /// console already known. A pairing row is skipped when its console was
    /// not loaded, its account id is zero, or it repeats.
    std::size_t rejected{};
  };

  /// @brief Not copyable; there is one registry and it owns its consoles.
  KnownConsoles(const KnownConsoles&) = delete;
  /// @brief Not copy-assignable; there is one registry and it owns its
  /// consoles.
  auto operator=(const KnownConsoles&) -> KnownConsoles& = delete;
  /// @brief Marks every console as forgotten and releases the registry. The
  /// store is left untouched.
  ~KnownConsoles();

  /// @brief Loads the saved consoles and their pairings from the
  /// ConsoleStore.
  ///
  /// Call it once at start-up. Calling it again is harmless: a console that
  /// is already known is kept as the same object and marked as saved.
  ///
  /// @return The consoles loaded and the number of rows skipped.
  /// @retval ConsoleStore::Error::unavailable The store could not be read.
  ///     Nothing changes.
  [[nodiscard]] auto remember_saved()
      -> Expected<Remembered, ConsoleStore::Error>;
  /// @brief Removes a console and all its pairings, from the store and from
  /// this registry.
  ///
  /// On success the console reports Console::is_forgotten, holds no pairings
  /// and is no longer returned by find() or everything(). Pointers to it held
  /// elsewhere stay valid. A console that was only found on the network is
  /// dropped without touching the store, and may be found again by a later
  /// search.
  ///
  /// @param console The console to forget. A console this registry does not
  ///     hold is ignored and reported as success.
  /// @return Nothing on success.
  /// @retval ConsoleStore::Error::unavailable The store refused the removal.
  ///     The console stays known and paired.
  [[nodiscard]] auto forget(Console& console)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Removes one account's pairing with a console, keeping the
  /// console and every other account's pairing with it.
  ///
  /// Use it when an account should no longer play on a console from this
  /// host, or when the console has refused the account's pairing keys. The
  /// pairing is deleted from the ConsoleStore through
  /// ConsoleStore::remove_pairing and then from the console, so
  /// Console::is_paired_with reports false for that account and
  /// Console::paired_accounts no longer lists it. The console stays known and
  /// saved even when it was its last pairing. A console that was only found
  /// on the network has nothing in the store, so the store is not called.
  ///
  /// Blocks while the store deletes the row. Safe to call from any thread;
  /// it is serialised with forget() and with pairing.
  ///
  /// @param account The account whose pairing goes.
  /// @param console The console it was paired with.
  /// @return Nothing on success, including when the console is not held by
  ///     this registry, has been forgotten, or holds no pairing for the
  ///     account; nothing is changed in those cases.
  /// @retval ConsoleStore::Error::unavailable The store refused the delete.
  ///     The pairing stays in place and still works.
  [[nodiscard]] auto unpair(AccountId account, Console& console)
      -> Expected<void, ConsoleStore::Error>;
  /// @brief Attaches the object that starts play sessions for this registry's
  /// consoles.
  ///
  /// Console::start and Console::route_for forward to it. Until one is
  /// attached they fail with Unreachable::not_available.
  ///
  /// @param starting The starter. Not owned; it must stay valid until it is
  ///     replaced or this registry is destroyed. Null detaches.
  void start_through(Starting* starting) noexcept;
  /// @brief Connects the WebSocket provider game invitations are watched
  /// through.
  ///
  /// ChiakiNg connects it when it wires the registry. Safe to call from any
  /// thread.
  ///
  /// @param websocket The provider, or null to disconnect it; must outlive
  ///     its connection.
  void watch_through(platform::WebSocketProvider* websocket) noexcept;

  /// @brief Looks a console up by its hardware id.
  ///
  /// @param hardware_id The console's MAC address.
  /// @return The console, or null when none with that id is known.
  auto find(HardwareId hardware_id) const -> std::shared_ptr<Console>;
  /// @brief Returns every known console, saved or only seen, as a snapshot.
  ///
  /// @return A copy of the list at the time of the call; empty when no console
  /// is known. Later changes do not affect it.
  auto everything() const -> std::vector<std::shared_ptr<Console>>;

  /// @brief Searches the home network for consoles until told to stop.
  ///
  /// The call blocks and keeps searching. It broadcasts on every usable
  /// network interface and, when the ranges ask for it, probes each address
  /// of the extra subnets in turn. A subnet given a provider of its own with
  /// SearchRanges::over is probed through that provider, by unicast only,
  /// which the search holds until the call returns. Every time what it sees
  /// changes, the watcher is called on the calling thread with the new
  /// NetworkView. Consoles that answer join the registry (unsaved until paired)
  /// and have their presence, name, address and running title refreshed; known
  /// consoles that stop answering have their presence updated. A reply sent
  /// from an address no console can be reached at, in 0.0.0.0/8 or at 224
  /// and above, is ignored, so a console that answers before it has an
  /// address is not recorded at 0.0.0.0.
  ///
  /// Several threads may search at once with the same ranges; they share one
  /// search. A call with different ranges while a search is running is
  /// refused.
  ///
  /// @param ranges Extra subnets to search beyond local broadcast. A
  ///     default-constructed value searches by broadcast only.
  /// @param watcher Receives each new view. Must stay valid for the call.
  /// @param stop Ends the search.
  /// @return Nothing once the stop token is triggered. Stopping is the normal
  ///     way out and is not reported as an error.
  /// @retval SearchFailure::busy A search with different ranges is already
  ///     running.
  /// @retval SearchFailure::network The search could not be started or
  ///     stopped working.
  /// @retval SearchFailure::permission_denied The operating system denied
  ///     access to the local network. The watcher sees a view with
  ///     NetworkView::permission_denied set first.
  [[nodiscard]] auto find_on_my_network(const SearchRanges& ranges,
                                        NetworkWatcher& watcher,
                                        const platform::StopToken& stop)
      -> Expected<void, SearchFailure>;

  /// @brief Pairs a user with a console that was found on the network.
  ///
  /// The console must be showing its pairing PIN. The call uses the console's
  /// last known address and learned firmware, blocks for up to 30 seconds,
  /// and on success saves the console and the pairing to the ConsoleStore.
  ///
  /// @param as The user to pair. A user without a PSN account can pair,
  ///     except with a PS4 on system software below 7.0.
  /// @param found The console to pair with, typically from a NetworkView.
  /// @param pin The PIN the console displays. Consumed and wiped.
  /// @param stop Cancels the attempt.
  /// @param watcher Told each PairingStep on the calling thread. May be null.
  /// @return The paired console, now saved. It is the registry's object for
  ///     that hardware id.
  /// @retval PairingFailure::Reason::unreachable The console has no last
  ///     known address, or it could not be reached there.
  /// @retval PairingFailure::Reason::address_not_for_generation The address
  ///     is IPv6 and the console is a PS4.
  /// @retval PairingFailure::Reason::needs_online_id The console is a PS4
  ///     below 7.0 and the user has no PSN account.
  /// @retval PairingFailure::Reason::refused The console rejected the
  ///     request, most often because of a wrong PIN.
  ///     PairingFailure::http_status and PairingFailure::ps_reason carry
  ///     its reply.
  /// @retval PairingFailure::Reason::unreadable_reply A cryptographic step
  ///     failed, usually decrypting the reply.
  /// @retval PairingFailure::Reason::malformed The request could not be
  ///     built, or the reply lacked a usable hardware id or keys.
  /// @retval PairingFailure::Reason::timed_out The console did not finish
  ///     within the deadline.
  /// @retval PairingFailure::Reason::cancelled The stop token was triggered.
  /// @retval PairingFailure::Reason::busy The exchange was not in a state to
  ///     continue.
  /// @retval PairingFailure::Reason::not_saved The console accepted the
  ///     pairing but the ConsoleStore refused to save it, so it is not kept.
  [[nodiscard]] auto pair_at_home(const users::PsUser& as, Console& found,
                                  PairingPin pin,
                                  const platform::StopToken& stop,
                                  PairingWatcher* watcher = nullptr)
      -> Expected<std::shared_ptr<Console>, PairingFailure>;
  /// @brief Pairs a user with a console at an address the user supplies.
  ///
  /// Use this when the console was not found by a search. The newest firmware
  /// family of the generation is assumed. Otherwise this behaves like the
  /// other overload and fails for the same reasons, except that an address is
  /// always present.
  ///
  /// @param as The user to pair.
  /// @param at Where the console is.
  /// @param generation Whether the console is a PS4 or a PS5.
  /// @param pin The PIN the console displays. Consumed and wiped.
  /// @param stop Cancels the attempt.
  /// @param watcher Told each PairingStep on the calling thread. May be null.
  /// @return The paired console, now saved, or the reason pairing failed.
  [[nodiscard]] auto pair_at_home(const users::PsUser& as, const Host& at,
                                  Generation generation, PairingPin pin,
                                  const platform::StopToken& stop,
                                  PairingWatcher* watcher = nullptr)
      -> Expected<std::shared_ptr<Console>, PairingFailure>;

  /// @brief Lists the consoles PSN holds for a user's account.
  ///
  /// For Generation::ps5 PSN is asked, and the answer is also used to match
  /// the user's saved PS5 consoles to the listed ones: a match records the
  /// console's DeviceUid with the pairing and adopts PSN's name where it
  /// outranks the one held. For Generation::ps4 PSN is not asked, because it
  /// does not list PS4 consoles; the result is the single placeholder
  /// AccountConsole::main_ps4.
  ///
  /// @param of The signed-in user whose account to list.
  /// @param generation Which generation to list.
  /// @param stop Cancels the PSN request.
  /// @return The consoles on the account. Entries PSN states without a usable
  ///     identifier are left out.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto consoles_on_account(users::PsUser& of,
                                         Generation generation,
                                         const platform::StopToken& stop)
      -> Expected<std::vector<AccountConsole>, users::Unanswered>;
  /// @brief Pairs a user with a console on their account without being on its
  /// network.
  ///
  /// The console is reached through PSN using the InternetPairing given to
  /// the constructor. On success the console and pairing are saved together
  /// with the console's DeviceUid, and the console takes PSN's name where
  /// that outranks the one held. The call blocks until pairing finishes.
  ///
  /// @param as The signed-in user to pair.
  /// @param which The console, from consoles_on_account().
  /// @param watcher Told the stages and steps reached. May be null.
  /// @param stop Cancels the attempt.
  /// @return The paired console, now saved.
  /// @retval PairingFailure::Reason::internet_pairing_missing No
  ///     InternetPairing was given to the constructor.
  /// @retval PairingFailure::Reason::unanswered The user has no PSN sign-in;
  ///     PairingFailure::unanswered says so.
  /// @retval PairingFailure::Reason::remote_play_disabled PSN reports Remote
  ///     Play as switched off on the console.
  /// @retval PairingFailure::Reason::session_playing Internet pairing was
  ///     refused because a play session is running.
  /// @retval PairingFailure::Reason::not_saved Pairing succeeded but the
  ///     ConsoleStore refused to save it.
  /// @retval PairingFailure Any other reason the InternetPairing reports,
  ///     with PairingFailure::internet_stage saying how far it got.
  [[nodiscard]] auto pair_over_the_internet(users::PsUser& as,
                                            const AccountConsole& which,
                                            PairingWatcher* watcher,
                                            const platform::StopToken& stop)
      -> Expected<std::shared_ptr<Console>, PairingFailure>;

  /// @brief Fetches what a user played recently, grouped by the PS5 consoles
  /// it is installed on.
  ///
  /// PSN is asked afresh on every call, and the answer replaces the one
  /// Console::titles reuses. Only PS5 consoles that the user is paired with
  /// and that could be matched to a console on the account appear.
  ///
  /// @param by The signed-in user.
  /// @param stop Cancels the PSN requests.
  /// @return One entry per matched console, each with its recently played
  ///     titles. A console with none still appears with an empty list.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto recently_played(users::PsUser& by,
                                     const platform::StopToken& stop)
      -> Expected<std::vector<RecentlyPlayed>, users::Unanswered>;

  /// @brief Lists the game invitations friends sent the user.
  ///
  /// PSN is asked afresh on every call for the account's notifications. Each
  /// invitation that carries an invitation id is joinable, by
  /// play::Session::join_invitation during a PS5 cloud stream.
  ///
  /// @param of The signed-in user.
  /// @param stop Cancels the PSN requests.
  /// @return The invitations, newest first.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto game_invitations(users::PsUser& of,
                                      const platform::StopToken& stop)
      -> Expected<std::vector<GameInvitation>, users::Unanswered>;
  /// @brief Clears invitations from the account's notification list by
  /// marking them deleted, so they are not listed again.
  ///
  /// @param of The signed-in user.
  /// @param notification_ids The invitations' GameInvitation::notification_ids.
  /// @param stop Cancels the request.
  /// @return Nothing once PSN accepted the change.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto dismiss_invitations(
      users::PsUser& of, std::span<const std::string> notification_ids,
      const platform::StopToken& stop) -> Expected<void, users::Unanswered>;
  /// @brief Lists the user's PSN friends, online friends first.
  ///
  /// Up to 500 friends are listed. Friends whose profile or presence PSN
  /// would not give are listed unnamed or offline.
  ///
  /// @param of The signed-in user.
  /// @param stop Cancels the requests.
  /// @return The friends.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto friends(users::PsUser& of, const platform::StopToken& stop)
      -> Expected<std::vector<PsnFriend>, users::Unanswered>;
  /// @brief Finds the game session the user plays in for a title, so friends
  /// can be invited to it.
  ///
  /// @param of The signed-in user.
  /// @param title The title being played.
  /// @param platform The console the game runs on.
  /// @param stop Cancels the requests.
  /// @return The session, or nothing when the user plays in none for the
  ///     title.
  /// @retval users::Unanswered::Reason::signed_out The user has no PSN
  ///     sign-in.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto own_game_session(users::PsUser& of, const NpTitleId& title,
                                      GamePlatform platform,
                                      const platform::StopToken& stop)
      -> Expected<std::optional<OwnGameSession>, users::Unanswered>;
  /// @brief Asks PSN for a link friends can open to join a game session that
  /// anyone may join.
  ///
  /// @param of The signed-in user.
  /// @param session_id The session, from own_game_session().
  /// @param stop Cancels the request.
  /// @return The link.
  /// @retval users::Unanswered PSN did not answer or gave no link.
  [[nodiscard]] auto share_game_session(users::PsUser& of,
                                        std::string_view session_id,
                                        const platform::StopToken& stop)
      -> Expected<SessionLink, users::Unanswered>;
  /// @brief Invites friends to the user's game session or party.
  ///
  /// A game session invitation is sent in the same form as a party
  /// invitation; that form is unconfirmed.
  ///
  /// @param of The signed-in user.
  /// @param target The session or party.
  /// @param accounts The friends, from PsnFriend::account_id.
  /// @param stop Cancels the requests.
  /// @return Nothing once PSN accepted every invitation.
  /// @retval users::Unanswered PSN did not answer or refused the
  ///     invitations.
  [[nodiscard]] auto invite_friends(users::PsUser& of,
                                    const InvitationTarget& target,
                                    std::span<const AccountId> accounts,
                                    const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;
  /// @brief Watches for game and party invitations and messages as PSN
  /// pushes them, until stopped.
  ///
  /// Lists the invitations once, then opens the account's PSN push channel
  /// as a mobile client, with the app token. The invitations are listed again
  /// only when PSN pushes that an invitation was created, so nothing is asked
  /// of PSN on a timer; each unread invitation not listed before goes to
  /// `watcher`, as does each party invitation and each message another
  /// account sends. A channel that drops is opened again after a
  /// pause; blocks until `stop` fires or the channel cannot be opened.
  ///
  /// @param of The signed-in user.
  /// @param watcher Told of each new invitation and message, on this
  ///     thread.
  /// @param stop Ends the watch.
  /// @return Nothing once stopped.
  /// @retval users::Unanswered The first listing failed, the push channel
  ///     could not be opened again after several pauses, or no WebSocket
  ///     provider is connected (Reason::unavailable).
  [[nodiscard]] auto watch_alerts(users::PsUser& of, AlertWatcher& watcher,
                                  const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;
  /// @brief Has the PS5 cloud console the user is streaming from join an
  /// invitation's session.
  ///
  /// The session is read again, the account's cloud console is found among
  /// its consoles and PSN's join command is sent to it, then followed until
  /// the console reports it finished. A cloud console that has just started
  /// streaming is not listed until it signs in, so the consoles are listed
  /// again every three seconds for up to a minute while none is. Blocks for
  /// up to that minute and then ten seconds after PSN accepts the command.
  ///
  /// @param as The signed-in user.
  /// @param session_id The invitation's GameInvitation::session_id.
  /// @param watcher Told each state of the join command; may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure Why it did not join.
  [[nodiscard]] auto join_in_cloud(users::PsUser& as,
                                   std::string_view session_id,
                                   StartWatcher* watcher,
                                   const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief Has one of the user's home PS5s join an invitation's game
  /// session through PSN.
  ///
  /// The command goes through PSN, so no connection to the console is opened
  /// and a resting PS5 takes it too. The session is read first and the join
  /// refused when the user already plays in it, it is full or it has ended.
  /// Blocks until the console reports the join finished.
  ///
  /// @param console The PS5, from this registry.
  /// @param as The signed-in user; must be paired with the console.
  /// @param session_id The invitation's GameInvitation::session_id.
  /// @param watcher Told each state of the command, on the calling thread;
  ///     may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure::Reason::console_unavailable The console
  ///     has been forgotten, is a PS4, holds no pairing for the user or could
  ///     not be matched to one on the account.
  /// @retval InvitationJoinFailure Any other reason the join did not happen.
  [[nodiscard]] auto join_on(Console& console, users::PsUser& as,
                             std::string_view session_id, StartWatcher* watcher,
                             const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief Has one of the user's PS5s join a friend's party through PSN,
  /// moving the party's voice chat to that PS5.
  ///
  /// The command goes through PSN, so no connection to the console is opened
  /// and a resting PS5 takes it too. The call then polls PSN until the join
  /// completes, fails, or about two minutes pass.
  ///
  /// @param console The PS5, from this registry.
  /// @param as The signed-in user; must be paired with the console.
  /// @param session_id The party, from a GameInvitation of
  ///     GameInvitation::Kind::party or a PartyInvitationAlert.
  /// @param watcher Told each state of the command, on the calling thread;
  ///     may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure::Reason::console_unavailable The console
  ///     has been forgotten, is a PS4, holds no pairing for the user or could
  ///     not be matched to one on the account.
  /// @retval InvitationJoinFailure Any other reason the join did not happen.
  [[nodiscard]] auto join_party_on(Console& console, users::PsUser& as,
                                   std::string_view session_id,
                                   StartWatcher* watcher,
                                   const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief The registry's internal state. Defined inside the library; not
  /// for frontends.
  class Core;

  /// @brief Reaches the internal state. For the library's own use.
  ///
  /// @return The state, which lives as long as this registry.
  auto core() noexcept -> Core& { return *core_; }

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Creates the registry.
  ///
  /// @param network Sends searches, wake packets and pairing requests.
  /// @param http Carries the PSN requests.
  /// @param crypto Performs the pairing cryptography.
  /// @param random Supplies the random bytes pairing needs.
  /// @param store Persists consoles and pairings.
  /// @param threading Makes the registry's locks, condition variables and
  ///     sleeps. Must outlive the registry.
  /// @param clock Monotonic clock used for every deadline. Must outlive the
  ///     registry.
  /// @param internet Pairs over the internet. May be null, which makes
  ///     pair_over_the_internet() fail with
  ///     PairingFailure::Reason::internet_pairing_missing.
  /// @param log Receives diagnostics. May be null.
  KnownConsoles(platform::NetworkProvider& network,
                platform::HttpProvider& http, platform::CryptoProvider& crypto,
                platform::SecureRandomProvider& random, ConsoleStore& store,
                platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock,
                InternetPairing* internet = nullptr,
                platform::DiagnosticsProvider* log = nullptr);

  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::consoles
