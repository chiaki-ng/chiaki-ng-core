#pragma once

#include "libchiaking/platform/websocket/provider.hpp"
#include "src/core/consoles/usecases/invitations.hpp"
#include "src/core/utils/json/ps_fields.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/unit/core/consoles/account_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class PushChannel final : public libchiaking::platform::WebSocketConnection {
 public:
  explicit PushChannel(const std::vector<std::string>& bodies) {
    for (const auto& body : bodies) {
      libchiaking::platform::WebSocketMessage message;
      message.payload.assign(body);
      messages_.push_back(std::move(message));
    }
  }

  auto receive(const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::WebSocketMessage,
                               libchiaking::platform::WebSocketError> override {
    if (!messages_.empty()) {
      auto message = std::move(messages_.front());
      messages_.pop_front();
      return message;
    }
    while (!stop.stop_requested())
      test_support::LiveProviders::nap(std::chrono::milliseconds{1});
    return libchiaking::unexpected(
        libchiaking::platform::WebSocketError::cancelled);
  }

  auto close()
      -> libchiaking::Expected<void,
                               libchiaking::platform::WebSocketError> override {
    return {};
  }

 private:
  std::deque<libchiaking::platform::WebSocketMessage> messages_;
};

class PushServer final : public libchiaking::platform::WebSocketProvider {
 public:
  explicit PushServer(std::vector<std::string> pushed)
      : bodies(std::move(pushed)) {}

  auto connect(const libchiaking::platform::WebSocketRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::WebSocketConnection>,
          libchiaking::platform::WebSocketError> override {
    url = std::string{request.url.view()};
    for (const auto& header : request.headers)
      headers.emplace_back(header.name, std::string{header.value.view()});
    return std::make_unique<PushChannel>(bodies);
  }

  auto header(std::string_view name) const -> std::string {
    for (const auto& [key, value] : headers)
      if (key == name) return value;
    return {};
  }

  std::vector<std::string> bodies;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
};

class Arrivals final : public libchiaking::consoles::AlertWatcher {
 public:
  void on_invitation(
      const libchiaking::consoles::GameInvitation& invitation) override {
    const libchiaking::platform::Lock lock{mutex_};
    sessions_.push_back(invitation.session_id);
  }

  void on_message(const libchiaking::consoles::MessageAlert& message) override {
    const libchiaking::platform::Lock lock{mutex_};
    messages_.push_back(message);
  }

  void on_party_invitation(
      const libchiaking::consoles::PartyInvitationAlert& party) override {
    const libchiaking::platform::Lock lock{mutex_};
    parties_.push_back(party);
  }

  auto invited() -> std::vector<libchiaking::consoles::PartyInvitationAlert> {
    const libchiaking::platform::Lock lock{mutex_};
    return parties_;
  }

  auto seen() -> std::vector<std::string> {
    const libchiaking::platform::Lock lock{mutex_};
    return sessions_;
  }

  auto heard() -> std::vector<libchiaking::consoles::MessageAlert> {
    const libchiaking::platform::Lock lock{mutex_};
    return messages_;
  }

  auto anything() -> bool {
    const libchiaking::platform::Lock lock{mutex_};
    return !sessions_.empty() || !messages_.empty() || !parties_.empty();
  }

 private:
  libchiaking::platform::Mutex mutex_{test_support::LiveProviders::threading()};
  std::vector<std::string> sessions_;
  std::vector<libchiaking::consoles::MessageAlert> messages_;
  std::vector<libchiaking::consoles::PartyInvitationAlert> parties_;
};

class InvitationsRig {
 public:
  using Reply = test_support::psn::Reply;
  using Sessions = std::vector<std::string>;

  static constexpr std::string_view token = "fake-mobile-token-5e21";
  static constexpr std::string_view remote_play_token =
      "fake-remote-play-token-77a0";
  static constexpr std::string_view session_a =
      "11111111-2222-3333-4444-555555555555";
  static constexpr std::string_view session_b =
      "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  static constexpr std::string_view session_c =
      "cccccccc-dddd-eeee-ffff-000000000000";
  static constexpr std::string_view command_id =
      "9c1e3f2a-0000-4000-8000-00000000beef";
  static constexpr std::string_view notifications_url =
      "https://m.np.playstation.com/api/userNotificationManager/v1/users/me/"
      "notifications?offset=0&limit=100&templateVersion=23.06";
  static constexpr std::string_view sessions_url =
      "https://m.np.playstation.com/api/sessionManager/v1/playerSessions"
      "?fields=sessionId,createdTimestamp,maxPlayers,member(players),"
      "member(nonPsnPlayers),member(spectators),supportedPlatforms,sessionName,"
      "leader,joinableUserType,invitableUserType,"
      "tokenizedPlayerGameIntent,tokenizedSpectatorGameIntent";
  static constexpr std::string_view clients_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients?platform=PS5&includeFields=device,systemData&limit=10&offset=0";
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";
  static constexpr std::string_view profile_url =
      "https://m.np.playstation.com/api/userProfile/v1/"
      "internal/users/222/profiles";

  static auto poll_url() -> std::string {
    return std::string{commands_url} +
           "?platform=PS5&commandIds=" + std::string{command_id} +
           "&includeFields=actions,commandDetail&commandStatus=executing,"
           "completed";
  }

  static auto quoted(std::string_view text) -> std::string {
    return "\"" + std::string{text} + "\"";
  }

  static auto notification(std::string_view use_case, std::string_view session,
                           std::string_view state = "New",
                           std::string_view expires = "2099-01-01T00:00:00Z")
      -> std::string {
    return "{\"notificationId\":\"n-" + std::string{session.substr(0, 4)} +
           "\",\"useCaseId\":" + quoted(use_case) +
           ",\"state\":" + quoted(state) +
           ",\"expirationDateTime\":" + quoted(expires) +
           ",\"updatedDateTime\":\"2026-10-21T20:00:00.000Z\",\"fromUser\":{"
           "\"accountId\":\"222\"},\"associatedTitle\":{\"npTitleId\":"
           "\"PPSA01234_00\"},\"viewData\":{\"parameters\":{\"invitationId\":"
           "\"inv-" +
           std::string{session.substr(0, 4)} +
           "\",\"associatedTitles\":[\"PPSA01234_00\",\"CUSA01234_00\",\"\"],"
           "\"sessionId\":" +
           quoted(session) + "}}}";
  }

  static auto notifications(const std::vector<std::string>& entries)
      -> std::string {
    std::string list;
    for (const auto& entry : entries) list += (list.empty() ? "" : ",") + entry;
    return "{\"notifications\":[" + list + "]}";
  }

  static auto session(std::string_view id, std::string_view players,
                      std::string_view intent = "intent-token") -> std::string {
    return "{\"sessionId\":" + quoted(id) +
           ",\"sessionName\":\"Squad\",\"maxPlayers\":4,\"member\":{"
           "\"players\":[" +
           std::string{players} +
           "]},\"playerGameIntent\":{\"npTitleIds\":[{\"npTitleId\":"
           "\"PPSA01234_00\"}]}" +
           (intent.empty()
                ? std::string{}
                : ",\"tokenizedPlayerGameIntent\":" + quoted(intent)) +
           "}";
  }

  static auto player(std::string_view account, std::string_view online)
      -> std::string {
    return "{\"accountId\":" + quoted(account) +
           ",\"onlineId\":" + quoted(online) + "}";
  }

  static auto invitation_push(std::string_view session) -> std::string {
    return R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created",)"
           R"("body":{"data":{"customProperties":{"from":{"onlineId":"Host",)"
           R"("platform":"PS5"}},"sessionId":")" +
           std::string{session} + R"("}}})";
  }

  static auto at(std::string_view text)
      -> libchiaking::consoles::GameInvitation::Time {
    return libchiaking::json::ps::Instant::from_text(text).value().time();
  }

  void lists(std::vector<Reply> replies) {
    account.http.on("GET", std::string{notifications_url}, std::move(replies));
  }

  void knows_the_friend() {
    account.http.on("GET", std::string{profile_url},
                    {{200, "{\"onlineId\":\"Friend\"}"}});
  }

  void reads_sessions(std::vector<Reply> replies) {
    account.http.on("GET", std::string{sessions_url}, std::move(replies));
  }

  static auto sessions(const std::vector<std::string>& entries) -> std::string {
    std::string list;
    for (const auto& entry : entries) list += (list.empty() ? "" : ",") + entry;
    return "{\"playerSessions\":[" + list + "]}";
  }

  auto sessions_read() const -> const test_support::psn::Seen& {
    return *std::ranges::find(account.http.seen, std::string{sessions_url},
                              &test_support::psn::Seen::url);
  }

  void watch(PushServer& server, Arrivals& arrivals,
             const std::function<bool()>& done) {
    knows_the_friend();
    account.http.on(
        "GET",
        "https://mobile-pushcl.np.communication.playstation.net/np/"
        "serveraddr?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3",
        {{200, "{\"fqdn\":\"push.example.com\"}"}});
    account.consoles.watch_through(&server);
    libchiaking::platform::StopSource stop;
    auto watching = test_support::LiveProviders::spawn("invitation-watch", [&] {
      EXPECT_TRUE(account.consoles.watch_alerts(*user, arrivals, stop.token()));
    });
    for (int tries = 0; tries < 10000 && !done(); ++tries)
      test_support::LiveProviders::nap(std::chrono::milliseconds{1});
    stop.request_stop();
    static_cast<void>(watching.join());
    account.consoles.watch_through(nullptr);
  }

  auto watched(PushServer& server) -> Sessions {
    Arrivals arrivals;
    watch(server, arrivals, [&] { return arrivals.anything(); });
    return arrivals.seen();
  }

  Account account;
  std::shared_ptr<libchiaking::users::PsUser> user{
      account.user(111, token, remote_play_token)};
};

}  // namespace consoles_test
