#include "src/core/consoles/usecases/invitations.hpp"

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/consoles/adapters/titles/title_start.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "src/core/users/server_time.hpp"
#include "src/core/utils/json/ps_fields.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/uuid.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace libchiaking::consoles::usecases {

Invitations::Invitations(platform::HttpProvider& http,
                         const platform::ClockProvider& clock,
                         platform::DiagnosticsProvider* log,
                         net::http::Waiter& waiter) noexcept
    : http_(http),
      clock_(clock),
      log_(log),
      waiter_(waiter) {}

auto Invitations::list(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<GameInvitation>, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  auto notifications = calls.notifications(http, language_of(user));
  if (!notifications) return unexpected(notifications.error());
  const auto now = users::ServerTime::of(user).now();
  std::vector<psn::rest::UserNotification> live;
  std::ranges::copy_if(notifications.value(), std::back_inserter(live),
                       [&](const psn::rest::UserNotification& notification) {
                         return is_invitation(notification, now);
                       });
  for (const auto& notification : notifications.value())
    if (!text::trim(notification.session_id).empty() &&
        !is_invitation(notification, now))
      http.log(platform::LogSeverity::info,
               "[INVITES] skipped " + notification.use_case_id + " " +
                   notification.state + " title " + notification.title_id);
  name_senders(calls, http, live);
  for (const auto& notification : live)
    http.log(
        platform::LogSeverity::info,
        "[INVITES] " + notification.use_case_id + " " + notification.state +
            " title " + notification.title_id +
            (notification.invitation_id.empty() ? " without" : " with") +
            " an invitation id and a " +
            (text::Uuid::parse(text::trim(notification.session_id))
                 ? std::string{"UUID"}
                 : std::to_string(text::trim(notification.session_id).size()) +
                       "-character") +
            " session id");
  auto sessions = read_sessions(calls, http, live, language_of(user));
  if (!sessions) return unexpected(sessions.error());
  auto matching =
      matched(live, sessions.value().read, user.account_id().decimal(), now);
  const auto invitations = matching.size();
  auto found = current(std::move(matching), sessions.value());
  http.log(platform::LogSeverity::info,
           "[INVITES] " + std::to_string(notifications.value().size()) +
               " notifications -> " + std::to_string(invitations) +
               " invitations, " + std::to_string(sessions.value().read.size()) +
               " of " + std::to_string(sessions.value().asked.size()) +
               " sessions open, " + std::to_string(found.size()) + " listed");
  return found;
}

auto Invitations::read_sessions(
    users::ConsoleCalls& calls, net::http::Client& http,
    std::span<const psn::rest::UserNotification> notifications,
    std::string_view language) -> Expected<SessionsRead, users::Unanswered> {
  SessionsRead sessions;
  for (const auto& notification : notifications) {
    const auto id = text::trim(notification.session_id);
    if (text::trim(notification.use_case_id) == party_use_case ||
        !psn::rest::SessionId::parse(id) ||
        std::ranges::contains(sessions.asked, id))
      continue;
    sessions.asked.emplace_back(id);
  }
  for (std::size_t first = 0; first < sessions.asked.size();
       first += psn::rest::player_sessions_batch) {
    const auto batch = std::span{sessions.asked}.subspan(
        first, std::min(psn::rest::player_sessions_batch,
                        sessions.asked.size() - first));
    auto read = calls.player_sessions(http, batch, language);
    if (!read) {
      if (read.error().reason == users::Unanswered::Reason::cancelled)
        return unexpected(read.error());
      http.log(platform::LogSeverity::warning,
               "[INVITES] " + std::to_string(sessions.asked.size() - first) +
                   " sessions unread, so their invitations stay listed");
      sessions.asked.resize(first);
      break;
    }
    std::ranges::move(read.value(), std::back_inserter(sessions.read));
  }
  return sessions;
}

auto Invitations::current(std::vector<GameInvitation> invitations,
                          const SessionsRead& sessions)
    -> std::vector<GameInvitation> {
  std::erase_if(invitations, [&](const GameInvitation& invitation) {
    if (invitation.joining == GameInvitation::Joining::already_joined)
      return true;
    const auto same = [&](std::string_view id) {
      return text::ieq(id, invitation.session_id);
    };
    return std::ranges::any_of(sessions.asked, same) &&
           std::ranges::none_of(sessions.read, same,
                                &psn::rest::PlayerSession::session_id);
  });
  return invitations;
}

void Invitations::name_senders(
    users::ConsoleCalls& calls, net::http::Client& http,
    std::vector<psn::rest::UserNotification>& notifications) {
  std::map<std::string, std::string, std::less<>> named;
  for (auto& notification : notifications) {
    if (!text::trim(notification.from_online_id).empty()) continue;
    const auto account = AccountId::parse(notification.from_account_id);
    if (!account) continue;
    auto known = named.find(notification.from_account_id);
    if (known == named.end()) {
      auto profile = calls.profile(http, *account);
      known = named
                  .emplace(notification.from_account_id,
                           profile ? std::move(profile.value().online_id)
                                   : std::string{})
                  .first;
    }
    notification.from_online_id = known->second;
  }
}

auto Invitations::dismiss(users::PsUser& user,
                          std::span<const std::string> notification_ids,
                          const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  return users::ConsoleCalls{user}.set_notification_state(
      http, notification_ids, "Deleted");
}

auto Invitations::friends(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<PsnFriend>, users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  const auto language = language_of(user);
  std::vector<PsnFriend> found;
  std::optional<std::size_t> offset = 0;
  for (std::size_t page = 0; page < most_friend_pages && offset; ++page) {
    auto listed = calls.friends_page(http, *offset, language);
    if (!listed) return unexpected(listed.error());
    const auto& ids = listed.value().account_ids;
    offset = listed.value().next_offset;
    for (std::size_t first = 0; first < ids.size();
         first += psn::rest::Friends::page_size) {
      const auto batch = std::span{ids}.subspan(
          first, std::min(psn::rest::Friends::page_size, ids.size() - first));
      auto profiles = calls.friend_profiles(http, batch, language);
      if (!profiles && profiles.error().reason == Reason::cancelled)
        return unexpected(profiles.error());
      auto presences = calls.friend_presences(http, batch, language);
      if (!presences && presences.error().reason == Reason::cancelled)
        return unexpected(presences.error());
      for (std::size_t index = 0; index < batch.size(); ++index) {
        const auto account = AccountId::parse(batch[index]);
        if (!account) continue;
        PsnFriend friend_entry{.account_id = *account};
        if (profiles && index < profiles.value().size()) {
          friend_entry.online_id = profiles.value()[index].online_id;
          friend_entry.avatar_url = profiles.value()[index].avatar_url;
        }
        if (presences && index < presences.value().size()) {
          friend_entry.online = presences.value()[index].online;
          friend_entry.playing = presences.value()[index].playing;
        }
        found.push_back(std::move(friend_entry));
      }
    }
  }
  std::ranges::stable_partition(found, &PsnFriend::online);
  return found;
}

auto Invitations::own_game_session(users::PsUser& user, const NpTitleId& title,
                                   GamePlatform platform,
                                   const platform::StopToken& stop)
    -> Expected<std::optional<OwnGameSession>, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  const auto language = language_of(user);
  const auto device = platform_name(platform);
  auto joined = calls.joined_player_sessions(http, title.text(), device, device,
                                             language);
  if (!joined) return unexpected(joined.error());
  if (joined.value().empty()) return std::optional<OwnGameSession>{};
  const std::array<std::string, 1> wanted{joined.value().front()};
  auto sessions = calls.player_sessions(http, wanted, language);
  if (!sessions) return unexpected(sessions.error());
  OwnGameSession own{.session_id = wanted.front()};
  const auto session = std::ranges::find_if(
      sessions.value(), [&](const psn::rest::PlayerSession& candidate) {
        return text::ieq(candidate.session_id, own.session_id);
      });
  if (session != sessions.value().end()) {
    own.name = session->name;
    own.players = static_cast<std::uint32_t>(session->players.size() +
                                             session->other_players);
    own.max_players = session->max_players;
    own.open_to_anyone = session->open_to_anyone;
  }
  return std::optional<OwnGameSession>{std::move(own)};
}

auto Invitations::platform_name(GamePlatform platform) noexcept
    -> std::string_view {
  return platform == GamePlatform::ps4 ? "PS4" : "PS5";
}

auto Invitations::share_game_session(users::PsUser& user,
                                     std::string_view session_id,
                                     const platform::StopToken& stop)
    -> Expected<SessionLink, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  auto link = users::ConsoleCalls{user}.session_share_link(http, session_id,
                                                           language_of(user));
  if (!link) return unexpected(link.error());
  return SessionLink{std::move(link.value().url),
                     std::move(link.value().image_url)};
}

auto Invitations::invite_friends(users::PsUser& user,
                                 const InvitationTarget& target,
                                 std::span<const AccountId> accounts,
                                 const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  const auto language = language_of(user);
  const auto party = target.kind == InvitationTarget::Kind::party;
  for (std::size_t first = 0; first < accounts.size();
       first += psn::rest::SessionInvitations::most_invitees) {
    const auto batch = accounts.subspan(
        first, std::min(psn::rest::SessionInvitations::most_invitees,
                        accounts.size() - first));
    auto sent =
        party ? calls.invite_to_party(http, target.session_id, batch, language)
              : calls.invite_to_game(http, target.session_id, batch, language);
    if (!sent) return unexpected(sent.error());
  }
  http.log(platform::LogSeverity::info,
           std::string{"[INVITES] sent "} + std::to_string(accounts.size()) +
               (party ? " party" : " game") + " invitations");
  return {};
}

auto Invitations::join_in_cloud(users::PsUser& user,
                                std::string_view session_id,
                                StartWatcher* watcher,
                                const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  return join(user, session_id, std::nullopt, watcher, stop);
}

auto Invitations::join_on_console(users::PsUser& user,
                                  std::string_view session_id,
                                  const PartyTarget& target,
                                  StartWatcher* watcher,
                                  const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  if (text::trim(target.duid).empty())
    return unexpected(InvitationJoinFailure{
        InvitationJoinFailure::Reason::console_unavailable});
  return join(user, session_id, target, watcher, stop);
}

auto Invitations::join(users::PsUser& user, std::string_view session_id,
                       const std::optional<PartyTarget>& home,
                       StartWatcher* watcher, const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  using Reason = InvitationJoinFailure::Reason;
  if (!psn::rest::SessionId::parse(session_id))
    return unexpected(InvitationJoinFailure{Reason::invalid_session});
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  const auto at = [&](Stage stage) {
    http.log(platform::LogSeverity::info,
             "[INVITES] join " + std::string{name_of(stage)});
  };

  at(Stage::reading_session);
  const std::array<std::string, 1> wanted{std::string{session_id}};
  auto sessions = calls.player_sessions(http, wanted, language_of(user));
  if (!sessions) return unexpected(unanswered(sessions.error()));
  const auto session = std::ranges::find(sessions.value(), session_id,
                                         &psn::rest::PlayerSession::session_id);
  if (session == sessions.value().end())
    return unexpected(InvitationJoinFailure{Reason::session_ended});
  switch (joining(*session, user.account_id().decimal())) {
    case GameInvitation::Joining::already_joined:
      return unexpected(InvitationJoinFailure{Reason::already_joined});
    case GameInvitation::Joining::full:
      return unexpected(InvitationJoinFailure{Reason::full});
    case GameInvitation::Joining::cannot_join:
      return unexpected(InvitationJoinFailure{Reason::cannot_join});
    case GameInvitation::Joining::joinable:
      break;
  }

  std::string duid;
  std::string platform;
  if (home) {
    duid = std::string{text::trim(home->duid)};
    platform = std::string{text::trim(home->platform)};
  } else {
    at(Stage::finding_cloud_console);
    auto console = await_cloud_console(calls, http, waiter_, stop);
    if (!console) return unexpected(std::move(console.error()));
    duid = std::move(console->duid);
    platform = std::move(console->platform);
  }

  at(Stage::joining);
  auto joined = TitleStart{http_, waiter_, clock_, log_}.join(
      user, duid, platform, session->game_intent.view(), watcher, stop);
  if (!joined) return unexpected(failed(joined.error()));
  if (joined.value() == StartState::failed)
    return unexpected(InvitationJoinFailure{Reason::failed});
  return {};
}

auto Invitations::join_party(users::PsUser& user, std::string_view session_id,
                             const PartyTarget& target, StartWatcher* watcher,
                             const platform::StopToken& stop)
    -> Expected<void, InvitationJoinFailure> {
  using Reason = InvitationJoinFailure::Reason;
  if (!psn::rest::SessionId::parse(session_id))
    return unexpected(InvitationJoinFailure{Reason::invalid_session});
  const auto duid = text::trim(target.duid);
  if (duid.empty())
    return unexpected(InvitationJoinFailure{Reason::console_unavailable});
  net::http::Client http{http_, clock_, stop, log_};
  http.log(platform::LogSeverity::info,
           "[INVITES] party join " + std::string{name_of(Stage::joining)});
  const TitleStart::Pace pace{
      .deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
          party_deadline)};
  auto joined = TitleStart{http_, waiter_, clock_, log_, pace}.join_party(
      user, duid, text::trim(target.platform), session_id, watcher, stop);
  if (!joined) return unexpected(failed(joined.error()));
  if (joined.value() == StartState::failed)
    return unexpected(InvitationJoinFailure{Reason::failed});
  return {};
}

auto Invitations::find_cloud_console(users::ConsoleCalls& calls,
                                     net::http::Client& http)
    -> Expected<psn::rest::RegisteredConsole, InvitationJoinFailure> {
  for (std::size_t page = 0; page < most_console_pages; ++page) {
    auto listed = calls.cloud_consoles(
        http, page * psn::rest::list_clients_page_size, "PS5");
    if (!listed) return unexpected(unanswered(listed.error()));
    if (auto console = cloud_console(listed.value().consoles)) return *console;
    if (listed.value().received < psn::rest::list_clients_page_size) break;
  }
  return unexpected(
      InvitationJoinFailure{InvitationJoinFailure::Reason::no_cloud_console});
}

auto Invitations::await_cloud_console(users::ConsoleCalls& calls,
                                      net::http::Client& http,
                                      net::http::Waiter& waits,
                                      const platform::StopToken& stop)
    -> Expected<psn::rest::RegisteredConsole, InvitationJoinFailure> {
  using Reason = InvitationJoinFailure::Reason;
  const auto started = clock_.now_microseconds();
  for (int attempt = 1;; ++attempt) {
    auto console = find_cloud_console(calls, http);
    if (console || console.error().reason != Reason::no_cloud_console)
      return console;
    const auto spent = std::chrono::microseconds{
        static_cast<std::int64_t>(clock_.now_microseconds() - started)};
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cloud_console_deadline - spent);
    if (remaining.count() <= 0) return console;
    http.log(platform::LogSeverity::info,
             "[INVITES] join waiting for cloud console (attempt " +
                 std::to_string(attempt) + ")");
    const auto delay =
        std::min<std::chrono::milliseconds>(cloud_console_poll, remaining);
    if (!waits.wait_milliseconds(static_cast<int>(delay.count()), stop) ||
        stop.stop_requested())
      return unexpected(InvitationJoinFailure{Reason::cancelled});
  }
}

auto Invitations::unanswered(const users::Unanswered& why)
    -> InvitationJoinFailure {
  using Reason = InvitationJoinFailure::Reason;
  return InvitationJoinFailure{
      why.reason == users::Unanswered::Reason::cancelled ? Reason::cancelled
                                                         : Reason::unanswered,
      why};
}

auto Invitations::language_of(const users::PsUser& user) -> std::string {
  return user.account_context().locale().value_or(std::string{default_locale});
}

auto Invitations::is_invitation(const psn::rest::UserNotification& notification,
                                std::optional<GameInvitation::Time> now)
    -> bool {
  const auto use_case = text::trim(notification.use_case_id);
  if (use_case != party_use_case &&
      !std::ranges::contains(invitation_use_cases, use_case))
    return false;
  if (text::trim(notification.session_id).empty()) return false;
  if (text::ieq(notification.state, "Deleted")) return false;
  if (!now) return true;
  const auto expires = time_of(notification.expires);
  if (expires && *expires <= *now) return false;
  if (use_case == party_use_case ||
      psn::rest::SessionId::parse(text::trim(notification.session_id)))
    return true;
  const auto sent = time_of(notification.updated);
  return !sent || *now - *sent < old_session_lifetime;
}

auto Invitations::matched(
    std::span<const psn::rest::UserNotification> notifications,
    std::span<const psn::rest::PlayerSession> sessions,
    std::string_view account_id, std::optional<GameInvitation::Time> now)
    -> std::vector<GameInvitation> {
  std::vector<GameInvitation> found;
  for (const auto& notification : notifications) {
    if (!is_invitation(notification, now)) continue;
    const auto id = text::trim(notification.session_id);
    const auto listed =
        std::ranges::find(found, id, &GameInvitation::session_id);
    if (listed != found.end()) {
      if (!notification.notification_id.empty())
        listed->notification_ids.push_back(notification.notification_id);
      continue;
    }
    GameInvitation invitation;
    if (text::trim(notification.use_case_id) == party_use_case)
      invitation.kind = GameInvitation::Kind::party;
    invitation.session_id = std::string{id};
    if (!notification.notification_id.empty())
      invitation.notification_ids.push_back(notification.notification_id);
    invitation.invitation_id =
        std::string{text::trim(notification.invitation_id)};
    if (!invitation.invitation_id.empty() ||
        invitation.kind == GameInvitation::Kind::party)
      invitation.joining = GameInvitation::Joining::joinable;
    invitation.inviter = notification.from_online_id;
    invitation.sent =
        time_of(notification.updated).value_or(GameInvitation::Time{});
    invitation.unread = text::ieq(notification.state, "New");
    if (!text::trim(notification.title_id).empty())
      invitation.title_ids.push_back(notification.title_id);
    for (const auto& title : notification.title_ids)
      if (!std::ranges::contains(invitation.title_ids, title))
        invitation.title_ids.push_back(title);
    const auto session =
        std::ranges::find(sessions, id, &psn::rest::PlayerSession::session_id);
    if (session != sessions.end()) {
      invitation.session_name = session->name;
      invitation.players = static_cast<std::uint32_t>(session->players.size() +
                                                      session->other_players);
      invitation.max_players = session->max_players;
      invitation.joining = joining(*session, account_id);
      if (invitation.joining == GameInvitation::Joining::cannot_join &&
          !invitation.invitation_id.empty())
        invitation.joining = GameInvitation::Joining::joinable;
      for (const auto& title : session->title_ids)
        if (!std::ranges::contains(invitation.title_ids, title))
          invitation.title_ids.push_back(title);
      if (invitation.inviter.empty()) {
        const auto sender =
            std::ranges::find(session->players, notification.from_account_id,
                              &psn::rest::SessionPlayer::account_id);
        if (sender != session->players.end())
          invitation.inviter = sender->online_id;
      }
    }
    found.push_back(std::move(invitation));
  }
  return found;
}

auto Invitations::joining(const psn::rest::PlayerSession& session,
                          std::string_view account_id)
    -> GameInvitation::Joining {
  using Joining = GameInvitation::Joining;
  if (std::ranges::any_of(session.players,
                          [&](const psn::rest::SessionPlayer& player) {
                            return player.account_id == account_id;
                          }))
    return Joining::already_joined;
  if (session.game_intent.view().empty()) return Joining::cannot_join;
  const auto players = session.players.size() + session.other_players;
  if (session.max_players != 0 && players >= session.max_players)
    return Joining::full;
  return Joining::joinable;
}

auto Invitations::cloud_console(
    std::span<const psn::rest::RegisteredConsole> consoles)
    -> std::optional<psn::rest::RegisteredConsole> {
  const auto console =
      std::ranges::find_if(consoles, [](const psn::rest::RegisteredConsole& c) {
        return c.cloud_console && !text::trim(c.duid).empty();
      });
  if (console == consoles.end()) return std::nullopt;
  auto picked = *console;
  picked.duid = std::string{text::trim(picked.duid)};
  picked.platform = std::string{text::trim(picked.platform)};
  return picked;
}

auto Invitations::time_of(std::string_view text)
    -> std::optional<GameInvitation::Time> {
  const auto instant = json::ps::Instant::from_text(text::trim(text));
  if (!instant) return std::nullopt;
  return instant->time();
}

auto Invitations::failed(const StartFailure& why) noexcept
    -> InvitationJoinFailure {
  using Reason = InvitationJoinFailure::Reason;
  switch (why.reason) {
    case StartFailure::Reason::refused:
      return {Reason::refused, why.unanswered};
    case StartFailure::Reason::timed_out:
      return {Reason::timed_out, why.unanswered};
    case StartFailure::Reason::cancelled:
      return {Reason::cancelled, why.unanswered};
    case StartFailure::Reason::not_on_account:
    case StartFailure::Reason::ps5_only:
    case StartFailure::Reason::not_paired:
    case StartFailure::Reason::forgotten:
      return {Reason::console_unavailable, why.unanswered};
    case StartFailure::Reason::unanswered:
    case StartFailure::Reason::not_available:
      break;
  }
  return {Reason::unanswered, why.unanswered};
}

auto Invitations::name_of(Stage stage) noexcept -> std::string_view {
  switch (stage) {
    case Stage::reading_session:
      return "reading session";
    case Stage::finding_cloud_console:
      return "finding cloud console";
    case Stage::joining:
      return "joining";
  }
  return "unknown";
}

}  // namespace libchiaking::consoles::usecases
