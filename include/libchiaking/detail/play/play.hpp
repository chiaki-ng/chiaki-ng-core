/// @file
/// @brief Declares Play, the entry point that starts sessions at home, over the
/// internet and on the cloud.
#pragma once

#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/detail/play/nat_type.hpp"
#include "libchiaking/detail/play/preferences.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/play/session_events.hpp"
#include "libchiaking/detail/play/start_refusal.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <memory>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class DiagnosticsProvider;
class PlatformFactory;
}  // namespace libchiaking::platform

namespace libchiaking::consoles {
class KnownConsoles;
}

namespace libchiaking::diagnostics {
class SessionJournal;
}

namespace libchiaking::catalog {
class Catalog;
}

namespace libchiaking::play {

namespace testing {
class ScriptedLink;
}

/// @brief Starts play sessions and keeps track of the one that is live.
///
/// A frontend creates one Play and keeps it for as long as it runs. Each start
/// function validates the request, returns a Session that is still connecting,
/// and leaves the connection to the session's own thread. Progress and the
/// final result arrive through SessionEvents, never through the return value of
/// the start.
///
/// Only one session can be live at a time. A start is refused with
/// StartRefusal::Kind::busy while an earlier session has not ended or while
/// InternetPairer is pairing. Start functions may be called from any thread;
/// concurrent calls are served one after another.
///
/// @see Session
/// @see SessionEvents
/// @see StartRefusal
/// @ingroup api_play
class Play {
 public:
  /// @brief Implementation behind Play; not part of the public surface.
  class Core;

  /// @brief Not copyable; a frontend has one Play.
  Play(const Play&) = delete;
  /// @brief Not copy-assignable; a frontend has one Play.
  auto operator=(const Play&) -> Play& = delete;
  /// @brief Releases Play and stops the pairer from consulting it.
  ///
  /// @pre Every session started by this Play has ended and been released.
  ~Play();

  /// @brief Starts remote play with a console on the local network.
  ///
  /// The call does not wait for the connection. It checks the request and
  /// returns a session in SessionState::connecting; the session then wakes
  /// the console if needed and retries a failed connection a limited number
  /// of times, reporting through SessionEvents::on_connect_progress.
  ///
  /// @param player Who is playing. Must be paired with `console`. A PSN
  ///     account is not needed.
  /// @param console Console to play on; it must have a known address.
  /// @param preferences Stream preferences, fitted to the console's
  ///     generation. Preferences::home can give the stream a network
  ///     provider of its own; the session keeps it until it has been
  ///     released, and waking the console goes through it too.
  /// @param media Receives video and audio. Borrowed; must outlive the
  ///     session.
  /// @param events Receives progress, prompts and the outcome on the
  ///     session's thread. Borrowed; must outlive the session.
  /// @return The session, shared with Play until it has ended.
  /// @retval StartRefusal::Kind::busy A session is live or the pairer is
  ///     pairing.
  /// @retval StartRefusal::Kind::invalid_preferences `player` or `console` is
  ///     null, or the preferences cannot be turned into a valid stream
  ///     request.
  /// @retval StartRefusal::Kind::not_paired `player` is not paired with
  ///     `console`.
  /// @retval StartRefusal::Kind::console_unreachable The console was
  ///     forgotten, has no address, or a required platform provider is
  ///     missing; see StartRefusal::unreachable.
  /// @retval StartRefusal::Kind::host_refused The host could not start the
  ///     session's thread.
  /// @see Play::start_over_internet
  [[nodiscard]] auto start_on_console(
      std::shared_ptr<users::PsUser> player,
      std::shared_ptr<consoles::Console> console, Preferences preferences,
      media::MediaSink& media, SessionEvents& events)
      -> Expected<std::shared_ptr<Session>, StartRefusal>;
  /// @brief Starts remote play with a console away from home, through PSN.
  ///
  /// Unlike Play::start_on_console this call can take a while before it
  /// returns: when the pairing for this route is not already held, it asks
  /// PSN for the consoles on the player's account. `stop` cancels that wait.
  /// The connection itself is then made on the session's thread and reported
  /// through SessionEvents::on_connect_progress. If the console is on the
  /// account but was never paired by this client, the session pairs it on the
  /// way and saves the pairing. A PS4 is the exception: PSN reaches the
  /// account's primary PS4, so the session ends with
  /// EndReason::Cause::unpaired_console when the PS4 that answers is not one
  /// the player has paired with this client.
  ///
  /// @param player Who is playing. Must be signed in to PSN.
  /// @param console Console to reach.
  /// @param preferences Stream preferences; Preferences::internet applies
  ///     here.
  /// @param media Receives video and audio. Borrowed; must outlive the
  ///     session.
  /// @param events Receives progress, prompts and the outcome on the
  ///     session's thread. Borrowed; must outlive the session.
  /// @param stop Cancels the start while it is still looking the console up.
  /// @return The session, shared with Play until it has ended.
  /// @retval StartRefusal::Kind::busy A session is live or the pairer is
  ///     pairing.
  /// @retval StartRefusal::Kind::needs_psn_sign_in `player` has no PSN
  ///     account or is signed out.
  /// @retval StartRefusal::Kind::sign_in_expired The PSN sign-in has expired.
  /// @retval StartRefusal::Kind::invalid_preferences `player` or `console` is
  ///     null, or the preferences cannot be turned into a valid stream
  ///     request.
  /// @retval StartRefusal::Kind::cancelled `stop` was requested.
  /// @retval StartRefusal::Kind::not_paired The console is not on the
  ///     player's PSN account.
  /// @retval StartRefusal::Kind::remote_play_disabled The account lists the
  ///     console with remote play turned off.
  /// @retval StartRefusal::Kind::console_unreachable The console was
  ///     forgotten, PSN did not answer, or a required platform provider is
  ///     missing; see StartRefusal::unreachable.
  /// @retval StartRefusal::Kind::host_refused The host could not start the
  ///     session's thread.
  [[nodiscard]] auto start_over_internet(
      std::shared_ptr<users::PsUser> player,
      std::shared_ptr<consoles::Console> console, Preferences preferences,
      media::MediaSink& media, SessionEvents& events,
      const platform::StopToken& stop = {})
      -> Expected<std::shared_ptr<Session>, StartRefusal>;
  /// @brief Starts a cloud streaming session for a title.
  ///
  /// The call checks what can be checked without the network and returns a
  /// session in SessionState::connecting. The launch itself (clearing the
  /// title with the catalog, reserving a server, choosing a datacenter) runs
  /// on the session's thread. Its progress arrives through
  /// SessionEvents::on_launch_progress and can be read from Session::launch.
  /// A launch that PS refuses does not fail this call; it ends the session,
  /// and Outcome::launch_refused says why.
  ///
  /// @param player Who is playing. Must be signed in to PSN.
  /// @param title Product to stream. The cloud service is derived from it.
  /// @param preferences Stream preferences, fitted to that service.
  /// @param datacenter How the datacenter is decided.
  /// @param media Receives video and audio. Borrowed; must outlive the
  ///     session.
  /// @param events Receives progress, prompts and the outcome on the
  ///     session's thread. Borrowed; must outlive the session.
  /// @param cleared A clearance the caller already obtained from
  ///     catalog::Catalog::clear_for_launch. When its product, or the
  ///     entitlement it is held as, is `title`, the launch uses it and does
  ///     not clear the title with the catalog again; otherwise it is ignored.
  ///     Empty to have the launch clear the title itself.
  /// @return The session, shared with Play until it has ended.
  /// @retval StartRefusal::Kind::busy A session is live or the pairer is
  ///     pairing.
  /// @retval StartRefusal::Kind::needs_psn_sign_in `player` has no PSN
  ///     account, is signed out, or has no usable sign-in credential.
  /// @retval StartRefusal::Kind::sign_in_expired The PSN sign-in has expired.
  /// @retval StartRefusal::Kind::invalid_preferences `player` is null, or the
  ///     preferences or datacenter preference cannot be turned into a valid
  ///     launch request.
  /// @retval StartRefusal::Kind::launch_refused The launch could not be
  ///     prepared at all; see StartRefusal::launch.
  /// @retval StartRefusal::Kind::host_refused The host could not start the
  ///     session's thread.
  /// @see CloudLaunch
  [[nodiscard]] auto start_on_cloud(
      std::shared_ptr<users::PsUser> player, const catalog::ProductId& title,
      Preferences preferences, DatacenterPreference datacenter,
      media::MediaSink& media, SessionEvents& events,
      std::optional<catalog::ClearedLaunch> cleared = std::nullopt)
      -> Expected<std::shared_ptr<Session>, StartRefusal>;
  /// @brief Starts a session over a scripted link, for tests and design
  /// previews.
  ///
  /// No console or service is contacted. The link plays back what the script
  /// says, so a frontend can exercise its screens without real hardware. The
  /// same admission rules apply as for a real start of the same kind.
  ///
  /// @param player Who is playing.
  /// @param where Kind of session to imitate.
  /// @param link Script that stands in for the console or service. Borrowed;
  ///     must outlive the session.
  /// @param preferences Stream preferences.
  /// @param media Receives video and audio. Borrowed; must outlive the
  ///     session.
  /// @param events Receives progress and the outcome. Borrowed; must outlive
  ///     the session.
  /// @return The session; Session::console is null for every scripted
  ///     session.
  /// @retval StartRefusal::Kind::busy A session is live or the pairer is
  ///     pairing.
  /// @retval StartRefusal::Kind::needs_psn_sign_in `where` is not Where::home
  ///     and `player` has no PSN account or is signed out.
  /// @retval StartRefusal::Kind::sign_in_expired `where` is not Where::home
  ///     and the PSN sign-in has expired.
  /// @retval StartRefusal::Kind::invalid_preferences `player` is null or the
  ///     session could not be opened.
  /// @retval StartRefusal::Kind::host_refused The host could not start the
  ///     session's thread.
  [[nodiscard]] auto start_scripted(std::shared_ptr<users::PsUser> player,
                                    Where where, testing::ScriptedLink& link,
                                    Preferences preferences,
                                    media::MediaSink& media,
                                    SessionEvents& events)
      -> Expected<std::shared_ptr<Session>, StartRefusal>;

  /// @brief Chooses the journal later sessions write their diagnostics into.
  ///
  /// @param journal Journal shared with every session started after this
  ///     call; null lets each session keep its own. A session already started
  ///     is not affected.
  /// @see diagnostics::SessionJournal
  /// @see Session::journal
  void journal_sessions_in(
      std::shared_ptr<diagnostics::SessionJournal> journal);

  /// @brief Returns the session that is currently live.
  ///
  /// @return The session while it is connecting, playing or leaving; null
  ///     when there is none or the last one has ended.
  auto live() const -> std::shared_ptr<Session>;
  /// @brief Finds this host's NAT type with PSN's STUN servers.
  ///
  /// Opens one UDP socket, asks PSN's party voice STUN server and then a
  /// public STUN server for its public mapping and compares the answers with
  /// the host's own addresses. Blocks for DNS and a few seconds per server
  /// that does not answer; needs no sign-in or session.
  ///
  /// @param stop Cancels the check.
  /// @return The NAT type.
  /// @retval NatTypeFailure Why it could not be determined.
  [[nodiscard]] auto nat_type(const platform::StopToken& stop = {})
      -> Expected<NatType, NatTypeFailure>;
  /// @brief Returns the datacenter measurements kept for one cloud service.
  ///
  /// The first call for a service reads them from DatacenterStore; later
  /// calls answer from memory. A cloud launch adds what it measured unless
  /// the player forced its datacenter (DatacenterChoice::was_forced).
  ///
  /// @param service Service to list.
  /// @return A snapshot, fastest first; empty when nothing is stored or the
  ///     store could not be read.
  auto known_datacenters(catalog::ServiceGeneration service) const
      -> KnownDatacenters;
  /// @brief Discards the datacenter measurements kept for one cloud service.
  ///
  /// The in-memory set is cleared whether or not the store accepts the
  /// change.
  ///
  /// @param service Service to clear.
  /// @return Nothing on success.
  /// @retval DatacenterStore::Error::unavailable The store could not be
  ///     written; the measurements may reappear after a restart.
  [[nodiscard]] auto forget_datacenters(catalog::ServiceGeneration service)
      -> Expected<void, DatacenterStore::Error>;
  /// @brief Leaves the live session if it belongs to an account that is being
  /// removed.
  ///
  /// Call it when a player signs out or their account is deleted. The session
  /// is asked to leave as Session::leave would; the call does not wait for it
  /// to end. Nothing happens when the live session belongs to someone else.
  ///
  /// @param account PSN account being removed.
  void forget(AccountId account);

  /// @brief Returns the port consoles use to start sessions on themselves.
  ///
  /// @return The interface behind consoles::Console::start, owned by this
  ///     Play.
  auto starting() noexcept -> consoles::Starting&;

  /// @brief Returns the implementation, for the library's own wiring and
  /// tests.
  ///
  /// @return The implementation, for the library's own wiring and tests.
  auto core() noexcept -> Core& { return *core_; }

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Wires Play to the frontend's platform services and stores.
  ///
  /// Every reference is borrowed and must outlive this Play.
  ///
  /// @param platform Source of the network, crypto, HTTP, WebSocket and clock
  ///     providers sessions run on.
  /// @param consoles Registry of the consoles the frontend knows; consulted
  ///     for pairings and updated when an internet start pairs a console.
  /// @param catalog Catalog that decides whether a title may be streamed from
  ///     the cloud.
  /// @param datacenters Storage for datacenter measurements.
  /// @param pairer Internet pairer. Play refuses starts while it is pairing,
  ///     and it refuses to pair while a session is live.
  /// @param log Sink for diagnostics; when null, the one `platform` provides
  ///     is used.
  Play(platform::PlatformFactory& platform, consoles::KnownConsoles& consoles,
       catalog::Catalog& catalog, DatacenterStore& datacenters,
       InternetPairer& pairer, platform::DiagnosticsProvider* log = nullptr);
  /// @brief Wraps an implementation built by the library's own wiring or a
  /// test.
  ///
  /// @param core The implementation; must not be null.
  explicit Play(std::unique_ptr<Core> core) noexcept;

  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play
