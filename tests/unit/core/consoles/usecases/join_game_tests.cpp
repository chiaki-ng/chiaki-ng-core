#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::InvitationJoinFailure;
using Why = InvitationJoinFailure::Reason;

class GameJoinAtHome : public ::testing::Test {
 protected:
  static constexpr std::string_view sessions_url =
      "https://m.np.playstation.com/api/sessionManager/v1/playerSessions"
      "?fields=sessionId,createdTimestamp,maxPlayers,member(players),"
      "member(nonPsnPlayers),member(spectators),supportedPlatforms,"
      "sessionName,leader,joinableUserType,invitableUserType,"
      "tokenizedPlayerGameIntent,tokenizedSpectatorGameIntent";

  void lists_only_the_living_room() {
    rig.clients(0, {{200, LibraryRig::clients_page({LibraryRig::listed_console(
                              LibraryRig::uid('a'), "Living Room")})}});
    rig.games(0, {{200, LibraryRig::gamelist({})}});
  }

  LibraryRig rig;
  std::shared_ptr<Console> living =
      rig.account.paired(*rig.user, LibraryRig::den, "Living Room");
};

TEST_F(GameJoinAtHome, SendsTheSessionsGameIntentToAHomePs5) {
  lists_only_the_living_room();
  rig.http.on("GET", std::string{sessions_url},
              {{200, "{\"playerSessions\":[{\"sessionId\":\"" +
                         std::string{LibraryRig::party_session} +
                         "\",\"maxPlayers\":4,\"member\":{\"players\":[{"
                         "\"accountId\":\"222\"}]},"
                         "\"tokenizedPlayerGameIntent\":\"home-intent\"}]}"}});
  const std::string commands{LibraryRig::commands_url};
  rig.http.on("POST", commands, {{202, "{\"commandId\":\"game-command\"}"}});
  rig.http.on("GET",
              commands +
                  "?platform=PS5&commandIds=game-command&includeFields="
                  "actions,commandDetail&commandStatus=executing,completed",
              {{200, "{\"commands\":[{\"commandStatus\":\"completed\"}]}"}});
  EXPECT_TRUE(rig.account.consoles.join_on(
      *living, *rig.user, LibraryRig::party_session, nullptr, {}));
  const auto post = std::ranges::find(rig.http.seen, std::string{"POST"},
                                      &test_support::psn::Seen::method);
  ASSERT_NE(post, rig.http.seen.end());
  EXPECT_EQ(post->body,
            "{\"commandDetail\":{\"platform\":\"PS5\",\"duid\":\"" +
                LibraryRig::uid('a') +
                "\",\"commandType\":\"sendGameIntent\",\"parameters\":{"
                "\"gameIntent\":\"home-intent\"}}}");
}

TEST_F(GameJoinAtHome, RefusesAnInvalidSession) {
  lists_only_the_living_room();
  const auto invalid = rig.account.consoles.join_on(
      *living, *rig.user, "not a session", nullptr, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().reason, Why::invalid_session);
}

}  // namespace
}  // namespace consoles_test
