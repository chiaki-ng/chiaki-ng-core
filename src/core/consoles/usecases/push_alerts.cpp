#include "src/core/consoles/usecases/push_alerts.hpp"

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "src/core/consoles/usecases/invitations.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/users/ports/console_calls.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace libchiaking::consoles::usecases {

PushAlerts::PushAlerts(platform::HttpProvider& http,
                       platform::WebSocketProvider& websocket,
                       const platform::ClockProvider& clock,
                       net::http::Waiter& waiter,
                       platform::DiagnosticsProvider* log) noexcept
    : http_(http),
      websocket_(websocket),
      clock_(clock),
      waiter_(waiter),
      log_(log) {}

auto PushAlerts::run(users::PsUser& user, AlertWatcher& watcher,
                     const platform::StopToken& stop)
    -> Expected<void, users::Unanswered> {
  Invitations invitations{http_, clock_, log_, waiter_};
  auto first = invitations.list(user, stop);
  if (!first) {
    note("[ALERTS] watch ended: the first listing failed, HTTP " +
         std::to_string(first.error().http_status));
    return unexpected(first.error());
  }
  std::set<std::string, std::less<>> known;
  for (const auto& invitation : first.value())
    if (invitation.kind == GameInvitation::Kind::game)
      known.insert(invitation.session_id);
  std::size_t failed_opens = 0;
  while (!stop.stop_requested()) {
    net::http::Client http{http_, clock_, stop, log_};
    auto connection = open(user, http, stop);
    if (!connection) {
      if (stop.stop_requested()) break;
      note("[ALERTS] push channel did not open, HTTP " +
           std::to_string(connection.error().http_status) +
           (connection.error().is_cancelled() ? ", cancelled" : ""));
      if (++failed_opens >= most_failed_opens) {
        note("[ALERTS] watch ended: the push channel failed to open " +
             std::to_string(failed_opens) + " times");
        return unexpected(connection.error());
      }
    } else {
      failed_opens = 0;
      note("[ALERTS] listening for pushes");
      listen(*connection.value(), user, watcher, known, stop);
      static_cast<void>(connection.value()->close());
      if (stop.stop_requested()) break;
    }
    const auto pause =
        std::chrono::duration_cast<std::chrono::milliseconds>(reopen_pause);
    if (!waiter_.wait_milliseconds(static_cast<int>(pause.count()), stop))
      break;
  }
  note("[ALERTS] watch stopped");
  return {};
}

auto PushAlerts::arrivals(const std::vector<GameInvitation>& listed,
                          std::set<std::string, std::less<>>& known)
    -> std::vector<GameInvitation> {
  std::vector<GameInvitation> arrived;
  for (const auto& invitation : listed) {
    if (invitation.kind == GameInvitation::Kind::party && !invitation.unread)
      continue;
    const bool fresh = known.insert(invitation.session_id).second;
    if (fresh && invitation.unread) arrived.push_back(invitation);
  }
  return arrived;
}

auto PushAlerts::open(users::PsUser& user, net::http::Client& http,
                      const platform::StopToken& stop)
    -> Expected<std::unique_ptr<platform::WebSocketConnection>,
                users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  auto server = users::ConsoleCalls{user}.push_server(http);
  if (!server) return unexpected(server.error());
  auto channel = psn::rest::PushChannelRequest::create(
      server.value().token, server.value().fqdn, psn::rest::PushApp::mobile);
  if (!channel) return unexpected(users::Unanswered{Reason::unavailable});
  auto connection = websocket_.connect(channel.value().request(), stop);
  if (!connection) {
    if (connection.error() == platform::WebSocketError::cancelled)
      return unexpected(users::Unanswered::cancelled());
    return unexpected(users::Unanswered{Reason::unavailable});
  }
  return std::move(connection.value());
}

void PushAlerts::listen(platform::WebSocketConnection& connection,
                        users::PsUser& user, AlertWatcher& watcher,
                        std::set<std::string, std::less<>>& known,
                        const platform::StopToken& stop) {
  while (!stop.stop_requested()) {
    auto message = connection.receive(stop);
    if (!message) {
      if (!stop.stop_requested())
        note("[ALERTS] push channel dropped, error " +
             std::to_string(static_cast<int>(message.error())));
      return;
    }
    const auto push =
        psn::rest::Notification::parse(message.value().payload.bytes());
    if (push.type() == psn::rest::NotificationType::lounge_group_changed) {
      tell_message(push, user, watcher);
      continue;
    }
    if (push.type() != psn::rest::NotificationType::invitation_created) {
      note("[ALERTS] push " + push.data_type() + " ignored");
      continue;
    }
    note("[ALERTS] push " + push.data_type());
    auto pushed = push.session_invitation();
    announce(user, watcher, known,
             pushed ? std::optional{std::move(pushed.value())} : std::nullopt,
             stop);
  }
}

void PushAlerts::announce(users::PsUser& user, AlertWatcher& watcher,
                          std::set<std::string, std::less<>>& known,
                          std::optional<psn::rest::SessionInvitation> pushed,
                          const platform::StopToken& stop) {
  Invitations invitations{http_, clock_, log_, waiter_};
  for (const auto pause : reread_pauses) {
    if (pause.count() > 0 &&
        !waiter_.wait_milliseconds(static_cast<int>(pause.count()), stop))
      return;
    auto listed = invitations.list(user, stop);
    const bool party =
        pushed &&
        tell_party(*pushed, listed ? &listed.value() : nullptr, known, watcher);
    pushed.reset();
    if (!listed) {
      if (party) return;
      continue;
    }
    const auto arrived = arrivals(listed.value(), known);
    for (const auto& invitation : arrived) tell(invitation, watcher);
    if (party || !arrived.empty()) return;
  }
}

auto PushAlerts::tell_party(const psn::rest::SessionInvitation& pushed,
                            const std::vector<GameInvitation>* listed,
                            std::set<std::string, std::less<>>& known,
                            AlertWatcher& watcher) const -> bool {
  if (listed != nullptr &&
      std::ranges::any_of(*listed, [&](const GameInvitation& invitation) {
        return invitation.session_id == pushed.session_id &&
               invitation.kind == GameInvitation::Kind::game;
      }))
    return false;
  if (!known.insert(pushed.session_id).second) {
    note("[ALERTS] party invitation already announced");
    return true;
  }
  note("[ALERTS] party invitation arrived");
  watcher.on_party_invitation(
      PartyInvitationAlert{pushed.sender, pushed.session_id});
  return true;
}

void PushAlerts::tell(const GameInvitation& invitation,
                      AlertWatcher& watcher) const {
  if (invitation.kind == GameInvitation::Kind::game) {
    watcher.on_invitation(invitation);
    return;
  }
  note("[ALERTS] party invitation listed");
  watcher.on_party_invitation(
      PartyInvitationAlert{invitation.inviter, invitation.session_id});
}

void PushAlerts::tell_message(const psn::rest::Notification& push,
                              const users::PsUser& user,
                              AlertWatcher& watcher) const {
  auto message = push.lounge_message();
  if (!message) {
    note("[ALERTS] push " + push.data_type() + " without a message");
    return;
  }
  if (message->sender == user.online_id()) {
    note("[ALERTS] push of the user's own message");
    return;
  }
  note("[ALERTS] message arrived");
  watcher.on_message(MessageAlert{std::move(message->sender),
                                  std::move(message->group_id),
                                  std::move(message->thread_id)});
}

void PushAlerts::note(const std::string& text) const {
  if (log_ == nullptr) return;
  log_->write({platform::LogSeverity::info, text});
}

}  // namespace libchiaking::consoles::usecases
