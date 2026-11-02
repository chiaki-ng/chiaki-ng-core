/// @file
/// @brief Declares the Invitations use case: listing the game and party
/// invitations in the account's PSN notifications and having the account's
/// PS5 cloud console join one.
#pragma once

#include "libchiaking/detail/consoles/console_titles.hpp"
#include "libchiaking/detail/consoles/game_invitation.hpp"
#include "libchiaking/detail/consoles/session_sharing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/np_title_id.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "src/core/psn/rest/session_manager/player_sessions.hpp"
#include "src/core/psn/rest/user_notification/user_notifications.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::net::http {
class Client;
class Waiter;
}  // namespace libchiaking::net::http

namespace libchiaking::users {
class ConsoleCalls;
}  // namespace libchiaking::users

namespace libchiaking::consoles::usecases {

/// @brief Lists and joins game and party invitations.
///
/// An invitation is a notification with one of the game invitation use
/// cases, or the party invitation use case,
/// that names a session and has not expired on PSN's clock. Each is matched to
/// its session, which decides whether it can be joined. Holds references only;
/// may be used from several threads at once. Everything it refers to must
/// outlive it.
class Invitations {
 public:
  /// @brief The notification use cases that carry game invitations.
  static constexpr std::array<std::string_view, 6> invitation_use_cases{
      "NUC3", "NUC68", "NUC235", "NUC236", "NUC309", "NUC597"};
  /// @brief The notification use case that carries party invitations.
  static constexpr std::string_view party_use_case = "NUC527";
  /// @brief How long an invitation to an old PS4 session is listed after it
  /// was sent. PSN cannot say whether such a session has ended and keeps the
  /// notification for 30 days, while a PS4 game session rarely lasts this
  /// long.
  static constexpr std::chrono::hours old_session_lifetime{6};
  /// @brief How long a party join may take, from sending the command.
  static constexpr std::chrono::seconds party_deadline{120};
  /// @brief How long a cloud join waits for the cloud console to be listed,
  /// from the first listing. A cloud console that has just started
  /// streaming is listed only once it has signed in.
  static constexpr std::chrono::seconds cloud_console_deadline{60};
  /// @brief Time between listings while a cloud join waits for the cloud
  /// console.
  static constexpr std::chrono::seconds cloud_console_poll{3};
  /// @brief The locale sent when the account states none.
  static constexpr std::string_view default_locale = "en-US";
  /// @brief The most console pages read looking for the cloud console.
  static constexpr std::size_t most_console_pages = 5;

  /// @brief Where a join has got to.
  enum class Stage : std::uint8_t {
    /// @brief Reading the session to learn how to join it.
    reading_session,
    /// @brief Looking for the account's cloud console, waiting for it to be
    /// listed.
    finding_cloud_console,
    /// @brief The console is joining.
    joining,
  };

  /// @brief Which console a party join goes to.
  struct PartyTarget {
    /// @brief The console's device uid as PSN lists it.
    std::string duid;
    /// @brief The console's platform, such as `PS5`.
    std::string platform;
  };

  /// @brief Makes the use case.
  ///
  /// @param http Sends the PSN requests; must outlive this object.
  /// @param clock The monotonic clock a join's deadline is measured by; must
  ///     outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param waiter Pauses a join's waits; must outlive this object and take
  ///     waits from every thread that joins through it.
  Invitations(platform::HttpProvider& http,
              const platform::ClockProvider& clock,
              platform::DiagnosticsProvider* log,
              net::http::Waiter& waiter) noexcept;
  /// @brief Not copyable.
  Invitations(const Invitations&) = delete;
  /// @brief Not copyable.
  auto operator=(const Invitations&) -> Invitations& = delete;

  /// @brief Lists the user's game invitations.
  ///
  /// Blocks while the notifications are read and then the session manager
  /// sessions the game invitations point at, so an
  /// invitation to a session that has ended or that the user already plays
  /// in is left out. Party invitations and old PS4 sessions, whose ids the
  /// session manager does not know, are kept until PSN expires them. When
  /// the sessions cannot be read, every live invitation is listed.
  ///
  /// @param user The signed-in user.
  /// @param stop Cancels the requests.
  /// @return The invitations, newest notification first as PSN lists them.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto list(users::PsUser& user, const platform::StopToken& stop)
      -> Expected<std::vector<GameInvitation>, users::Unanswered>;
  /// @brief Deletes notifications from the account's list.
  ///
  /// @param user The signed-in user.
  /// @param notification_ids The notifications, from
  ///     GameInvitation::notification_ids.
  /// @param stop Cancels the request.
  /// @return Nothing once PSN accepted the change.
  /// @retval users::Unanswered PSN did not answer; the reason and HTTP status
  ///     say why.
  [[nodiscard]] auto dismiss(users::PsUser& user,
                             std::span<const std::string> notification_ids,
                             const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;
  /// @brief The most friend pages read, so at most 500 friends are listed.
  static constexpr std::size_t most_friend_pages = 5;

  /// @brief Lists the user's friends with their online ids, avatars and
  /// whether they are online.
  ///
  /// Reads the friends a page at a time, up to most_friend_pages, then each
  /// page's profiles and presences. A presence or profile read PSN refuses
  /// leaves those friends shown offline or unnamed.
  ///
  /// @param user The signed-in user.
  /// @param stop Cancels the requests.
  /// @return The friends in PSN's order, online friends first.
  /// @retval users::Unanswered A friends page could not be read, or the stop
  ///     token fired.
  [[nodiscard]] auto friends(users::PsUser& user,
                             const platform::StopToken& stop)
      -> Expected<std::vector<PsnFriend>, users::Unanswered>;
  /// @brief Finds the game session the user plays in for a title.
  ///
  /// Lists the user's sessions for the title and reads the first one's
  /// details.
  ///
  /// @param user The signed-in user.
  /// @param title The title.
  /// @param platform The console the game runs on; also sent as the device
  ///     type.
  /// @param stop Cancels the requests.
  /// @return The session, or nothing when the user plays in none for the
  ///     title.
  /// @retval users::Unanswered PSN did not answer.
  [[nodiscard]] auto own_game_session(users::PsUser& user,
                                      const NpTitleId& title,
                                      GamePlatform platform,
                                      const platform::StopToken& stop)
      -> Expected<std::optional<OwnGameSession>, users::Unanswered>;
  /// @brief Asks for a link friends can open to join a game session.
  ///
  /// @param user The signed-in user.
  /// @param session_id The session, from own_game_session().
  /// @param stop Cancels the request.
  /// @return The link.
  /// @retval users::Unanswered PSN did not answer or gave no link.
  [[nodiscard]] auto share_game_session(users::PsUser& user,
                                        std::string_view session_id,
                                        const platform::StopToken& stop)
      -> Expected<SessionLink, users::Unanswered>;
  /// @brief Invites friends to a game session or a party.
  ///
  /// Sends the invitees psn::rest::SessionInvitations::most_invitees at a
  /// time and stops at the first batch PSN refuses.
  ///
  /// @param user The signed-in user.
  /// @param target The session or party.
  /// @param accounts The friends.
  /// @param stop Cancels the requests.
  /// @return Nothing once PSN accepted every invitation; nothing is sent for
  ///     no invitees.
  /// @retval users::Unanswered PSN did not answer or refused a batch.
  [[nodiscard]] auto invite_friends(users::PsUser& user,
                                    const InvitationTarget& target,
                                    std::span<const AccountId> accounts,
                                    const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;
  /// @brief Has the account's PS5 cloud console join an invitation's
  /// session.
  ///
  /// Reads the session again, finds the cloud console and sends PSN's join
  /// command to it, following the command until the console reports it
  /// finished. A cloud console that is not listed yet is looked for again
  /// every cloud_console_poll for up to cloud_console_deadline. Blocks for
  /// up to that wait and then the join's deadline.
  ///
  /// @param user The signed-in user.
  /// @param session_id The invitation's session.
  /// @param watcher Told each state of the join command; may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure::Reason::no_cloud_console No cloud console
  ///     was listed within cloud_console_deadline.
  /// @retval InvitationJoinFailure Why it did not join.
  [[nodiscard]] auto join_in_cloud(users::PsUser& user,
                                   std::string_view session_id,
                                   StartWatcher* watcher,
                                   const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief Has one of the user's home PS5s join an invitation's game
  /// session.
  ///
  /// Reads the session, checks it can be joined and sends PSN's join command
  /// to the console, following the command until the console reports it
  /// finished.
  ///
  /// @param user The signed-in user.
  /// @param session_id The invitation's session.
  /// @param target The console.
  /// @param watcher Told each state of the join command; may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure::Reason::console_unavailable The target
  ///     names no console.
  /// @retval InvitationJoinFailure Why it did not join.
  [[nodiscard]] auto join_on_console(users::PsUser& user,
                                     std::string_view session_id,
                                     const PartyTarget& target,
                                     StartWatcher* watcher,
                                     const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief Has a home console join a party invitation's party.
  ///
  /// Sends PSN's party command to the given console and follows it until the
  /// console reports it finished. Blocks for up to party_deadline. PSN
  /// forbids the command on the account's cloud console, so there is no
  /// cloud form.
  ///
  /// @param user The signed-in user.
  /// @param session_id The party session.
  /// @param target The console.
  /// @param watcher Told each state of the command; may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure::Reason::console_unavailable The target
  ///     names no console.
  /// @retval InvitationJoinFailure Why it did not join.
  [[nodiscard]] auto join_party(users::PsUser& user,
                                std::string_view session_id,
                                const PartyTarget& target,
                                StartWatcher* watcher,
                                const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;

  /// @brief Matches invitation notifications to their sessions.
  ///
  /// Keeps notifications with an invitation use case and a session id that
  /// are not deleted and, when PSN's time is known, have not expired, and
  /// for an old PS4 session were sent within old_session_lifetime. Each
  /// session is listed once, from its newest notification.
  ///
  /// @param notifications The account's notifications, newest first.
  /// @param sessions The sessions read for them.
  /// @param account_id The user's account id in decimal.
  /// @param now PSN's time, when known.
  /// @return The invitations.
  static auto matched(
      std::span<const psn::rest::UserNotification> notifications,
      std::span<const psn::rest::PlayerSession> sessions,
      std::string_view account_id, std::optional<GameInvitation::Time> now)
      -> std::vector<GameInvitation>;
  /// @brief Decides whether a session can be joined.
  ///
  /// @param session The session.
  /// @param account_id The user's account id in decimal.
  /// @return already_joined when the user plays in it; cannot_join when it
  ///     gives no game intent; full when its players reach its limit;
  ///     otherwise joinable.
  static auto joining(const psn::rest::PlayerSession& session,
                      std::string_view account_id) -> GameInvitation::Joining;
  /// @brief Finds the cloud console among the account's consoles.
  ///
  /// @param consoles One page of consoles.
  /// @return The first cloud console that has a duid, with its platform, if
  ///     any has one.
  static auto cloud_console(
      std::span<const psn::rest::RegisteredConsole> consoles)
      -> std::optional<psn::rest::RegisteredConsole>;
  /// @brief The join failure for a command that did not finish, or for a
  /// home console that could not take it.
  ///
  /// @param why The start failure.
  /// @return The matching join failure: console_unavailable for a console
  ///     that is forgotten, a PS4, not paired or not on the account.
  static auto failed(const StartFailure& why) noexcept -> InvitationJoinFailure;
  /// @brief The session manager sessions read for a list of invitations.
  struct SessionsRead {
    /// @brief The session ids asked about and answered for.
    std::vector<std::string> asked;
    /// @brief The sessions PSN returned; an asked session missing here has
    /// ended.
    std::vector<psn::rest::PlayerSession> read;
  };

  /// @brief Leaves out the invitations that can no longer be accepted.
  ///
  /// @param invitations The matched invitations.
  /// @param sessions The sessions read for them.
  /// @return The invitations without those the user already plays in and
  ///     those whose session was asked about and has ended.
  static auto current(std::vector<GameInvitation> invitations,
                      const SessionsRead& sessions)
      -> std::vector<GameInvitation>;
  /// @brief Whether a notification is a live game or party invitation.
  ///
  /// @param notification The notification.
  /// @param now PSN's time, when known.
  /// @return True for a game or party invitation use case with a session id
  ///     that is not deleted and has not expired; with PSN's time known, an
  ///     invitation to an old PS4 session, whose id the session manager
  ///     cannot read, only while it is younger than old_session_lifetime.
  static auto is_invitation(const psn::rest::UserNotification& notification,
                            std::optional<GameInvitation::Time> now) -> bool;

 private:
  /// @brief Fills in the online id of each sender PS named only by account
  /// id, reading each account's profile once; a sender whose profile cannot be
  /// read stays unnamed.
  ///
  /// @param calls The user's authorised calls.
  /// @param http The client the requests are sent through.
  /// @param notifications The notifications to name the senders of.
  static void name_senders(
      users::ConsoleCalls& calls, net::http::Client& http,
      std::vector<psn::rest::UserNotification>& notifications);
  /// @brief Reads the session manager sessions of the game invitations, in
  /// batches of psn::rest::player_sessions_batch.
  ///
  /// Only game invitations whose session id is a session manager id are
  /// asked about. A batch PSN does not answer ends the reading; the sessions
  /// it and later batches name are then not counted as asked.
  ///
  /// @param calls The user's authorised calls.
  /// @param http The client the requests are sent through.
  /// @param notifications The live invitation notifications.
  /// @param language The locale to send.
  /// @return The sessions asked about and the ones PSN returned.
  /// @retval users::Unanswered The stop token fired.
  static auto read_sessions(
      users::ConsoleCalls& calls, net::http::Client& http,
      std::span<const psn::rest::UserNotification> notifications,
      std::string_view language) -> Expected<SessionsRead, users::Unanswered>;
  /// @brief The locale PS is asked to write in.
  ///
  /// @param user The user.
  /// @return The account's locale, or default_locale when it states none.
  static auto language_of(const users::PsUser& user) -> std::string;
  /// @brief The platform as PSN names it in session requests.
  ///
  /// @param platform The console the game runs on.
  /// @return `PS4` or `PS5`.
  static auto platform_name(GamePlatform platform) noexcept -> std::string_view;
  /// @brief Reads a time PSN wrote.
  ///
  /// @param text The time.
  /// @return The time, or nothing when it does not parse.
  static auto time_of(std::string_view text)
      -> std::optional<GameInvitation::Time>;
  /// @brief Finds the account's cloud console, reading up to
  /// most_console_pages pages.
  ///
  /// @param calls The user's authorised calls.
  /// @param http The client the requests are sent through.
  /// @return The cloud console.
  /// @retval InvitationJoinFailure No cloud console is listed, or PSN did not
  ///     answer.
  static auto find_cloud_console(users::ConsoleCalls& calls,
                                 net::http::Client& http)
      -> Expected<psn::rest::RegisteredConsole, InvitationJoinFailure>;
  /// @brief Finds the account's cloud console, listing the consoles again
  /// every cloud_console_poll while none is listed, for up to
  /// cloud_console_deadline.
  ///
  /// Only a listing without a cloud console is tried again; a request PSN
  /// did not answer ends the wait at once.
  ///
  /// @param calls The user's authorised calls.
  /// @param http The client the requests are sent through.
  /// @param waits Pauses between listings.
  /// @param stop Ends the wait.
  /// @return The cloud console.
  /// @retval InvitationJoinFailure::Reason::no_cloud_console None was listed
  ///     by the deadline.
  /// @retval InvitationJoinFailure::Reason::cancelled The stop token fired.
  /// @retval InvitationJoinFailure PSN did not answer.
  auto await_cloud_console(users::ConsoleCalls& calls, net::http::Client& http,
                           net::http::Waiter& waits,
                           const platform::StopToken& stop)
      -> Expected<psn::rest::RegisteredConsole, InvitationJoinFailure>;
  /// @brief The join failure for a request PSN did not answer.
  ///
  /// @param why PSN's failure.
  /// @return cancelled for a cancellation, otherwise unanswered.
  static auto unanswered(const users::Unanswered& why) -> InvitationJoinFailure;
  /// @brief Joins a game session on the cloud console or a home console.
  ///
  /// @param user The signed-in user.
  /// @param session_id The session.
  /// @param home The home console, or nothing for the account's cloud
  ///     console.
  /// @param watcher Told each state of the join command; may be null.
  /// @param stop Cancels the join.
  /// @return Nothing once the console reports the join completed.
  /// @retval InvitationJoinFailure Why it did not join.
  [[nodiscard]] auto join(users::PsUser& user, std::string_view session_id,
                          const std::optional<PartyTarget>& home,
                          StartWatcher* watcher,
                          const platform::StopToken& stop)
      -> Expected<void, InvitationJoinFailure>;
  /// @brief A stage's name, for the log.
  ///
  /// @param stage The stage.
  /// @return Its name.
  static auto name_of(Stage stage) noexcept -> std::string_view;

  /// @brief Sends the PSN requests.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Pauses a join's waits.
  net::http::Waiter& waiter_;
};

}  // namespace libchiaking::consoles::usecases
