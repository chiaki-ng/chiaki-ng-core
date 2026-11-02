#include "tests/unit/core/consoles/usecases/invitations_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::GamePlatform;
using libchiaking::consoles::InvitationTarget;
namespace psn = libchiaking::psn;
using Rig = InvitationsRig;

class SessionSharing : public ::testing::Test {
 protected:
  static constexpr std::string_view own_sessions_url =
      "https://m.np.playstation.com/api/sessionManager/v1/users/111/"
      "playerSessions?relatedCreatorNpTitleIdFilter=PPSA01234_00&"
      "platformFilter=PS5&deviceType=PS5";
  static constexpr std::string_view profile_base =
      "https://m.np.playstation.com/api/userProfile";

  static auto invitations_url(std::string_view kind, std::string_view session)
      -> std::string {
    return "https://m.np.playstation.com/api/sessionManager/" +
           std::string{kind} + "/" + std::string{session} + "/invitations";
  }

  Rig rig;
};

TEST_F(SessionSharing, FindsTheUsersOwnGameSessionForTheTitle) {
  rig.account.http.on("GET", std::string{own_sessions_url},
                      {{200, "{\"playerSessions\":[{\"sessionId\":" +
                                 Rig::quoted(Rig::session_a) + "}]}"}});
  rig.reads_sessions(
      {{200,
        "{\"playerSessions\":[{\"sessionId\":" + Rig::quoted(Rig::session_a) +
            ",\"sessionName\":\"Squad\",\"maxPlayers\":4,"
            "\"joinableUserType\":\"ANYONE\",\"member\":{\"players\":[" +
            Rig::player("111", "Me") + "]}}]}"}});
  const auto own = rig.account.consoles.own_game_session(
      *rig.user, *libchiaking::NpTitleId::parse("PPSA01234_00"),
      GamePlatform::ps5, {});
  ASSERT_TRUE(own);
  ASSERT_TRUE(own->has_value());
  EXPECT_EQ((*own)->session_id, Rig::session_a);
  EXPECT_EQ((*own)->name, "Squad");
  EXPECT_EQ((*own)->players, 1U);
  EXPECT_EQ((*own)->max_players, 4U);
  EXPECT_TRUE((*own)->open_to_anyone);
  EXPECT_TRUE(rig.account.http.unmatched.empty());
}

TEST_F(SessionSharing, FindsNoSessionWhenTheUserPlaysNone) {
  rig.account.http.on("GET", std::string{own_sessions_url},
                      {{200, "{\"playerSessions\":[]}"}});
  const auto own = rig.account.consoles.own_game_session(
      *rig.user, *libchiaking::NpTitleId::parse("PPSA01234_00"),
      GamePlatform::ps5, {});
  ASSERT_TRUE(own);
  EXPECT_FALSE(own->has_value());
  EXPECT_EQ(rig.account.http.count("playerSessions?fields"), 0);
}

TEST_F(SessionSharing, ReadsAShareLinkFromTheShareService) {
  rig.account.http.on(
      "GET",
      "https://m.np.playstation.com/api/cpss/v1/invite/" +
          std::string{Rig::session_a},
      {{200,
        "{\"sessionJoinUrl\":\"https://example.invalid/join\","
        "\"sessionJoinImageUrl\":\"https://example.invalid/join.png\"}"}});
  const auto link =
      rig.account.consoles.share_game_session(*rig.user, Rig::session_a, {});
  ASSERT_TRUE(link);
  EXPECT_EQ(link->url, "https://example.invalid/join");
  EXPECT_EQ(link->image_url, "https://example.invalid/join.png");
  EXPECT_EQ(rig.account.http.seen.front().header("Authorization"),
            "Bearer " + std::string{Rig::token});
}

TEST_F(SessionSharing, InvitesFriendsToAPartyAndAGame) {
  const auto party_url = invitations_url("v2/partySessions", Rig::session_b);
  const auto game_url = invitations_url("v1/playerSessions", Rig::session_a);
  rig.account.http.on("POST", party_url, {{201, "{}"}});
  rig.account.http.on("POST", game_url, {{201, "{}"}});
  const std::vector<AccountId> friends{AccountId{222}, AccountId{333}};
  EXPECT_TRUE(rig.account.consoles.invite_friends(
      *rig.user,
      InvitationTarget{InvitationTarget::Kind::party,
                       std::string{Rig::session_b}},
      friends, {}));
  EXPECT_TRUE(rig.account.consoles.invite_friends(
      *rig.user,
      InvitationTarget{InvitationTarget::Kind::game,
                       std::string{Rig::session_a}},
      friends, {}));
  const auto& party = rig.account.http.seen.front();
  EXPECT_EQ(party.body,
            "{\"invitations\":[{\"to\":{\"accountId\":\"222\"}},{\"to\":{"
            "\"accountId\":\"333\"}}],\"invitationType\":\"NORMAL\"}");
  EXPECT_EQ(party.header("Content-Type"), "application/json; charset=utf-8");
  EXPECT_EQ(rig.account.http.seen.back().body,
            "{\"invitations\":[{\"to\":{\"accountId\":\"222\"}},{\"to\":{"
            "\"accountId\":\"333\"}}]}");
}

TEST_F(SessionSharing, InvitesManyFriendsInBatches) {
  rig.account.http.on("POST",
                      invitations_url("v2/partySessions", Rig::session_b),
                      {{201, "{}"}, {201, "{}"}});
  std::vector<AccountId> friends;
  for (std::uint64_t index = 0; index < 17; ++index)
    friends.emplace_back(1000 + index);
  EXPECT_TRUE(rig.account.consoles.invite_friends(
      *rig.user,
      InvitationTarget{InvitationTarget::Kind::party,
                       std::string{Rig::session_b}},
      friends, {}));
  EXPECT_EQ(rig.account.http.count("/invitations"), 2);
}

TEST_F(SessionSharing, RefusesAnUnsafeSessionWithoutARequest) {
  const std::vector<AccountId> friends{AccountId{222}};
  EXPECT_FALSE(rig.account.consoles.invite_friends(
      *rig.user, InvitationTarget{InvitationTarget::Kind::party, "../x"},
      friends, {}));
  EXPECT_TRUE(rig.account.http.seen.empty());
}

TEST_F(SessionSharing, ListsFriendsWithTheirProfilesAndPresence) {
  const std::string base{profile_base};
  rig.account.http.on(
      "GET",
      base +
          "/v2/internal/users/me/friends?limit=100&offset=0&order="
          "availability%2BrealName%2BonlineId&platforms=PS4%2CPS5",
      {{200, "{\"friends\":[\"222\",\"333\"],\"totalItemCount\":2}"}});
  rig.account.http.on(
      "GET", base + "/v1/internal/users/profiles?accountIds=222%2C333",
      {{200,
        "{\"profiles\":[{\"onlineId\":\"Away\",\"avatars\":[{\"size\":"
        "\"s\",\"url\":\"https://example.invalid/s.png\"},{\"size\":\"m\","
        "\"url\":\"https://example.invalid/m.png\"}]},{\"onlineId\":"
        "\"Here\"}]}"}});
  rig.account.http.on(
      "GET",
      base +
          "/v2/internal/users/basicPresences?accountIds=222%2C333&platforms="
          "PS4%2CPS5&type=primary&withOwnGameTitleInfo=true",
      {{200,
        "{\"basicPresences\":[{\"primaryPlatformInfo\":{\"onlineStatus\":"
        "\"offline\"}},{\"primaryPlatformInfo\":{\"onlineStatus\":"
        "\"online\"},\"gameTitleInfoList\":[{\"titleName\":\"Astro\"}]}]}"}});
  const auto found = rig.account.consoles.friends(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 2U);
  EXPECT_EQ(found->front().account_id, AccountId{333});
  EXPECT_EQ(found->front().online_id, "Here");
  EXPECT_TRUE(found->front().online);
  EXPECT_EQ(found->front().playing, "Astro");
  EXPECT_EQ(found->back().online_id, "Away");
  EXPECT_FALSE(found->back().online);
  EXPECT_EQ(found->back().avatar_url, "https://example.invalid/m.png");
  EXPECT_TRUE(rig.account.http.unmatched.empty());
}

TEST_F(SessionSharing, ListsFriendsOfflineWhenTheirPresenceIsRefused) {
  const std::string base{profile_base};
  rig.account.http.on(
      "GET",
      base +
          "/v2/internal/users/me/friends?limit=100&offset=0&order="
          "availability%2BrealName%2BonlineId&platforms=PS4%2CPS5",
      {{200, "{\"friends\":[\"222\"]}"}});
  rig.account.http.on("GET",
                      base + "/v1/internal/users/profiles?accountIds=222",
                      {{200, "{\"profiles\":[{\"onlineId\":\"Away\"}]}"}});
  rig.account.http.on(
      "GET",
      base +
          "/v2/internal/users/basicPresences?accountIds=222&platforms="
          "PS4%2CPS5&type=primary&withOwnGameTitleInfo=true",
      {{403, "{}"}, {403, "{}"}});
  const auto found = rig.account.consoles.friends(*rig.user, {});
  ASSERT_TRUE(found);
  ASSERT_EQ(found->size(), 1U);
  EXPECT_EQ(found->front().online_id, "Away");
  EXPECT_FALSE(found->front().online);
}

}  // namespace
}  // namespace consoles_test
