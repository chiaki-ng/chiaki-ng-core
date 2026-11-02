#include "src/core/play/usecases/start.hpp"

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/play/start_refusal.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"
#include "src/core/play/adapters/links/scripted_link_core.hpp"
#include "src/core/play/adapters/session/session_share_core.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "src/core/play/model/start_policy.hpp"

#include <utility>

namespace libchiaking::consoles {

auto Console::start(std::shared_ptr<users::PsUser> player,
                    const play::Preferences& preferences,
                    media::MediaSink& media, play::SessionEvents& events,
                    Route route, const platform::StopToken& stop)
    -> Expected<std::shared_ptr<play::Session>, play::StartRefusal> {
  auto* const starting = this->starting();
  if (starting == nullptr)
    return unexpected(
        play::StartRefusal::console_unreachable(Unreachable::not_available));
  return starting->start(std::move(player), *this, preferences, media, events,
                         route, stop);
}

auto Console::route_for(users::PsUser& player, const platform::StopToken& stop)
    -> Expected<Route, play::StartRefusal> {
  auto* const starting = this->starting();
  if (starting == nullptr)
    return unexpected(
        play::StartRefusal::console_unreachable(Unreachable::not_available));
  return starting->route_for(player, *this, stop);
}

}  // namespace libchiaking::consoles

namespace libchiaking::play::usecases {

Start::Start(platform::PlatformFactory& platform,
             consoles::KnownConsoles& consoles, catalog::Catalog& catalog,
             DatacenterStore& store, Datacenters& datacenters,
             InternetPairer& pairer, LiveSession& live,
             platform::DiagnosticsProvider* log,
             StartCollaborators collaborators)
    : platform_(platform),
      consoles_(consoles),
      catalog_(catalog),
      store_(store),
      datacenters_(datacenters),
      pairer_(pairer),
      live_(live),
      log_(log),
      collaborators_(std::move(collaborators)) {}

auto Start::start(std::shared_ptr<users::PsUser> player,
                  consoles::Console& console, const Preferences& preferences,
                  media::MediaSink& media, SessionEvents& events,
                  consoles::Route route, const platform::StopToken& stop)
    -> Started {
  if (!player) return unexpected(StartRefusal::invalid_preferences());
  auto known = consoles_.find(console.hardware_id());
  if (known.get() != &console)
    return unexpected(
        StartRefusal::console_unreachable(consoles::Unreachable::forgotten));
  const auto where =
      where_to_play(*player, console, route, preferences.home, stop);
  if (!where) return unexpected(where.error());
  if (where.value().where == Where::internet)
    return over_internet(std::move(player), std::move(known), preferences,
                         media, events, stop, std::move(where.value().found));
  return on_console(std::move(player), std::move(known), preferences, media,
                    events);
}

auto Start::route_for(users::PsUser& player, consoles::Console& console,
                      const platform::StopToken& stop)
    -> Expected<consoles::Route, StartRefusal> {
  if (consoles_.find(console.hardware_id()).get() != &console)
    return unexpected(
        StartRefusal::console_unreachable(consoles::Unreachable::forgotten));
  const auto way =
      where_to_play(player, console, consoles::Route::automatic, {}, stop);
  if (!way) return unexpected(way.error());
  return way.value().where == Where::internet ? consoles::Route::internet
                                              : consoles::Route::home;
}

auto Start::where_to_play(users::PsUser& player, consoles::Console& console,
                          consoles::Route route,
                          const consoles::HomeNetwork& over,
                          const platform::StopToken& stop)
    -> Expected<Way, StartRefusal> {
  using consoles::Presence;
  if (route == consoles::Route::home) return Way{Where::home, std::nullopt};
  if (route == consoles::Route::internet)
    return Way{Where::internet, std::nullopt};
  if (!player.has_psn_account() || console.is_forgotten())
    return Way{Where::home, std::nullopt};
  const auto presence = console.presence();
  const bool nearby =
      presence == Presence::ready || presence == Presence::rest_mode;
  if (!console.is_paired_with(player))
    return nearby ? Way{Where::home, std::nullopt}
                  : without_home_pairing(player, console, stop);
  switch (presence) {
    case Presence::ready:
    case Presence::rest_mode:
      return Way{Where::home, std::nullopt};
    case Presence::not_nearby:
      return Way{Where::internet, std::nullopt};
    case Presence::unknown:
      break;
  }
  if (stop.stop_requested()) return unexpected(StartRefusal::cancelled());
  const auto seen = console.check(stop, over);
  if (!seen && seen.error() == consoles::Unreachable::cancelled)
    return unexpected(StartRefusal::cancelled());
  const bool at_home = seen && (seen.value() == Presence::ready ||
                                seen.value() == Presence::rest_mode);
  return Way{at_home ? Where::home : Where::internet, std::nullopt};
}

auto Start::without_home_pairing(users::PsUser& player,
                                 const consoles::Console& console,
                                 const platform::StopToken& stop)
    -> Expected<Way, StartRefusal> {
  if (stop.stop_requested()) return unexpected(StartRefusal::cancelled());
  auto on_account =
      consoles::PairedPlay::found_on_account(consoles_, console, player, stop);
  if (stop.stop_requested()) return unexpected(StartRefusal::cancelled());
  if (!on_account) return Way{Where::home, std::nullopt};
  return Way{Where::internet, std::move(on_account.value())};
}

auto Start::on_console(std::shared_ptr<users::PsUser> player,
                       std::shared_ptr<consoles::Console> console,
                       const Preferences& preferences, media::MediaSink& media,
                       SessionEvents& events) -> Started {
  if (!console) return unexpected(StartRefusal::invalid_preferences());
  const auto one_start = admit(Where::home, player);
  if (!one_start) return unexpected(one_start.error());
  const auto resolved =
      LinkPreferences::on_console(preferences, console->generation());
  if (!resolved) return unexpected(StartRefusal::invalid_preferences());
  if (const auto target = consoles::PairedPlay{*console, *player}.at_home();
      !target)
    return unexpected(refusal_of(target.error(), *player));

  auto playing = std::make_shared<LivePlay>(consoles_, player, console, log_);
  auto& stream_platform =
      playing->streams_over(platform_, preferences.home.network());
  auto share =
      SessionShare::Core::create(platform_.threading(), platform_.clock(),
                                 {Where::home, resolved->video.fps()});
  const auto wired = tapped(resolved->console, *share);
  platform::StopSource stop;
  auto opener =
      home_link(player, console, resolved->video, wired, stop, stream_platform);
  if (!HomeLinkConfiguration::create(*console, *player, resolved->video, wired))
    return unexpected(StartRefusal::invalid_preferences());
  if (!platform_.supplies(platform::PlatformCapability::crypto))
    return unexpected(StartRefusal::console_unreachable(
        consoles::Unreachable::not_available));
  auto link = std::make_unique<ReconnectingLink>(
      Where::home, std::move(opener), stop, platform_.clock(),
      platform_.threading(), playing->link_watcher(), preferences.handover,
      preferences.reconnect_after_drop);
  return seated(std::move(playing),
                opening(Where::home, std::move(player), std::move(console),
                        std::nullopt, std::move(link), stop, media, events,
                        preferences, std::move(share)),
                nullptr);
}

auto Start::over_internet(std::shared_ptr<users::PsUser> player,
                          std::shared_ptr<consoles::Console> console,
                          const Preferences& preferences,
                          media::MediaSink& media, SessionEvents& events,
                          const platform::StopToken& cancel) -> Started {
  return over_internet(std::move(player), std::move(console), preferences,
                       media, events, cancel, std::nullopt);
}

auto Start::over_internet(
    std::shared_ptr<users::PsUser> player,
    std::shared_ptr<consoles::Console> console, const Preferences& preferences,
    media::MediaSink& media, SessionEvents& events,
    const platform::StopToken& cancel,
    std::optional<consoles::PairedPlay::InternetTarget> found) -> Started {
  if (!console) return unexpected(StartRefusal::invalid_preferences());
  const auto one_start = admit(Where::internet, player);
  if (!one_start) return unexpected(one_start.error());
  const auto resolved =
      LinkPreferences::on_console(preferences, console->generation());
  if (!resolved) return unexpected(StartRefusal::invalid_preferences());
  if (cancel.stop_requested()) return unexpected(StartRefusal::cancelled());
  const auto target =
      found ? Expected<consoles::PairedPlay::InternetTarget,
                       consoles::PairedPlay::Refusal>{std::move(*found)}
            : consoles::PairedPlay::found_on_account(consoles_, *console,
                                                     *player, cancel);
  if (cancel.stop_requested()) return unexpected(StartRefusal::cancelled());
  if (!target) return unexpected(refusal_of(target.error(), *player));

  auto playing = std::make_shared<LivePlay>(consoles_, player, console, log_);
  playing->reached_as(target.value().device_uid);
  auto share =
      SessionShare::Core::create(platform_.threading(), platform_.clock(),
                                 {Where::internet, resolved->video.fps()});
  const auto wired = tapped(resolved->console, *share);
  platform::StopSource stop;
  auto opener = internet_link(player, target.value(), *resolved, wired, stop,
                              preferences.developer.nat_probe_sockets(),
                              playing->pairings());
  if (!InternetLinks::configured(target.value(), player, resolved->video, wired,
                                 resolved->port_guessing,
                                 resolved->start_timeout))
    return unexpected(StartRefusal::invalid_preferences());
  if (!platform_.supplies(platform::PlatformCapability::crypto |
                          platform::PlatformCapability::http |
                          platform::PlatformCapability::websocket))
    return unexpected(StartRefusal::console_unreachable(
        consoles::Unreachable::not_available));
  auto link = std::make_unique<ReconnectingLink>(
      Where::internet, std::move(opener), stop, platform_.clock(),
      platform_.threading(), playing->link_watcher(), preferences.handover,
      preferences.reconnect_after_drop);
  return seated(std::move(playing),
                opening(Where::internet, std::move(player), std::move(console),
                        std::nullopt, std::move(link), stop, media, events,
                        preferences, std::move(share)),
                nullptr);
}

auto Start::on_cloud(std::shared_ptr<users::PsUser> player,
                     const catalog::ProductId& title,
                     const Preferences& preferences,
                     const DatacenterPreference& datacenter,
                     media::MediaSink& media, SessionEvents& events,
                     std::optional<catalog::ClearedLaunch> cleared) -> Started {
  const auto one_start = admit(Where::cloud, player);
  if (!one_start) return unexpected(one_start.error());
  const auto resolved =
      LinkPreferences::on_cloud(preferences, expected_service(*player, title));
  if (!resolved) return unexpected(StartRefusal::invalid_preferences());
  auto configuration = CloudLinkConfiguration::create(
      cloud_request(player, title, *resolved, datacenter, std::move(cleared)),
      collaborators_.time_zone);
  if (!configuration)
    return unexpected(configuration.error() ==
                              ConfigurationError::invalid_credential
                          ? StartRefusal::needs_psn_sign_in()
                          : StartRefusal::invalid_preferences());
  platform::StopSource stop;
  configuration.value().stop = stop.token();
  auto link = CloudLinkConfiguration::link(
      std::move(configuration.value()), platform_,
      CloudLaunchSeams{&collaborators_.clearance, &collaborators_.waiter,
                       nullptr, &datacenters_, &collaborators_.time_zone});
  auto* launch = link ? CloudProvisioning::launch_of(*link.value()) : nullptr;
  if (launch == nullptr)
    return unexpected(
        StartRefusal::launch_refused(LaunchRefused::unavailable()));
  if (const auto unmet = launch->precondition())
    return unexpected(StartRefusal::launch_refused(
        LaunchFailure::precondition(*unmet).refused()));

  auto playing = std::make_shared<LivePlay>(consoles_, player, nullptr, log_);
  auto share =
      SessionShare::Core::create(platform_.threading(), platform_.clock(),
                                 {Where::cloud, resolved->video.fps()});
  return seated(std::move(playing),
                opening(Where::cloud, std::move(player), nullptr,
                        launch->service(), std::move(link.value()), stop, media,
                        events, preferences, std::move(share)),
                launch);
}

auto Start::scripted(std::shared_ptr<users::PsUser> player, Where where,
                     testing::ScriptedLink& link,
                     const Preferences& preferences, media::MediaSink& media,
                     SessionEvents& events) -> Started {
  const auto one_start = admit(where, player);
  if (!one_start) return unexpected(one_start.error());
  auto playing = std::make_shared<LivePlay>(consoles_, player, nullptr, log_);
  playing->woken_by(link.core().waker());
  if (where == Where::cloud) link.core().report_launch_to(events);
  auto share =
      SessionShare::Core::create(platform_.threading(), platform_.clock(),
                                 {where, preferences.picture.fps()});
  link.core().share_through(
      share->tap(), platform_.supplies(platform::PlatformCapability::crypto)
                        ? &platform_.crypto()
                        : nullptr);
  platform::StopSource stop;
  auto wire =
      where == Where::cloud
          ? link.core().link(stop, where)
          : std::make_unique<ReconnectingLink>(
                where, link.core().opener(stop, where), stop, platform_.clock(),
                platform_.threading(), playing->link_watcher());
  auto session =
      seated(std::move(playing),
             opening(where, std::move(player), nullptr,
                     where == Where::cloud
                         ? std::optional{catalog::ServiceGeneration::ps5_cloud}
                         : std::nullopt,
                     std::move(wire), stop, media, events, preferences,
                     std::move(share)),
             nullptr);
  if (session) link.core().settle();
  return session;
}

auto Start::admit(Where where, const std::shared_ptr<users::PsUser>& player)
    -> Expected<platform::Lock, StartRefusal> {
  if (!player) return unexpected(StartRefusal::invalid_preferences());
  auto one_start = live_.one_start_at_a_time();
  if (auto refused = StartPolicy::refusal(
          where, StartPolicy::standing_of(*player, live_.busy(),
                                          pairer_.core().is_pairing())))
    return unexpected(std::move(*refused));
  return one_start;
}

auto Start::tapped(ConsolePreferences preferences, SessionShare::Core& share)
    -> ConsolePreferences {
  preferences.tuning.source_tap = &share.tap();
  return preferences;
}

auto Start::home_link(std::shared_ptr<users::PsUser> player,
                      std::shared_ptr<consoles::Console> console,
                      VideoRequest video, ConsolePreferences wired,
                      platform::StopSource stop,
                      platform::PlatformFactory& platform)
    -> ReconnectingLink::Opener {
  return
      [player = std::move(player), console = std::move(console), video, wired,
       stop, &platform]() -> Expected<std::unique_ptr<Link>,
                                      net::protocol::remote_play::StreamError> {
        auto configuration =
            HomeLinkConfiguration::create(*console, *player, video, wired);
        if (!configuration)
          return unexpected(
              net::protocol::remote_play::StreamError::invalid_configuration);
        configuration.value().stop = stop.token();
        return HomeLinkConfiguration::link(std::move(configuration.value()),
                                           platform);
      };
}

auto Start::internet_link(std::shared_ptr<users::PsUser> player,
                          consoles::PairedPlay::InternetTarget reached,
                          LinkPreferences::OnConsole found,
                          ConsolePreferences wired, platform::StopSource stop,
                          std::size_t nat_probe_sockets,
                          InternetStart::PairingListener* pairings)
    -> ReconnectingLink::Opener {
  return [this, player = std::move(player), reached = std::move(reached),
          found = std::move(found), wired, stop, nat_probe_sockets,
          pairings]() -> Expected<std::unique_ptr<Link>,
                                  net::protocol::remote_play::StreamError> {
    auto configuration =
        InternetLinks::configured(reached, player, found.video, wired,
                                  found.port_guessing, found.start_timeout);
    if (!configuration)
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_configuration);
    configuration.value().nat_probe_socket_count = nat_probe_sockets;
    configuration.value().stop = stop.token();
    return InternetLinks::playing(std::move(configuration.value()), platform_,
                                  pairings);
  };
}

auto Start::expected_service(users::PsUser& player,
                             const catalog::ProductId& title)
    -> catalog::ServiceGeneration {
  return CloudLaunch::Core::expected_service(&collaborators_.clearance,
                                             &player, title);
}

auto Start::cloud_request(std::shared_ptr<users::PsUser> player,
                          const catalog::ProductId& title,
                          const LinkPreferences::OnCloud& resolved,
                          const DatacenterPreference& datacenter,
                          std::optional<catalog::ClearedLaunch> cleared)
    -> CloudLinkRequest {
  CloudLinkRequest launch{std::move(player), catalog_, title};
  launch.cleared = std::move(cleared);
  launch.datacenters = &store_;
  launch.video = resolved.video;
  launch.measure_bandwidth = resolved.measure_bandwidth;
  launch.controller = resolved.controller;
  launch.feedback = resolved.feedback;
  launch.idr_on_fec_failure = resolved.idr_on_fec_failure;
  launch.tuning = resolved.tuning;
  launch.accessibility = resolved.accessibility;
  launch.quality = resolved.quality;
  launch.datacenter = datacenter;
  return launch;
}

auto Start::opening(Where where, std::shared_ptr<users::PsUser> player,
                    std::shared_ptr<consoles::Console> console,
                    std::optional<catalog::ServiceGeneration> service,
                    std::unique_ptr<Link> link, platform::StopSource stop,
                    media::MediaSink& media, SessionEvents& events,
                    const Preferences& preferences,
                    std::shared_ptr<SessionShare::Core> share)
    -> Session::Core::Opening {
  auto settings = LinkPreferences::engine(preferences.output, collaborators_.settings);
  settings.commands.controller_speaker =
      service &&
      preferences.sound.on_cloud(*service).wants_controller_speaker();
  return Session::Core::Opening{
      .player = std::move(player),
      .console = std::move(console),
      .where = where,
      .service = service,
      .link = std::move(link),
      .stop = stop,
      .media = media,
      .events = events,
      .clock = platform_.clock(),
      .threading = platform_.threading(),
      .journal = live_.journal_for_session(),
      .first_controller = first_controller(preferences.controls),
      .settings = settings,
      .share = std::move(share),
      .controller_waking =
          platform_.network().capabilities().costly_socket_calls
              ? Controllers::Core::Waking::changed_samples
              : Controllers::Core::Waking::every_sample};
}

auto Start::seated(std::shared_ptr<LivePlay> playing,
                   Session::Core::Opening opening, CloudLaunch::Core* launch)
    -> Started {
  auto opened = Session::Core::open(std::move(opening));
  if (!opened) return unexpected(StartRefusal::invalid_preferences());
  auto& core = playing->seat(std::move(opened.value()), launch);
  if (!core.begin()) return unexpected(StartRefusal::host_refused());
  std::shared_ptr<Session> session{playing, &playing->session()};
  live_.hold(session, core);
  return session;
}

auto Start::refusal_of(consoles::PairedPlay::Refusal refusal,
                       const users::PsUser& player) -> StartRefusal {
  using Refusal = consoles::PairedPlay::Refusal;
  switch (refusal) {
    case Refusal::forgotten:
      return StartRefusal::console_unreachable(
          consoles::Unreachable::forgotten);
    case Refusal::no_address:
      return StartRefusal::console_unreachable(
          consoles::Unreachable::no_address);
    case Refusal::needs_psn_sign_in:
      return StartRefusal::needs_psn_sign_in();
    case Refusal::remote_play_disabled:
      return StartRefusal::remote_play_disabled();
    case Refusal::account_unanswered:
      return player.sign_in_state() == users::SignInState::expired
                 ? StartRefusal::sign_in_expired()
                 : StartRefusal::console_unreachable(
                       consoles::Unreachable::network);
    case Refusal::not_paired:
    case Refusal::not_on_account:
      break;
  }
  return StartRefusal::not_paired();
}

auto Start::first_controller(const ControlsRequest& controls) noexcept
    -> controllers::ControllerKind {
  switch (controls.controller()) {
    case ControllerChoice::console_default:
      return controllers::ControllerKind::console_default;
    case ControllerChoice::dualshock4:
      return controllers::ControllerKind::dualshock4;
    case ControllerChoice::dualsense:
      break;
  }
  return controllers::ControllerKind::dualsense;
}

}  // namespace libchiaking::play::usecases
