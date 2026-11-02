#include "tests/unit/core/consoles/usecases/invitations_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::GameInvitation;
using libchiaking::consoles::InvitationJoinFailure;
using libchiaking::consoles::usecases::Invitations;
using libchiaking::psn::rest::PlayerSession;
using libchiaking::psn::rest::RegisteredConsole;
using libchiaking::psn::rest::SessionPlayer;
using libchiaking::psn::rest::UserNotification;
using Joining = GameInvitation::Joining;
using Reason = InvitationJoinFailure::Reason;
using Rig = InvitationsRig;

class InvitationRules : public ::testing::Test {
 protected:
  static auto read_session(std::string_view id, std::uint32_t max,
                           std::vector<std::string> accounts,
                           std::string intent) -> PlayerSession {
    PlayerSession found;
    found.session_id = std::string{id};
    found.max_players = max;
    for (auto& account : accounts)
      found.players.push_back(SessionPlayer{std::move(account), "someone"});
    found.game_intent = libchiaking::SecretString{intent};
    return found;
  }

  static auto read_notification(
      std::string_view use_case, std::string_view session_id,
      std::string_view state = "New",
      std::string_view expires = "2099-01-01T00:00:00Z") -> UserNotification {
    UserNotification found;
    found.use_case_id = std::string{use_case};
    found.session_id = std::string{session_id};
    found.state = std::string{state};
    found.expires = std::string{expires};
    found.from_account_id = "222";
    return found;
  }

  const GameInvitation::Time now = Rig::at("2026-10-21T21:00:00Z");
};

TEST_F(InvitationRules, KeepTheInvitationUseCases) {
  for (const std::string_view use_case : {"NUC3", "NUC597", "NUC309"})
    EXPECT_TRUE(Invitations::is_invitation(
        read_notification(use_case, Rig::session_a), now))
        << use_case;
}

TEST_F(InvitationRules, DropAnythingElse) {
  EXPECT_FALSE(Invitations::is_invitation(
      read_notification("NUC1", Rig::session_a), now));
  EXPECT_FALSE(Invitations::is_invitation(read_notification("NUC3", ""), now));
  EXPECT_FALSE(Invitations::is_invitation(
      read_notification("NUC3", Rig::session_a, "Deleted"), now));
}

TEST_F(InvitationRules, DropAnExpiredInvitationOnlyWhenTheTimeIsKnown) {
  EXPECT_FALSE(Invitations::is_invitation(
      read_notification("NUC3", Rig::session_a, "New", "2026-10-21T20:00:00Z"),
      now));
  EXPECT_TRUE(Invitations::is_invitation(
      read_notification("NUC3", Rig::session_a, "New", "2026-10-21T20:00:00Z"),
      std::nullopt));
}

TEST_F(InvitationRules, DropAnOldPs4SessionInvitationAfterSixHours) {
  auto fresh =
      read_notification("NUC309", "000-" + std::string{Rig::session_a});
  fresh.updated = "2026-10-21T15:30:00Z";
  EXPECT_TRUE(Invitations::is_invitation(fresh, now));
  auto stale = fresh;
  stale.updated = "2026-10-21T14:59:00Z";
  EXPECT_FALSE(Invitations::is_invitation(stale, now));
  EXPECT_TRUE(Invitations::is_invitation(stale, std::nullopt));
  auto current = read_notification("NUC3", Rig::session_a);
  current.updated = "2026-10-01T00:00:00Z";
  EXPECT_TRUE(Invitations::is_invitation(current, now));
  auto party = read_notification("NUC527", "party-session");
  party.updated = "2026-10-01T00:00:00Z";
  EXPECT_TRUE(Invitations::is_invitation(party, now));
}

TEST_F(InvitationRules, FollowTheSessionStateForJoining) {
  EXPECT_EQ(Invitations::joining(read_session(Rig::session_a, 4, {"222"}, "t"),
                                 "111"),
            Joining::joinable);
  EXPECT_EQ(Invitations::joining(read_session(Rig::session_a, 4, {"111"}, "t"),
                                 "111"),
            Joining::already_joined);
  EXPECT_EQ(Invitations::joining(
                read_session(Rig::session_a, 2, {"1", "2"}, "t"), "111"),
            Joining::full);
  EXPECT_EQ(Invitations::joining(
                read_session(Rig::session_a, 0, {"1", "2"}, "t"), "111"),
            Joining::joinable);
  EXPECT_EQ(
      Invitations::joining(read_session(Rig::session_a, 4, {"222"}, ""), "111"),
      Joining::cannot_join);
}

TEST_F(InvitationRules, ListEachSessionOnceWithItsDetails) {
  const std::vector<UserNotification> notifications{
      read_notification("NUC3", Rig::session_a),
      read_notification("NUC68", Rig::session_a, "Read"),
      read_notification("NUC236", Rig::session_b, "Seen")};
  auto first = read_session(Rig::session_a, 4, {"222"}, "t");
  first.players.front().online_id = "Friend";
  first.name = "Squad";
  std::vector<PlayerSession> sessions;
  sessions.push_back(std::move(first));
  const auto found =
      Invitations::matched(notifications, sessions, "111", std::nullopt);
  ASSERT_EQ(found.size(), 2U);
  EXPECT_EQ(found[0].session_id, Rig::session_a);
  EXPECT_TRUE(found[0].unread);
  EXPECT_EQ(found[0].inviter, "Friend");
  EXPECT_EQ(found[0].session_name, "Squad");
  EXPECT_EQ(found[0].players, 1U);
  EXPECT_EQ(found[0].max_players, 4U);
  EXPECT_EQ(found[0].joining, Joining::joinable);
  EXPECT_EQ(found[1].session_id, Rig::session_b);
  EXPECT_FALSE(found[1].unread);
  EXPECT_EQ(found[1].joining, Joining::cannot_join);
}

TEST(InvitationCloudConsole, IsFoundByItsProperty) {
  RegisteredConsole home;
  home.duid = "AAAA";
  RegisteredConsole cloud;
  cloud.duid = " BBBB ";
  cloud.cloud_console = true;
  const std::vector<RegisteredConsole> consoles{home, cloud};
  const auto found = Invitations::cloud_console(consoles);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->duid, "BBBB");
  const std::vector<RegisteredConsole> none{home};
  EXPECT_FALSE(Invitations::cloud_console(none));
}

class InvitationsListed : public ::testing::Test {
 protected:
  Rig rig;
};

TEST_F(InvitationsListed, AreReadWithTheAppToken) {
  rig.lists(
      {{200, "{\"notifications\":[" +
                 Rig::notification("NUC3", Rig::session_a) + "," +
                 Rig::notification("NUC9", Rig::session_b) +
                 "],\"lastModifiedDateTime\":\"2026-10-21T20:00:00Z\"}"}});
  rig.knows_the_friend();
  rig.reads_sessions(
      {{200, Rig::sessions({Rig::session(Rig::session_a,
                                         Rig::player("222", "Friend"))})}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 1U);
  const auto& invitation = found->front();
  EXPECT_EQ(invitation.session_id, Rig::session_a);
  EXPECT_EQ(invitation.invitation_id, "inv-1111");
  EXPECT_EQ(invitation.session_name, "Squad");
  EXPECT_EQ(invitation.players, 1U);
  EXPECT_EQ(invitation.inviter, "Friend");
  EXPECT_EQ(invitation.title_ids,
            (std::vector<std::string>{"PPSA01234_00", "CUSA01234_00"}));
  EXPECT_EQ(invitation.joining, Joining::joinable);
  EXPECT_EQ(invitation.sent, Rig::at("2026-10-21T20:00:00.000Z"));
  ASSERT_EQ(rig.account.http.seen.size(), 3U);
  EXPECT_EQ(rig.account.http.count("profiles"), 1);
  EXPECT_EQ(rig.account.http.count("playerSessions"), 1);
  const auto& read = rig.account.http.seen.front();
  EXPECT_EQ(read.header("Authorization"), "Bearer " + std::string{Rig::token});
  EXPECT_EQ(read.header("Accept-Language"), "en-US");
  EXPECT_EQ(rig.sessions_read().header("X-PSN-SESSION-MANAGER-SESSION-IDS"),
            Rig::session_a);
}

TEST_F(InvitationsListed, LeaveEndedAndJoinedSessionsOut) {
  const std::string old_ps4_session = "0123456789ABCDEF";
  rig.lists({{200, Rig::notifications(
                       {Rig::notification("NUC3", Rig::session_a),
                        Rig::notification("NUC68", Rig::session_b),
                        Rig::notification("NUC309", old_ps4_session),
                        Rig::notification("NUC527", Rig::session_c)})}});
  rig.knows_the_friend();
  rig.reads_sessions({{200, Rig::sessions({Rig::session(
                                Rig::session_a, Rig::player("111", "Me"))})}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 2U);
  EXPECT_EQ(found->front().session_id, old_ps4_session);
  EXPECT_EQ(found->back().session_id, Rig::session_c);
  EXPECT_EQ(found->back().kind, GameInvitation::Kind::party);
  EXPECT_EQ(rig.sessions_read().header("X-PSN-SESSION-MANAGER-SESSION-IDS"),
            std::string{Rig::session_a} + "," + std::string{Rig::session_b});
}

TEST_F(InvitationsListed, KeepEveryInvitationWhenTheSessionsCannotBeRead) {
  rig.lists({{200, Rig::notifications(
                       {Rig::notification("NUC3", Rig::session_a),
                        Rig::notification("NUC68", Rig::session_b)})}});
  rig.knows_the_friend();
  rig.reads_sessions({{500, "{}"}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 2U);
  EXPECT_EQ(found->front().joining, Joining::joinable);
}

TEST_F(InvitationsListed, ReadTheSessionsInBatches) {
  std::vector<std::string> ids;
  std::vector<std::string> listed;
  for (int index = 0; index < 12; ++index) {
    auto id = std::string{Rig::session_a};
    id.replace(0, 2, (index < 10 ? "0" : "") + std::to_string(index));
    ids.push_back(id);
    listed.push_back(Rig::notification("NUC3", id));
  }
  rig.lists({{200, Rig::notifications(listed)}});
  rig.knows_the_friend();
  rig.reads_sessions(
      {{200,
        Rig::sessions({Rig::session(ids[0], Rig::player("222", "Friend"))})},
       {200,
        Rig::sessions({Rig::session(ids[11], Rig::player("222", "Friend"))})}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 2U);
  EXPECT_EQ(found->front().session_id, ids[0]);
  EXPECT_EQ(found->back().session_id, ids[11]);
  EXPECT_EQ(rig.account.http.count("playerSessions"), 2);
}

TEST_F(InvitationsListed, ShowPartiesAsParties) {
  rig.lists(
      {{200, Rig::notifications({Rig::notification("NUC527", Rig::session_b),
                                 Rig::notification("NUC3", Rig::session_a)})}});
  rig.knows_the_friend();
  rig.reads_sessions(
      {{200, Rig::sessions({Rig::session(Rig::session_a,
                                         Rig::player("222", "Friend"))})}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 2U);
  EXPECT_EQ(found->front().kind, GameInvitation::Kind::party);
  EXPECT_EQ(found->front().session_id, Rig::session_b);
  EXPECT_EQ(found->front().inviter, "Friend");
  EXPECT_EQ(found->front().joining, Joining::joinable);
  EXPECT_EQ(found->back().kind, GameInvitation::Kind::game);
}

TEST_F(InvitationsListed, AreDeletedFromTheNotificationListWhenCleared) {
  rig.lists({{200, Rig::notifications(
                       {Rig::notification("NUC3", Rig::session_a),
                        Rig::notification("NUC68", Rig::session_a)})}});
  rig.knows_the_friend();
  rig.account.http.on(
      "PUT",
      "https://m.np.playstation.com/api/userNotificationManager/"
      "v1/users/me/notifications",
      {{200, "{}"}});
  const auto found = rig.account.consoles.game_invitations(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 1U);
  EXPECT_EQ(found->front().notification_ids,
            (std::vector<std::string>{"n-1111", "n-1111"}));
  const std::vector<std::string> ids{"n-1111", "n-2222"};
  ASSERT_TRUE(rig.account.consoles.dismiss_invitations(*rig.user, ids, {}));
  const auto& put = rig.account.http.seen.back();
  EXPECT_EQ(put.method, "PUT");
  EXPECT_EQ(put.body,
            "{\"notifications\":[{\"notificationId\":\"n-1111\",\"state\":"
            "\"Deleted\"},{\"notificationId\":\"n-2222\",\"state\":"
            "\"Deleted\"}]}");
  EXPECT_EQ(put.header("Authorization"), "Bearer " + std::string{Rig::token});
  const auto seen = rig.account.http.seen.size();
  EXPECT_TRUE(rig.account.consoles.dismiss_invitations(*rig.user, {}, {}));
  EXPECT_EQ(rig.account.http.seen.size(), seen);
}

class CloudJoin : public ::testing::Test {
 protected:
  void lists_the_session_with(std::string_view players) {
    rig.account.http.on(
        "GET", std::string{Rig::sessions_url},
        {{200, "{\"playerSessions\":[" + Rig::session(Rig::session_a, players) +
                   "]}"}});
  }

  Rig rig;
};

TEST_F(CloudJoin, SendsTheGameIntentToTheCloudConsole) {
  lists_the_session_with(Rig::player("222", "Friend"));
  rig.account.http.on(
      "GET", std::string{Rig::clients_url},
      {{200,
        "{\"clients\":[{\"duid\":\"AAAA\",\"device\":{\"name\":\"Den\"}},"
        "{\"duid\":\"CCCC\",\"deviceProperties\":[\"CLDK\"]}]}"}});
  rig.account.http.on(
      "POST", std::string{Rig::commands_url},
      {{202, "{\"commandId\":" + Rig::quoted(Rig::command_id) + "}"}});
  rig.account.http.on(
      "GET", Rig::poll_url(),
      {{200, "{\"commands\":[{\"commandStatus\":\"completed\"}]}"}});
  EXPECT_TRUE(rig.account.consoles.join_in_cloud(*rig.user, Rig::session_a,
                                                 nullptr, {}));
  EXPECT_TRUE(rig.account.http.unmatched.empty());
  const auto post =
      std::ranges::find(rig.account.http.seen, std::string{"POST"},
                        &test_support::psn::Seen::method);
  ASSERT_NE(post, rig.account.http.seen.end());
  EXPECT_EQ(post->body,
            "{\"commandDetail\":{\"platform\":\"PS5\",\"duid\":\"CCCC\","
            "\"commandType\":\"sendGameIntent\",\"parameters\":{\"gameIntent\":"
            "\"intent-token\"}}}");
  EXPECT_EQ(post->header("X-Psn-Can-Mobile-App-Type"), "PSAPP");
  const auto list =
      std::ranges::find(rig.account.http.seen, std::string{Rig::clients_url},
                        &test_support::psn::Seen::url);
  ASSERT_NE(list, rig.account.http.seen.end());
  EXPECT_EQ(list->header("X-Psn-Can-Mobile-App-Type"), "PSAPP");
  const auto poll = std::ranges::find(rig.account.http.seen, Rig::poll_url(),
                                      &test_support::psn::Seen::url);
  ASSERT_NE(poll, rig.account.http.seen.end());
  EXPECT_EQ(poll->header("X-Psn-Can-Mobile-App-Type"), "PSAPP");
}

TEST_F(CloudJoin, StopsWhenTheSessionHasEnded) {
  rig.account.http.on("GET", std::string{Rig::sessions_url},
                      {{200, "{\"playerSessions\":[]}"}});
  const auto joined = rig.account.consoles.join_in_cloud(
      *rig.user, Rig::session_a, nullptr, {});
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::session_ended);
}

TEST_F(CloudJoin, StopsWhenAlreadyJoined) {
  lists_the_session_with(Rig::player("111", "Me"));
  const auto joined = rig.account.consoles.join_in_cloud(
      *rig.user, Rig::session_a, nullptr, {});
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::already_joined);
}

TEST_F(CloudJoin, RefusesAnInvalidSessionWithoutARequest) {
  const auto joined = rig.account.consoles.join_in_cloud(
      *rig.user, "not-a-session", nullptr, {});
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::invalid_session);
  EXPECT_TRUE(rig.account.http.seen.empty());
}

TEST(InvitationsSignedOut, TurnAwayAHomeUser) {
  Account account;
  const auto home = account.home_user(40);
  const auto found = account.consoles.game_invitations(*home, {});
  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().reason,
            libchiaking::users::Unanswered::Reason::signed_out);
  const auto joined =
      account.consoles.join_in_cloud(*home, Rig::session_a, nullptr, {});
  ASSERT_FALSE(joined);
  EXPECT_EQ(joined.error().reason, Reason::unanswered);
  EXPECT_TRUE(account.http.seen.empty());
}

}  // namespace
}  // namespace consoles_test
