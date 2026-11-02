#include "src/core/users/ports/console_calls.hpp"

#include "src/core/psn/rest/push/push.hpp"
#include "src/core/users/ports/authorised_request.hpp"

#include <utility>

namespace libchiaking::users {

ConsoleCalls::ConsoleCalls(PsUser& user) noexcept
    : user_(PsUser::Core::of(user)) {}

auto ConsoleCalls::played_titles(net::http::Client& http, std::size_t offset,
                                 std::size_t maximum_response_bytes)
    -> Expected<psn::rest::PlayedTitlesPage, Unanswered> {
  return request().played_titles(http, offset, maximum_response_bytes);
}

auto ConsoleCalls::request() const noexcept -> AuthorisedRequest {
  return AuthorisedRequest{user_};
}

auto ConsoleCalls::account_consoles(net::http::Client& http, std::size_t offset,
                                    std::size_t maximum_response_bytes)
    -> Expected<psn::rest::ClientsPage, Unanswered> {
  if (user_.off_psn())
    return unexpected(Unanswered{Unanswered::Reason::signed_out});
  return user_.in_flight().account_consoles.answer(
      AuthorisedRequest::question(maximum_response_bytes,
                                  std::to_string(offset)),
      http.stop_token(),
      [&] { return consoles_page(http, offset, maximum_response_bytes); });
}

auto ConsoleCalls::installed_titles(net::http::Client& http, std::size_t offset,
                                    std::size_t maximum_response_bytes)
    -> Expected<psn::rest::ClientsPage, Unanswered> {
  return request().asked_of_console_directory<psn::rest::ClientsPage>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .list_clients(offset, true);
      });
}

auto ConsoleCalls::notifications(net::http::Client& http,
                                 std::string_view language,
                                 std::size_t maximum_response_bytes)
    -> Expected<std::vector<psn::rest::UserNotification>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::UserNotification>>(
          http, [&](psn::auth::BearerToken token) {
            auto capped = http.limited(maximum_response_bytes);
            return psn::rest::UserNotifications{capped, std::move(token)}.list(
                language);
          });
}

auto ConsoleCalls::set_notification_state(
    net::http::Client& http, std::span<const std::string> notification_ids,
    std::string_view state) -> Expected<void, Unanswered> {
  auto changed = request().asked_of_console_directory<bool>(
      http,
      [&](psn::auth::BearerToken token)
          -> Expected<bool, psn::rest::UserNotifications::SetStateError> {
        auto done =
            psn::rest::UserNotifications{http, std::move(token)}.set_state(
                notification_ids, state);
        if (!done) return unexpected(std::move(done.error()));
        return true;
      });
  if (!changed) return unexpected(changed.error());
  return {};
}

auto ConsoleCalls::push_server(net::http::Client& http)
    -> Expected<PushServer, Unanswered> {
  std::optional<psn::auth::BearerToken> used;
  auto fqdn = request().asked_of_console_directory<std::string>(
      http, [&](psn::auth::BearerToken token) {
        used.emplace(token.clone());
        return psn::rest::Push{http, std::move(token)}.server_address();
      });
  if (!fqdn) return unexpected(fqdn.error());
  return PushServer{std::move(*used), std::move(fqdn.value())};
}

auto ConsoleCalls::profile(net::http::Client& http, AccountId account)
    -> Expected<psn::rest::ProfileDetails, Unanswered> {
  return request().asked_of_console_directory<psn::rest::ProfileDetails>(
      http, [&](psn::auth::BearerToken token) {
        return psn::rest::AccountProfile{http,
                                         psn::auth::AppToken{token.view()}}
            .details(account);
      });
}

auto ConsoleCalls::player_sessions(net::http::Client& http,
                                   std::span<const std::string> session_ids,
                                   std::string_view language,
                                   std::size_t maximum_response_bytes)
    -> Expected<std::vector<psn::rest::PlayerSession>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::PlayerSession>>(
          http, [&](psn::auth::BearerToken token) {
            auto capped = http.limited(maximum_response_bytes);
            return psn::rest::PlayerSessions{capped, std::move(token)}.read(
                session_ids, language);
          });
}

auto ConsoleCalls::friends_page(net::http::Client& http, std::size_t offset,
                                std::string_view language)
    -> Expected<psn::rest::FriendsPage, Unanswered> {
  return request().asked_of_console_directory<psn::rest::FriendsPage>(
      http, [&](psn::auth::BearerToken token) {
        return psn::rest::Friends{http, std::move(token)}.page(offset,
                                                               language);
      });
}

auto ConsoleCalls::friend_profiles(net::http::Client& http,
                                   std::span<const std::string> account_ids,
                                   std::string_view language)
    -> Expected<std::vector<psn::rest::FriendProfile>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::FriendProfile>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Friends{http, std::move(token)}.profiles(
                account_ids, language);
          });
}

auto ConsoleCalls::friend_presences(net::http::Client& http,
                                    std::span<const std::string> account_ids,
                                    std::string_view language)
    -> Expected<std::vector<psn::rest::FriendPresence>, Unanswered> {
  return request()
      .asked_of_console_directory<std::vector<psn::rest::FriendPresence>>(
          http, [&](psn::auth::BearerToken token) {
            return psn::rest::Friends{http, std::move(token)}.presences(
                account_ids, language);
          });
}

auto ConsoleCalls::joined_player_sessions(net::http::Client& http,
                                          std::string_view title_id,
                                          std::string_view platform,
                                          std::string_view device_type,
                                          std::string_view language)
    -> Expected<std::vector<std::string>, Unanswered> {
  const auto account = user_.account_id().decimal();
  return request().asked_of_console_directory<std::vector<std::string>>(
      http, [&](psn::auth::BearerToken token) {
        return psn::rest::SessionInvitations{http, std::move(token)}.joined(
            account, title_id, platform, device_type, language);
      });
}

auto ConsoleCalls::session_share_link(net::http::Client& http,
                                      std::string_view session_id,
                                      std::string_view language)
    -> Expected<psn::rest::SessionShareLink, Unanswered> {
  return request().asked_of_console_directory<psn::rest::SessionShareLink>(
      http, [&](psn::auth::BearerToken token) {
        return psn::rest::SessionInvitations{http, std::move(token)}.share_link(
            session_id, language);
      });
}

auto ConsoleCalls::invite_to_party(net::http::Client& http,
                                   std::string_view party_id,
                                   std::span<const AccountId> accounts,
                                   std::string_view language)
    -> Expected<void, Unanswered> {
  const auto ids = AuthorisedRequest::decimal_ids(accounts);
  auto sent = request().asked_of_console_directory<bool>(
      http,
      [&](psn::auth::BearerToken token)
          -> Expected<bool, psn::rest::SessionInvitations::InviteError> {
        auto done = psn::rest::SessionInvitations{http, std::move(token)}
                        .invite_to_party(party_id, ids, language);
        if (!done) return unexpected(std::move(done.error()));
        return true;
      });
  if (!sent) return unexpected(sent.error());
  return {};
}

auto ConsoleCalls::invite_to_game(net::http::Client& http,
                                  std::string_view session_id,
                                  std::span<const AccountId> accounts,
                                  std::string_view language)
    -> Expected<void, Unanswered> {
  const auto ids = AuthorisedRequest::decimal_ids(accounts);
  auto sent = request().asked_of_console_directory<bool>(
      http,
      [&](psn::auth::BearerToken token)
          -> Expected<bool, psn::rest::SessionInvitations::InviteError> {
        auto done = psn::rest::SessionInvitations{http, std::move(token)}
                        .invite_to_game(session_id, ids, language);
        if (!done) return unexpected(std::move(done.error()));
        return true;
      });
  if (!sent) return unexpected(sent.error());
  return {};
}

auto ConsoleCalls::cloud_consoles(net::http::Client& http, std::size_t offset,
                                  std::string_view platform,
                                  std::size_t maximum_response_bytes)
    -> Expected<psn::rest::ClientsPage, Unanswered> {
  return request().asked_of_console_directory<psn::rest::ClientsPage>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .list_cloud_clients(offset, platform);
      });
}

auto ConsoleCalls::join_session(net::http::Client& http,
                                std::string_view console_duid,
                                std::string_view platform,
                                std::string_view game_intent,
                                std::chrono::milliseconds total_timeout,
                                std::size_t maximum_response_bytes)
    -> Expected<std::string, Unanswered> {
  return request().asked_of_console_directory<std::string>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .send_game_intent(console_duid, platform, game_intent,
                              total_timeout);
      });
}

auto ConsoleCalls::join_party(net::http::Client& http,
                              std::string_view console_duid,
                              std::string_view platform,
                              std::string_view session_id,
                              std::chrono::milliseconds total_timeout,
                              std::size_t maximum_response_bytes)
    -> Expected<std::string, Unanswered> {
  return request().asked_of_console_directory<std::string>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .join_party(console_duid, platform, session_id, total_timeout);
      });
}

auto ConsoleCalls::start_title(net::http::Client& http,
                               std::string_view console_duid,
                               std::string_view launch_id,
                               std::chrono::milliseconds total_timeout,
                               std::size_t maximum_response_bytes)
    -> Expected<std::string, Unanswered> {
  return request().asked_of_console_directory<std::string>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .launch_title(console_duid, launch_id, total_timeout);
      });
}

auto ConsoleCalls::title_start_progress(net::http::Client& http,
                                        std::string_view command_id,
                                        bool mobile, std::string_view platform,
                                        std::chrono::milliseconds total_timeout,
                                        std::size_t maximum_response_bytes)
    -> Expected<psn::rest::CommandPoll, Unanswered> {
  return request().asked_of_console_directory<psn::rest::CommandPoll>(
      http, [&](psn::auth::BearerToken token) {
        auto capped = http.limited(maximum_response_bytes);
        return psn::rest::CloudAssistedNavigation{capped, std::move(token)}
            .poll_command(command_id, total_timeout, mobile, platform);
      });
}

auto ConsoleCalls::consoles_page(net::http::Client& http, std::size_t offset,
                                 std::size_t maximum_response_bytes)
    -> Expected<psn::rest::ClientsPage, Unanswered> {
  using Reason = Unanswered::Reason;
  const auto ask = [&](const psn::auth::BearerToken& token) {
    auto capped = http.limited(maximum_response_bytes);
    return psn::rest::CloudAssistedNavigation{
        capped,
        psn::auth::BearerToken{token.secret().clone(),
                               AuthorisedRequest::remote_play_credential}}
        .list_clients(offset, false);
  };
  if (auto restored = user_.restore_remote_play_access(http.stop_token());
      !restored)
    return unexpected(restored.error());
  auto token = user_.remote_play_token();
  if (!token) return unexpected(Unanswered{Reason::remote_play_access_missing});
  auto renewed = retry::with_renewal(
      [&] { return ask(*token); },
      [](const auto& refused) { return refused.auth_refused(); },
      [&](const auto& refused) -> Expected<void, Unanswered> {
        user_.distrust_remote_play_access(*token);
        if (auto restored = user_.restore_remote_play_access(http.stop_token());
            !restored)
          return unexpected(
              Unanswered{restored.error().reason, refused.http_status()});
        token = user_.remote_play_token();
        if (!token)
          return unexpected(Unanswered{Reason::remote_play_access_missing,
                                       refused.http_status()});
        return {};
      });
  if (!renewed) return unexpected(renewed.error());
  auto& page = renewed.value();
  if (page) return std::move(page.value());
  if (!page.error().auth_refused())
    return unexpected(request().unanswered(page.error()));
  user_.distrust_remote_play_access(*token);
  return unexpected(Unanswered{Reason::remote_play_access_missing,
                               page.error().http_status()});
}

}  // namespace libchiaking::users
