/// @file
/// @brief Declares usecases::Start, the use case that turns a player's request
/// to play into a running Session at home, over the internet or on the cloud.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/detail/consoles/starting.hpp"
#include "libchiaking/detail/play/play.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/runtime/utc_offset.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"
#include "src/core/play/adapters/cloud_launch/title_clearance.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/play/adapters/links/reconnecting_link.hpp"
#include "src/core/play/adapters/session/live_session.hpp"
#include "src/core/play/adapters/session/session_core.hpp"
#include "src/core/play/adapters/session/session_settings.hpp"
#include "src/core/play/adapters/start/link_preferences.hpp"
#include "src/core/play/usecases/live_play.hpp"

#include <memory>
#include <optional>

namespace libchiaking::play {

/// @brief What every start clears titles, waits and keeps time with, chosen
/// once when the start use case is built. Plain data; every reference must
/// outlive the use case.
struct StartCollaborators {
  /// @brief Clears cloud titles and names their service.
  TitleClearance& clearance;
  /// @brief Waits between the polls of a cloud launch.
  net::http::PollWaiter& waiter;
  /// @brief Supplies the time zone offset a cloud launch reports.
  const platform::UtcOffsetProvider& time_zone;
  /// @brief Timing and tolerances for the session loop.
  SessionSettings settings;
};

}  // namespace libchiaking::play

namespace libchiaking::play::usecases {

/// @brief Starts a play session: decides where it runs, checks the player may
/// start, builds the link and seats the session in the live slot.
///
/// It implements consoles::Starting, so Console::start lands here, and serves
/// the Play facade's own start calls. Only one start runs at a time and only
/// one session may be live; StartPolicy decides the refusals. A start returns
/// once the session is opened and begun; connecting happens on the session's
/// own thread, so only working out the route may block, on presence checks
/// and PSN lookups. Owned by Play::Core; every collaborator passed in must
/// outlive it. Not copyable. Starts from several threads are serialised by
/// the live slot.
class Start final : public consoles::Starting {
 public:
  /// @brief Where a console session will run and, for the internet, the target
  /// already found on the player's account while deciding.
  struct Way {
    /// @brief Home or internet.
    Where where;
    /// @brief The console as found on the player's PSN account; set only when
    /// the lookup that decided the route already found it.
    std::optional<consoles::PairedPlay::InternetTarget> found;
  };

  /// @brief The result of a start: the shared session, or why none was started.
  using Started = Expected<std::shared_ptr<Session>, StartRefusal>;

  /// @brief Builds the use case.
  ///
  /// @param platform The host's providers; must outlive the use case.
  /// @param consoles The known consoles; must outlive the use case.
  /// @param catalog Clears cloud titles and names their service; must outlive
  ///     the use case.
  /// @param store Where measured datacenters are kept; must outlive the use
  ///     case.
  /// @param datacenters The view the cloud launch reads and records
  ///     datacenters through; must outlive the use case.
  /// @param pairer Tells whether a pairing is in progress; must outlive the
  ///     use case.
  /// @param live The slot the started session is held in; must outlive the use
  ///     case.
  /// @param log Receives log lines; may be null.
  /// @param collaborators What every start clears titles, waits and keeps
  ///     time with; each reference must outlive the use case.
  Start(platform::PlatformFactory& platform, consoles::KnownConsoles& consoles,
        catalog::Catalog& catalog, DatacenterStore& store,
        Datacenters& datacenters, InternetPairer& pairer, LiveSession& live,
        platform::DiagnosticsProvider* log, StartCollaborators collaborators);
  /// @brief Not copyable; it stands for one Play.
  Start(const Start&) = delete;
  /// @brief Not copy-assignable; it stands for one Play.
  auto operator=(const Start&) -> Start& = delete;

  /// @brief Starts a session with a known console by the route asked for.
  ///
  /// Blocks while the route is worked out, which may check the console's
  /// presence on the network or look it up on the player's PSN account.
  ///
  /// @param player The user to play as.
  /// @param console The console; must be the object @p consoles holds now.
  /// @param preferences Video, audio and input preferences.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @param route Which way to reach the console.
  /// @param stop Cancels the route lookups.
  /// @return The session. StartRefusal::invalid_preferences without a player;
  ///     a console_unreachable refusal with Unreachable::forgotten when the
  ///     console is no longer known; otherwise any refusal from
  ///     Start::where_to_play, Start::on_console or Start::over_internet.
  [[nodiscard]] auto start(std::shared_ptr<users::PsUser> player,
                           consoles::Console& console,
                           const Preferences& preferences,
                           media::MediaSink& media, SessionEvents& events,
                           consoles::Route route,
                           const platform::StopToken& stop) -> Started override;
  /// @brief Works out the route Route::automatic would take now.
  ///
  /// Blocks like Start::where_to_play.
  ///
  /// @param player The user who would play.
  /// @param console The console; must be the object @p consoles holds now.
  /// @param stop Cancels the lookups.
  /// @return Route::home or Route::internet; Unreachable::forgotten when the
  ///     console is no longer known; StartRefusal::cancelled when @p stop
  ///     fired.
  [[nodiscard]] auto route_for(users::PsUser& player,
                               consoles::Console& console,
                               const platform::StopToken& stop)
      -> Expected<consoles::Route, StartRefusal> override;
  /// @brief Decides whether a console session runs at home or over the
  /// internet.
  ///
  /// An explicit route is taken as asked. Otherwise a player without a PSN
  /// account, or a forgotten console, plays at home. A console not paired with
  /// the player plays at home when it is nearby, and otherwise over the
  /// internet only when it is found on the player's account. A paired console
  /// plays at home when ready or resting, over the internet when known not to
  /// be nearby, and when its presence is unknown it is checked on the network
  /// first. Blocks on that check and on the account lookup.
  ///
  /// @param player The user who would play.
  /// @param console The console.
  /// @param route The route asked for.
  /// @param over The network the presence check goes through; a home
  ///     stream's own, or the platform's.
  /// @param stop Cancels the check and the lookup.
  /// @return The way; StartRefusal::cancelled when @p stop fired.
  [[nodiscard]] auto where_to_play(users::PsUser& player,
                                   consoles::Console& console,
                                   consoles::Route route,
                                   const consoles::HomeNetwork& over,
                                   const platform::StopToken& stop)
      -> Expected<Way, StartRefusal>;

  /// @brief Starts a session with a console on the home network.
  ///
  /// Does not block on the network; connecting happens on the session thread,
  /// with StartPolicy::home_attempts attempts.
  ///
  /// @param player The user to play as.
  /// @param console The console to play on.
  /// @param preferences Video, audio and input preferences.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @return The begun session. StartRefusal::invalid_preferences without a
  ///     console or player, with preferences the console cannot take, or when
  ///     the session cannot be opened; a refusal from StartPolicy::refusal when
  ///     another session is live or a pairing runs; the console's pairing
  ///     refusal when it is not paired for home play;
  ///     Unreachable::not_available when the platform has no crypto provider;
  ///     StartRefusal::host_refused when the session thread cannot start.
  [[nodiscard]] auto on_console(std::shared_ptr<users::PsUser> player,
                                std::shared_ptr<consoles::Console> console,
                                const Preferences& preferences,
                                media::MediaSink& media, SessionEvents& events)
      -> Started;
  /// @brief Starts a session with a console over the internet, looking the
  /// console up on the player's PSN account.
  ///
  /// @param player The user to play as.
  /// @param console The console to play on.
  /// @param preferences Video, audio and input preferences.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @param cancel Cancels the account lookup.
  /// @return As the private overload with nothing found yet.
  [[nodiscard]] auto over_internet(std::shared_ptr<users::PsUser> player,
                                   std::shared_ptr<consoles::Console> console,
                                   const Preferences& preferences,
                                   media::MediaSink& media,
                                   SessionEvents& events,
                                   const platform::StopToken& cancel)
      -> Started;
  /// @brief Starts a cloud stream of one title.
  ///
  /// Builds the cloud link and its launch without blocking; the title is
  /// cleared and the server reserved later, on the session thread.
  ///
  /// @param player The user to play as.
  /// @param title The title to stream.
  /// @param preferences Video, audio and input preferences.
  /// @param datacenter Which datacenter to use, or how to choose one.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @param cleared What an earlier catalog check cleared for launch, so the
  ///     launch does not check again; empty to check during the launch.
  /// @return The begun session. A refusal from StartPolicy::refusal;
  ///     StartRefusal::needs_psn_sign_in when the player's credential cannot be
  ///     used; StartRefusal::invalid_preferences for preferences the cloud
  ///     cannot take or when the session cannot be opened; a launch_refused
  ///     refusal when no launch could be built or a launch precondition is
  ///     unmet; StartRefusal::host_refused when the session thread cannot
  ///     start.
  [[nodiscard]] auto on_cloud(std::shared_ptr<users::PsUser> player,
                              const catalog::ProductId& title,
                              const Preferences& preferences,
                              const DatacenterPreference& datacenter,
                              media::MediaSink& media, SessionEvents& events,
                              std::optional<catalog::ClearedLaunch> cleared)
      -> Started;
  /// @brief Starts a session over a scripted link, for tests.
  ///
  /// @param player The user to play as.
  /// @param where Where the scripted session pretends to run.
  /// @param link The scripted link; must outlive the session.
  /// @param preferences Video, audio and input preferences.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @return The begun session, or a refusal from StartPolicy::refusal, or
  ///     StartRefusal::invalid_preferences when the session cannot be opened,
  ///     or StartRefusal::host_refused when the session thread cannot start.
  [[nodiscard]] auto scripted(std::shared_ptr<users::PsUser> player,
                              Where where, testing::ScriptedLink& link,
                              const Preferences& preferences,
                              media::MediaSink& media, SessionEvents& events)
      -> Started;

 private:
  /// @brief Routes an unpaired, distant console: over the internet when it is
  /// on the player's PSN account, otherwise home.
  ///
  /// Blocks on the account lookup.
  ///
  /// @param player The user who would play.
  /// @param console The console.
  /// @param stop Cancels the lookup.
  /// @return The way, with the found target for the internet;
  ///     StartRefusal::cancelled when @p stop fired.
  [[nodiscard]] auto without_home_pairing(users::PsUser& player,
                                          const consoles::Console& console,
                                          const platform::StopToken& stop)
      -> Expected<Way, StartRefusal>;
  /// @brief Starts a session over the internet with the target already found,
  /// or looked up now.
  ///
  /// Blocks on the account lookup when @p found is empty.
  ///
  /// @param player The user to play as.
  /// @param console The console to play on.
  /// @param preferences Video, audio and input preferences.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @param cancel Cancels the lookup.
  /// @param found The console as found on the account, when already known.
  /// @return The begun session. StartRefusal::invalid_preferences without a
  ///     console or player, with preferences the console cannot take, or when
  ///     the session cannot be opened; a refusal from StartPolicy::refusal;
  ///     StartRefusal::cancelled when @p cancel fired; the account's refusal
  ///     when the console is not found; Unreachable::not_available when the
  ///     platform lacks crypto, HTTP or websocket providers;
  ///     StartRefusal::host_refused when the session thread cannot start.
  [[nodiscard]] auto over_internet(
      std::shared_ptr<users::PsUser> player,
      std::shared_ptr<consoles::Console> console,
      const Preferences& preferences, media::MediaSink& media,
      SessionEvents& events, const platform::StopToken& cancel,
      std::optional<consoles::PairedPlay::InternetTarget> found) -> Started;
  /// @brief Holds the one-start-at-a-time lock and asks StartPolicy whether the
  /// start may begin.
  ///
  /// Blocks until any other start in progress releases the lock.
  ///
  /// @param where Where the stream would run.
  /// @param player The user who wants to start.
  /// @return The held lock, kept for the rest of the start;
  ///     StartRefusal::invalid_preferences without a player, or the policy's
  ///     refusal.
  [[nodiscard]] auto admit(Where where,
                           const std::shared_ptr<users::PsUser>& player)
      -> Expected<platform::Lock, StartRefusal>;
  /// @brief Builds the opener a ReconnectingLink calls for each home attempt.
  ///
  /// @param player The user to play as.
  /// @param console The console to play on.
  /// @param video The resolved video request.
  /// @param wired The console preferences with the share tap set.
  /// @param stop Stops the attempt's link.
  /// @param platform The platform every attempt's link runs on; must outlive
  ///     the opener and every link it opens.
  /// @return An opener that configures and opens a fresh home link, or fails
  ///     with StreamError::invalid_configuration.
  static auto home_link(std::shared_ptr<users::PsUser> player,
                        std::shared_ptr<consoles::Console> console,
                        VideoRequest video, ConsolePreferences wired,
                        platform::StopSource stop,
                        platform::PlatformFactory& platform)
      -> ReconnectingLink::Opener;
  /// @brief Builds the opener a ReconnectingLink calls for each internet
  /// attempt.
  ///
  /// @param player The user to play as.
  /// @param reached The console as found on the account.
  /// @param found The resolved console preferences.
  /// @param wired The console preferences with the share tap set.
  /// @param stop Stops the attempt's link.
  /// @param nat_probe_sockets How many sockets to probe the NAT with.
  /// @param pairings Told when the start pairs the console; may be null.
  /// @return An opener that configures and opens a fresh internet link, or
  ///     fails with StreamError::invalid_configuration.
  auto internet_link(std::shared_ptr<users::PsUser> player,
                     consoles::PairedPlay::InternetTarget reached,
                     LinkPreferences::OnConsole found, ConsolePreferences wired,
                     platform::StopSource stop, std::size_t nat_probe_sockets,
                     InternetStart::PairingListener* pairings)
      -> ReconnectingLink::Opener;
  /// @brief Asks which streaming service will carry a title.
  ///
  /// @param player The user who wants to play.
  /// @param title The title.
  /// @return The service the clearance names.
  auto expected_service(users::PsUser& player, const catalog::ProductId& title)
      -> catalog::ServiceGeneration;
  /// @brief Builds the cloud link request from the resolved preferences.
  ///
  /// @param player The user to play as.
  /// @param title The title.
  /// @param resolved The cloud preferences.
  /// @param datacenter The datacenter preference.
  /// @param cleared What an earlier check cleared, if any.
  /// @return The request.
  auto cloud_request(std::shared_ptr<users::PsUser> player,
                     const catalog::ProductId& title,
                     const LinkPreferences::OnCloud& resolved,
                     const DatacenterPreference& datacenter,
                     std::optional<catalog::ClearedLaunch> cleared)
      -> CloudLinkRequest;
  /// @brief Collects what Session::Core::open needs.
  ///
  /// @param where Where the session runs.
  /// @param player The user playing.
  /// @param console The console; null for the cloud.
  /// @param service The cloud service; empty for consoles.
  /// @param link The link the session drives.
  /// @param stop Stops the link.
  /// @param media Receives video and audio.
  /// @param events Receives the session's events.
  /// @param preferences The player's preferences.
  /// @param share Lets other players join the session.
  /// @return The opening.
  auto opening(Where where, std::shared_ptr<users::PsUser> player,
               std::shared_ptr<consoles::Console> console,
               std::optional<catalog::ServiceGeneration> service,
               std::unique_ptr<Link> link, platform::StopSource stop,
               media::MediaSink& media, SessionEvents& events,
               const Preferences& preferences,
               std::shared_ptr<SessionShare::Core> share)
      -> Session::Core::Opening;
  /// @brief Opens the session, seats it in @p playing, begins it and holds it
  /// in the live slot.
  ///
  /// @param playing The holder of the new session.
  /// @param opening What the session opens with.
  /// @param launch The cloud launch; null for consoles.
  /// @return The session sharing ownership of @p playing;
  ///     StartRefusal::invalid_preferences when the session cannot be opened;
  ///     StartRefusal::host_refused when its thread cannot start, in which
  ///     case the session is not held.
  [[nodiscard]] auto seated(std::shared_ptr<LivePlay> playing,
                            Session::Core::Opening opening,
                            CloudLaunch::Core* launch) -> Started;

  /// @brief Points the console preferences' source tap at the session share.
  ///
  /// @param preferences The resolved console preferences.
  /// @param share The session share.
  /// @return The preferences with the tap set.
  static auto tapped(ConsolePreferences preferences, SessionShare::Core& share)
      -> ConsolePreferences;
  /// @brief Turns a pairing refusal into a start refusal.
  ///
  /// @param refusal Why the console cannot be played as the player.
  /// @param player The player, whose sign-in decides an unanswered account.
  /// @return Unreachable::forgotten or Unreachable::no_address; needs PSN
  ///     sign-in; Remote Play disabled; sign-in expired, or
  ///     Unreachable::network, when the account did not answer; otherwise
  ///     StartRefusal::not_paired.
  static auto refusal_of(consoles::PairedPlay::Refusal refusal,
                         const users::PsUser& player) -> StartRefusal;
  /// @brief Names the controller the session starts with.
  ///
  /// @param controls The controls preference.
  /// @return The matching controller kind; DualSense by default.
  static auto first_controller(const ControlsRequest& controls) noexcept
      -> controllers::ControllerKind;

  /// @brief The host's providers.
  platform::PlatformFactory& platform_;
  /// @brief The known consoles.
  consoles::KnownConsoles& consoles_;
  /// @brief Clears cloud titles.
  catalog::Catalog& catalog_;
  /// @brief Where measured datacenters are kept.
  DatacenterStore& store_;
  /// @brief The datacenter view given to cloud launches.
  Datacenters& datacenters_;
  /// @brief Tells whether a pairing is in progress.
  InternetPairer& pairer_;
  /// @brief The slot holding the one live session.
  LiveSession& live_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief What every start clears titles, waits and keeps time with.
  StartCollaborators collaborators_;
};

}  // namespace libchiaking::play::usecases
