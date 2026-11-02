#include "src/core/consoles/adapters/titles/title_start.hpp"
#include "src/core/consoles/known_consoles_core.hpp"
#include "src/core/consoles/usecases/invitations.hpp"
#include "src/core/consoles/usecases/push_alerts.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"

#include <utility>

namespace libchiaking::consoles {

KnownConsoles::Core::Core(
    KnownConsoles& face, platform::NetworkProvider& network,
    platform::HttpProvider& http, platform::CryptoProvider& crypto,
    platform::SecureRandomProvider& random, ConsoleStore& store,
    platform::ThreadingProvider& threading,
    const platform::ClockProvider& clock, InternetPairing* internet,
    platform::DiagnosticsProvider* log)
    : http_(http),
      threading_(threading),
      clock_(clock),
      log_(log),
      pause_(threading, clock_),
      roster_(*this, store, log),
      search_(network, threading, clock_, log),
      library_(face, http, threading, clock_, log),
      find_(network, threading, clock_, search_, roster_),
      pair_(network, http, crypto, random, threading, clock_, internet, roster_,
            log),
      wake_(network, threading, clock_, roster_) {}

KnownConsoles::KnownConsoles(
    platform::NetworkProvider& network, platform::HttpProvider& http,
    platform::CryptoProvider& crypto, platform::SecureRandomProvider& random,
    ConsoleStore& store, platform::ThreadingProvider& threading,
    const platform::ClockProvider& clock, InternetPairing* internet,
    platform::DiagnosticsProvider* log)
    : core_(std::make_unique<Core>(*this, network, http, crypto, random, store,
                                   threading, clock, internet, log)) {}

KnownConsoles::~KnownConsoles() = default;

void KnownConsoles::start_through(Starting* starting) noexcept {
  core_->start_through(starting);
}

auto KnownConsoles::remember_saved()
    -> Expected<Remembered, ConsoleStore::Error> {
  return core_->roster().remember_saved();
}

auto KnownConsoles::forget(Console& console)
    -> Expected<void, ConsoleStore::Error> {
  return core_->roster().forget(console);
}

auto KnownConsoles::unpair(AccountId account, Console& console)
    -> Expected<void, ConsoleStore::Error> {
  return core_->roster().unpair(account, console);
}

auto KnownConsoles::find(HardwareId hardware_id) const
    -> std::shared_ptr<Console> {
  return core_->roster().find(hardware_id);
}

auto KnownConsoles::everything() const
    -> std::vector<std::shared_ptr<Console>> {
  return core_->roster().everything();
}

auto KnownConsoles::find_on_my_network(const SearchRanges& ranges,
                                       NetworkWatcher& watcher,
                                       const platform::StopToken& stop)
    -> Expected<void, SearchFailure> {
  return core_->find().on_my_network(ranges, watcher, stop);
}

auto KnownConsoles::pair_at_home(const users::PsUser& as, Console& found,
                                 PairingPin pin,
                                 const platform::StopToken& stop,
                                 PairingWatcher* watcher)
    -> Expected<std::shared_ptr<Console>, PairingFailure> {
  return core_->pair().at_home(as, found, std::move(pin), stop, watcher);
}

auto KnownConsoles::pair_at_home(const users::PsUser& as, const Host& at,
                                 Generation generation, PairingPin pin,
                                 const platform::StopToken& stop,
                                 PairingWatcher* watcher)
    -> Expected<std::shared_ptr<Console>, PairingFailure> {
  return core_->pair().at_home(as, at, generation, std::move(pin), stop,
                               watcher);
}

auto KnownConsoles::consoles_on_account(users::PsUser& of,
                                        Generation generation,
                                        const platform::StopToken& stop)
    -> Expected<std::vector<AccountConsole>, users::Unanswered> {
  return core_->pair().on_account(of, generation, stop);
}

auto KnownConsoles::pair_over_the_internet(users::PsUser& as,
                                           const AccountConsole& which,
                                           PairingWatcher* watcher,
                                           const platform::StopToken& stop)
    -> Expected<std::shared_ptr<Console>, PairingFailure> {
  return core_->pair().over_the_internet(as, which, watcher, stop);
}

auto KnownConsoles::recently_played(users::PsUser& by,
                                    const platform::StopToken& stop)
    -> Expected<std::vector<RecentlyPlayed>, users::Unanswered> {
  return core_->library().recently_played(by, stop);
}

auto KnownConsoles::game_invitations(users::PsUser& of,
                                     const platform::StopToken& stop)
    -> Expected<std::vector<GameInvitation>, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .list(of, stop);
}

auto KnownConsoles::dismiss_invitations(
    users::PsUser& of, std::span<const std::string> notification_ids,
    const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .dismiss(of, notification_ids, stop);
}

auto KnownConsoles::friends(users::PsUser& of, const platform::StopToken& stop)
    -> Expected<std::vector<PsnFriend>, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .friends(of, stop);
}

auto KnownConsoles::own_game_session(users::PsUser& of, const NpTitleId& title,
                                     GamePlatform platform,
                                     const platform::StopToken& stop)
    -> Expected<std::optional<OwnGameSession>, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .own_game_session(of, title, platform, stop);
}

auto KnownConsoles::share_game_session(users::PsUser& of,
                                       std::string_view session_id,
                                       const platform::StopToken& stop)
    -> Expected<SessionLink, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .share_game_session(of, session_id, stop);
}

auto KnownConsoles::invite_friends(users::PsUser& of,
                                   const InvitationTarget& target,
                                   std::span<const AccountId> accounts,
                                   const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .invite_friends(of, target, accounts, stop);
}

auto KnownConsoles::watch_alerts(users::PsUser& of, AlertWatcher& watcher,
                                 const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  auto* websocket = core_->websocket();
  if (websocket == nullptr)
    return unexpected(
        users::Unanswered{users::Unanswered::Reason::unavailable});
  return usecases::PushAlerts{core_->http(),  *websocket,     core_->clock(),
                              core_->pause(), core_->log()}
      .run(of, watcher, stop);
}

void KnownConsoles::watch_through(
    platform::WebSocketProvider* websocket) noexcept {
  core_->watch_through(websocket);
}

auto KnownConsoles::join_in_cloud(users::PsUser& as,
                                  std::string_view session_id,
                                  StartWatcher* watcher,
                                  const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  return usecases::Invitations{core_->http(), core_->clock(), core_->log(),
                               core_->pause()}
      .join_in_cloud(as, session_id, watcher, stop);
}

auto KnownConsoles::join_on(Console& console, users::PsUser& as,
                            std::string_view session_id, StartWatcher* watcher,
                            const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  using Invitations = usecases::Invitations;
  if (!psn::rest::SessionId::parse(session_id))
    return unexpected(
        InvitationJoinFailure{InvitationJoinFailure::Reason::invalid_session});
  if (console.is_forgotten())
    return unexpected(InvitationJoinFailure{
        InvitationJoinFailure::Reason::console_unavailable});
  const auto shelf = core_->library().shelf_of(console, as, stop);
  if (!shelf)
    return unexpected(
        Invitations::failed(TitleStart::not_started(shelf.error())));
  return Invitations{core_->http(), core_->clock(), core_->log(),
                     core_->pause()}
      .join_on_console(as, session_id,
                       Invitations::PartyTarget{shelf.value().duid, "PS5"},
                       watcher, stop);
}

auto KnownConsoles::join_party_on(Console& console, users::PsUser& as,
                                  std::string_view session_id,
                                  StartWatcher* watcher,
                                  const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  using Invitations = usecases::Invitations;
  if (!psn::rest::SessionId::parse(session_id))
    return unexpected(
        InvitationJoinFailure{InvitationJoinFailure::Reason::invalid_session});
  if (console.is_forgotten())
    return unexpected(InvitationJoinFailure{
        InvitationJoinFailure::Reason::console_unavailable});
  const auto shelf = core_->library().shelf_of(console, as, stop);
  if (!shelf)
    return unexpected(
        Invitations::failed(TitleStart::not_started(shelf.error())));
  return Invitations{core_->http(), core_->clock(), core_->log(),
                     core_->pause()}
      .join_party(as, session_id,
                  Invitations::PartyTarget{shelf.value().duid, "PS5"}, watcher,
                  stop);
}

}  // namespace libchiaking::consoles
