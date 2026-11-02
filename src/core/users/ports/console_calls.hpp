/// @file
/// @brief Declares ConsoleCalls, the consoles context's port onto a user's
/// authorised calls to PS.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/psn/rest/session_manager/player_sessions.hpp"
#include "src/core/psn/rest/session_manager/session_invitations.hpp"
#include "src/core/psn/rest/user_notification/user_notifications.hpp"
#include "src/core/psn/rest/user_profile/account_profile.hpp"
#include "src/core/psn/rest/user_profile/friends.hpp"

#include <libchiaking/detail/users/ps_user.hpp>
#include "src/core/psn/auth/credentials.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::users {

class AuthorisedRequest;

/// @brief What the consoles context may ask PS on a user's behalf: the
/// account's consoles, their titles, starting a title, push alerts, friends,
/// player sessions and invitations.
///
/// A user that is off PSN is answered at once and nothing is sent. A failure
/// that means PSN no longer accepts the NPSSO marks the user's sign-in as
/// refused, so the user reports SignInState::expired.
///
/// The object is a short-lived view: it only refers to the user's core, holds
/// no state of its own, and is made afresh wherever a call is needed. It must
/// not outlive the PsUser it was made for. Every call blocks the calling
/// thread for its requests, each bounded by the HTTP client's timeouts and its
/// stop token; calls may be made from several threads at once, the user's core
/// providing the locking.
class ConsoleCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit ConsoleCalls(PsUser& user) noexcept;

  /// @brief Where the user's push channel connects as a mobile client, and the
  /// app token it opens with.
  struct PushServer {
    /// @brief The app token.
    psn::auth::BearerToken token;
    /// @brief The push server's host name.
    std::string fqdn;
  };

  /// @brief Reads one page of the consoles registered to the account, signed
  /// with the Remote Play token.
  ///
  /// The Remote Play token is restored first. When PS refuses it, it is
  /// dropped, renewed and the page asked once more. Concurrent callers asking
  /// for the same page share one request.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first console on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::remote_play_access_missing No usable Remote
  ///     Play token could be obtained, or PS refused the renewed one too.
  /// @retval Unanswered::Reason Any other failure; see the private
  ///     unanswered(const psn::rest::Error<Reasons...>&).
  [[nodiscard]] auto account_consoles(
      net::http::Client& http, std::size_t offset,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::ClientsPage, Unanswered>;

  /// @brief Reads one page of the account's consoles with the titles
  /// installed on each, signed with the app token.
  ///
  /// The app token is restored first and renewed once if PS refuses it.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first console on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto installed_titles(
      net::http::Client& http, std::size_t offset,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::ClientsPage, Unanswered>;

  /// @brief Reads one page of the titles the account has played recently;
  /// see AuthorisedRequest::played_titles.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first title on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered The failures AuthorisedRequest::played_titles lists.
  [[nodiscard]] auto played_titles(
      net::http::Client& http, std::size_t offset,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::PlayedTitlesPage, Unanswered>;

  /// @brief Reads the account's newest notifications, signed with the app
  /// token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param language The locale PS writes the notifications in, such as
  ///     `en-GB`.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The notifications.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto notifications(
      net::http::Client& http, std::string_view language,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<std::vector<psn::rest::UserNotification>, Unanswered>;

  /// @brief Moves notifications of the user's list to a state, signed with
  /// the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param notification_ids The notifications.
  /// @param state `Deleted`, `Read` or `Seen`.
  /// @return Nothing once PS accepted the change.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto set_notification_state(
      net::http::Client& http, std::span<const std::string> notification_ids,
      std::string_view state) -> Expected<void, Unanswered>;

  /// @brief Finds the PSN push server for the user's push channel opened as
  /// a mobile client, with the app token that opens it.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @return The token and the server's host name.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto push_server(net::http::Client& http)
      -> Expected<PushServer, Unanswered>;

  /// @brief Reads another account's profile, such as the online id of a
  /// friend who sent an invitation, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param account The account to read.
  /// @return The profile.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto profile(net::http::Client& http, AccountId account)
      -> Expected<psn::rest::ProfileDetails, Unanswered>;

  /// @brief Reads game sessions, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param session_ids The sessions; at most psn::rest::player_sessions_batch
  ///     are read.
  /// @param language The locale to send, such as `en-GB`.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The sessions that still exist.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto player_sessions(
      net::http::Client& http, std::span<const std::string> session_ids,
      std::string_view language,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<std::vector<psn::rest::PlayerSession>, Unanswered>;

  /// @brief Reads one page of the user's friends, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param offset Index of the first friend on the page.
  /// @param language The locale to send.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto friends_page(net::http::Client& http, std::size_t offset,
                                  std::string_view language)
      -> Expected<psn::rest::FriendsPage, Unanswered>;

  /// @brief Reads the profiles of up to psn::rest::Friends::page_size
  /// accounts, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param account_ids The accounts.
  /// @param language The locale to send.
  /// @return One profile for each account, in the same order.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto friend_profiles(net::http::Client& http,
                                     std::span<const std::string> account_ids,
                                     std::string_view language)
      -> Expected<std::vector<psn::rest::FriendProfile>, Unanswered>;

  /// @brief Reads whether up to psn::rest::Friends::page_size accounts are
  /// online, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param account_ids The accounts.
  /// @param language The locale to send.
  /// @return One presence for each account, in the same order.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto friend_presences(net::http::Client& http,
                                      std::span<const std::string> account_ids,
                                      std::string_view language)
      -> Expected<std::vector<psn::rest::FriendPresence>, Unanswered>;

  /// @brief Lists the game sessions the user plays in for a title, signed
  /// with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param title_id The title, such as `PPSA01234_00`.
  /// @param platform The game's platform, such as `PS5`.
  /// @param device_type The device the game runs on, such as `PS5`.
  /// @param language The locale to send.
  /// @return The session ids.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto joined_player_sessions(net::http::Client& http,
                                            std::string_view title_id,
                                            std::string_view platform,
                                            std::string_view device_type,
                                            std::string_view language)
      -> Expected<std::vector<std::string>, Unanswered>;

  /// @brief Asks for a link that lets friends join a game session, signed
  /// with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param session_id The session.
  /// @param language The locale to send.
  /// @return The link.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto session_share_link(net::http::Client& http,
                                        std::string_view session_id,
                                        std::string_view language)
      -> Expected<psn::rest::SessionShareLink, Unanswered>;

  /// @brief Invites accounts to a party, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param party_id The party.
  /// @param accounts The invitees; at most
  ///     psn::rest::SessionInvitations::most_invitees.
  /// @param language The locale to send.
  /// @return Nothing once PS accepted the invitations.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto invite_to_party(net::http::Client& http,
                                     std::string_view party_id,
                                     std::span<const AccountId> accounts,
                                     std::string_view language)
      -> Expected<void, Unanswered>;

  /// @brief Invites accounts to a game session, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param session_id The game session.
  /// @param accounts The invitees; at most
  ///     psn::rest::SessionInvitations::most_invitees.
  /// @param language The locale to send.
  /// @return Nothing once PS accepted the invitations.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto invite_to_game(net::http::Client& http,
                                    std::string_view session_id,
                                    std::span<const AccountId> accounts,
                                    std::string_view language)
      -> Expected<void, Unanswered>;

  /// @brief Reads one page of the account's consoles to find its cloud
  /// console, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first console on the page.
  /// @param platform The platform to list, such as `PS5` or `PS4`.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto cloud_consoles(
      net::http::Client& http, std::size_t offset, std::string_view platform,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::ClientsPage, Unanswered>;

  /// @brief Asks PSN to have one of the account's consoles join a game
  /// session.
  ///
  /// Renews the app token as installed_titles() does. Joining is
  /// asynchronous; follow it with title_start_progress().
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param console_duid The console's device id, as the console directory
  ///     lists it.
  /// @param platform The console's platform, such as `PS5` or `PS4`; the
  ///     game intent command carries it.
  /// @param game_intent The session's tokenised player game intent.
  /// @param total_timeout How long the request may take in all.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The id of the join command.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto join_session(net::http::Client& http,
                                  std::string_view console_duid,
                                  std::string_view platform,
                                  std::string_view game_intent,
                                  std::chrono::milliseconds total_timeout,
                                  std::size_t maximum_response_bytes =
                                      net::http::default_maximum_response_bytes)
      -> Expected<std::string, Unanswered>;

  /// @brief Asks a console through PSN to join a party, signed with the app
  /// token, which moves the party's voice chat to the console.
  ///
  /// Renews the app token as installed_titles() does. Joining is
  /// asynchronous; follow it with title_start_progress().
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param console_duid The console's device id, as the console directory
  ///     lists it.
  /// @param platform The console's platform, such as `PS5`.
  /// @param session_id The party session.
  /// @param total_timeout How long the request may take in all.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The id of the join command.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto join_party(net::http::Client& http,
                                std::string_view console_duid,
                                std::string_view platform,
                                std::string_view session_id,
                                std::chrono::milliseconds total_timeout,
                                std::size_t maximum_response_bytes =
                                    net::http::default_maximum_response_bytes)
      -> Expected<std::string, Unanswered>;

  /// @brief Asks PSN to start a title on one of the account's consoles.
  ///
  /// Renews the app token as installed_titles() does. Starting is
  /// asynchronous; follow it with title_start_progress().
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param console_duid The console's device id, as the console directory
  ///     lists it.
  /// @param launch_id The title to start.
  /// @param total_timeout How long the request may take in all.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The id of the start command.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto start_title(net::http::Client& http,
                                 std::string_view console_duid,
                                 std::string_view launch_id,
                                 std::chrono::milliseconds total_timeout,
                                 std::size_t maximum_response_bytes =
                                     net::http::default_maximum_response_bytes)
      -> Expected<std::string, Unanswered>;

  /// @brief Polls how far a start command has got.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param command_id The id start_title() returned.
  /// @param mobile Whether the command was created as a mobile client, as
  ///     join_session() does; its poll needs the matching header.
  /// @param platform The command's platform, such as `PS5` or `PS4`.
  /// @param total_timeout How long the request may take in all.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The command's progress.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto title_start_progress(
      net::http::Client& http, std::string_view command_id, bool mobile,
      std::string_view platform, std::chrono::milliseconds total_timeout,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::CommandPoll, Unanswered>;

 private:
  /// @brief Reads one page of the account's consoles with the Remote Play
  /// token; the request behind account_consoles().
  ///
  /// @param http The client the requests are sent through.
  /// @param offset Index of the first console on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered The failures account_consoles() lists.
  [[nodiscard]] auto consoles_page(net::http::Client& http, std::size_t offset,
                                   std::size_t maximum_response_bytes)
      -> Expected<psn::rest::ClientsPage, Unanswered>;

  /// @brief Opens the shared request helpers on this user.
  ///
  /// @return A view of the same user.
  auto request() const noexcept -> AuthorisedRequest;

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
