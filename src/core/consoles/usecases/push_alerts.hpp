/// @file
/// @brief Declares the PushAlerts use case: watching for game and party
/// invitations and messages through the account's PSN push channel, opened as a
/// mobile client.
#pragma once

#include "libchiaking/detail/consoles/game_invitation.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/psn/rest/push/push.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
class HttpProvider;
class WebSocketConnection;
class WebSocketProvider;
}  // namespace libchiaking::platform

namespace libchiaking::net::http {
class Client;
class Waiter;
}  // namespace libchiaking::net::http

namespace libchiaking::consoles::usecases {

/// @brief Watches for game and party invitations and messages without asking
/// PSN on a timer.
///
/// The invitations are listed once to learn which are already there. The
/// account's push channel is then opened as a mobile client, with the app
/// token, and the list is read again only when PSN pushes that an invitation
/// was created. The notification that
/// names an invitation can reach the list a moment after the push, so a read
/// that finds nothing new is tried again after short pauses. An invitation
/// push whose session the first read does not list as a game invitation is a
/// party invitation, passed on once however often it is pushed or listed;
/// a party listed unread after the watch started is passed on too. Parties
/// listed when the watch starts are not remembered, so a later push of one
/// still alerts. A message push
/// is passed on with its sender unless the user sent it. Every other push is
/// logged by its type and otherwise ignored.
class PushAlerts {
 public:
  /// @brief How many times in a row the channel may fail to open before the
  /// watch gives up.
  static constexpr std::size_t most_failed_opens = 5;
  /// @brief The pause before the channel is opened again after it dropped.
  static constexpr std::chrono::seconds reopen_pause{30};
  /// @brief The pauses before each read of the list after an invitation
  /// push, the first read made at once.
  static constexpr std::array<std::chrono::milliseconds, 3> reread_pauses{
      std::chrono::milliseconds{0}, std::chrono::milliseconds{2000},
      std::chrono::milliseconds{5000}};

  /// @brief Prepares the watch.
  ///
  /// @param http Carries the PSN requests; must outlive this object.
  /// @param websocket Opens the push channel; must outlive this object.
  /// @param clock The monotonic clock requests are timed by; must outlive
  ///     this object.
  /// @param waiter Waits between reads and before the channel is opened
  ///     again; must outlive this object.
  /// @param log Where the watch logs; may be null.
  PushAlerts(platform::HttpProvider& http,
             platform::WebSocketProvider& websocket,
             const platform::ClockProvider& clock, net::http::Waiter& waiter,
             platform::DiagnosticsProvider* log) noexcept;

  /// @brief Watches until stopped.
  ///
  /// @param user The signed-in user.
  /// @param watcher Told of each new invitation and message, on this thread.
  /// @param stop Ends the watch.
  /// @return Nothing once stopped.
  /// @retval users::Unanswered The first listing failed, or the channel
  ///     failed to open most_failed_opens times in a row.
  [[nodiscard]] auto run(users::PsUser& user, AlertWatcher& watcher,
                         const platform::StopToken& stop)
      -> Expected<void, users::Unanswered>;

  /// @brief Picks the invitations to announce from a listing and remembers
  /// every game session listed and every party announced.
  ///
  /// @param listed The invitations listed.
  /// @param known The game sessions listed and parties announced before;
  ///     every listed game session and every unread listed party is added.
  /// @return The unread invitations whose sessions were not in `known`.
  static auto arrivals(const std::vector<GameInvitation>& listed,
                       std::set<std::string, std::less<>>& known)
      -> std::vector<GameInvitation>;

 private:
  /// @brief Opens the push channel as a mobile client.
  ///
  /// @param user The signed-in user.
  /// @param http The client the server lookup goes through.
  /// @param stop Cancels the opening.
  /// @return The connection.
  /// @retval users::Unanswered The token, the server lookup or the connection
  ///     failed.
  auto open(users::PsUser& user, net::http::Client& http,
            const platform::StopToken& stop)
      -> Expected<std::unique_ptr<platform::WebSocketConnection>,
                  users::Unanswered>;
  /// @brief Reads pushes until the channel drops or the watch is stopped,
  /// announcing the invitations each invitation push brings.
  ///
  /// @param connection The open channel.
  /// @param user The signed-in user.
  /// @param watcher Told of each new invitation and message.
  /// @param known The sessions listed so far.
  /// @param stop Ends the reading.
  void listen(platform::WebSocketConnection& connection, users::PsUser& user,
              AlertWatcher& watcher, std::set<std::string, std::less<>>& known,
              const platform::StopToken& stop);
  /// @brief Reads the list after an invitation push, again after each of
  /// reread_pauses until something new is found, and announces it.
  ///
  /// @param user The signed-in user.
  /// @param watcher Told of each new invitation.
  /// @param known The sessions listed or announced so far.
  /// @param pushed The invitation the push named, if it named one; announced
  ///     as a party invitation after the first read unless that read lists
  ///     its session or it was announced before. A party ends the reading
  ///     after that first read, since no listing will name it.
  /// @param stop Ends the reading.
  void announce(users::PsUser& user, AlertWatcher& watcher,
                std::set<std::string, std::less<>>& known,
                std::optional<psn::rest::SessionInvitation> pushed,
                const platform::StopToken& stop);
  /// @brief Passes on a pushed party invitation once.
  ///
  /// @param pushed The invitation the push named.
  /// @param listed The first read after the push, or null when it failed.
  /// @param known The sessions listed or announced so far; the party is
  ///     added.
  /// @param watcher Told of the invitation.
  /// @return True when the push named a party, announced now or before;
  ///     false when the read lists its session as a game invitation. A
  ///     session the read lists as a party invitation is a party.
  auto tell_party(const psn::rest::SessionInvitation& pushed,
                  const std::vector<GameInvitation>* listed,
                  std::set<std::string, std::less<>>& known,
                  AlertWatcher& watcher) const -> bool;
  /// @brief Passes on a listed invitation: a game invitation as one, a
  /// party invitation as a party.
  ///
  /// @param invitation The invitation.
  /// @param watcher Told of it.
  void tell(const GameInvitation& invitation, AlertWatcher& watcher) const;
  /// @brief Passes on the message a lounge-group push announces, unless it
  /// names none or the user sent it.
  ///
  /// @param push The push.
  /// @param user The signed-in user.
  /// @param watcher Told of the message.
  void tell_message(const psn::rest::Notification& push,
                    const users::PsUser& user, AlertWatcher& watcher) const;
  /// @brief Logs a line at info level, when there is a log.
  ///
  /// @param text The line.
  void note(const std::string& text) const;

  /// @brief Carries the PSN requests.
  platform::HttpProvider& http_;
  /// @brief Opens the push channel.
  platform::WebSocketProvider& websocket_;
  /// @brief Makes the locks the invitation listing needs.
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock_;
  /// @brief Waits between reads and openings.
  net::http::Waiter& waiter_;
  /// @brief Where the watch logs; may be null.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::consoles::usecases
