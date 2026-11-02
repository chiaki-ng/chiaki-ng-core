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

class PartyJoin : public ::testing::Test {
 protected:
  void lists_only_the_living_room() {
    rig.clients(0, {{200, LibraryRig::clients_page({LibraryRig::listed_console(
                              LibraryRig::uid('a'), "Living Room")})}});
    rig.games(0, {{200, LibraryRig::gamelist({})}});
  }

  LibraryRig rig;
  std::shared_ptr<Console> living =
      rig.account.paired(*rig.user, LibraryRig::den, "Living Room");
};

TEST_F(PartyJoin, IsRefusedForAConsolePsDoesNotList) {
  const auto bedroom =
      rig.account.paired(*rig.user, LibraryRig::attic, "Bedroom");
  rig.clients(
      0,
      {{200,
        LibraryRig::clients_page(
            {LibraryRig::listed_console(LibraryRig::uid('a'), "Living Room"),
             LibraryRig::listed_console(LibraryRig::uid('c'), "Kitchen")})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  ASSERT_TRUE(living->titles(*rig.user, {}));
  const auto unparsed = rig.account.consoles.join_party_on(
      *bedroom, *rig.user, "not a session", nullptr, {});
  ASSERT_FALSE(unparsed);
  EXPECT_EQ(unparsed.error().reason, Why::invalid_session);
  const auto party = rig.account.consoles.join_party_on(
      *bedroom, *rig.user, LibraryRig::party_session, nullptr, {});
  ASSERT_FALSE(party);
  EXPECT_EQ(party.error().reason, Why::console_unavailable);
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

TEST_F(PartyJoin, IsRefusedWhenPsnForbidsIt) {
  rig.account.renews_store_access(LibraryRig::renewed_token);
  lists_only_the_living_room();
  rig.http.on(
      "POST", std::string{LibraryRig::commands_url},
      {{403, "{\"error\":{\"code\":2330755,\"message\":\"Forbidden\"}}"},
       {403, "{\"error\":{\"code\":2330755,\"message\":\"Forbidden\"}}"}});
  const auto forbidden = rig.account.consoles.join_party_on(
      *living, *rig.user, LibraryRig::party_session, nullptr, {});
  ASSERT_FALSE(forbidden);
  EXPECT_EQ(forbidden.error().reason, Why::refused);
  EXPECT_EQ(rig.http.count("/commands"), 2);
}

TEST_F(PartyJoin, SendsTheSessionToAHomePs5) {
  lists_only_the_living_room();
  const std::string commands{LibraryRig::commands_url};
  rig.http.on("POST", commands, {{202, "{\"commandId\":\"party-command\"}"}});
  rig.http.on("GET",
              commands +
                  "?platform=PS5&commandIds=party-command&includeFields="
                  "actions,commandDetail&commandStatus=executing,completed",
              {{200, "{\"commands\":[{\"commandStatus\":\"completed\"}]}"}});
  EXPECT_TRUE(rig.account.consoles.join_party_on(
      *living, *rig.user, LibraryRig::party_session, nullptr, {}));
  const auto post = std::ranges::find(rig.http.seen, std::string{"POST"},
                                      &test_support::psn::Seen::method);
  ASSERT_NE(post, rig.http.seen.end());
  EXPECT_EQ(post->body,
            "{\"commandDetail\":{\"platform\":\"PS5\",\"duid\":\"" +
                LibraryRig::uid('a') +
                "\",\"commandType\":\"party\",\"parameters\":{\"type\":"
                "\"sessionSharing\",\"typeParams\":{\"data\":\"{"
                "\\\"sessionId\\\":"
                "\\\"" +
                std::string{LibraryRig::party_session} + "\\\"}\"}}}}");
  EXPECT_EQ(post->header("X-Psn-Can-Mobile-App-Type"), "PSAPP");
}

TEST_F(PartyJoin, AnswersAHomeUserSignedOut) {
  lists_only_the_living_room();
  const auto home = rig.account.home_user(40);
  const auto signed_out = rig.account.consoles.join_party_on(
      *living, *home, LibraryRig::party_session, nullptr, {});
  ASSERT_FALSE(signed_out);
  EXPECT_EQ(signed_out.error().reason, Why::unanswered);
}

TEST_F(PartyJoin, RefusesAnInvalidSession) {
  lists_only_the_living_room();
  const auto invalid = rig.account.consoles.join_party_on(
      *living, *rig.user, "not a session", nullptr, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().reason, Why::invalid_session);
}

}  // namespace
}  // namespace consoles_test
