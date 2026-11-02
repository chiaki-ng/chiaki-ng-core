#include "tests/unit/core/consoles/usecases/invitations_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::MessageAlert;
using libchiaking::consoles::PartyInvitationAlert;
using Reply = InvitationsRig::Reply;
using Rig = InvitationsRig;
using Sessions = InvitationsRig::Sessions;

class InvitationAlerts : public ::testing::Test {
 protected:
  Rig rig;
};

TEST_F(InvitationAlerts, AnnounceTheNewInvitationOnAPush) {
  rig.lists(
      {{200, Rig::notifications({Rig::notification("NUC3", Rig::session_a)})},
       {200, Rig::notifications({Rig::notification("NUC3", Rig::session_a),
                                 Rig::notification("NUC3", Rig::session_b)})}});
  PushServer server{
      {R"({"dataType":"np:service:gamingLoungeGroups"})",
       R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})"}};
  EXPECT_EQ(rig.watched(server), Sessions{std::string{Rig::session_b}});
  EXPECT_EQ(server.url, "wss://push.example.com/np/pushNotification");
  EXPECT_EQ(server.header("Authorization"),
            "Bearer " + std::string{Rig::token});
  EXPECT_EQ(server.header("X-PSN-APP-TYPE"), "MOBILE_APP.PSAPP");
  EXPECT_EQ(rig.account.http.count("userNotificationManager"), 2);
  EXPECT_EQ(rig.account.http.count("serveraddr"), 1);
}

TEST_F(InvitationAlerts, ReadAListThatLagsBehindThePushAgain) {
  const auto first =
      Rig::notifications({Rig::notification("NUC309", Rig::session_a)});
  rig.lists({{200, first},
             {200, first},
             {200, Rig::notifications(
                       {Rig::notification("NUC309", Rig::session_a),
                        Rig::notification("NUC309", Rig::session_b)})}});
  PushServer server{
      {R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})"}};
  EXPECT_EQ(rig.watched(server), Sessions{std::string{Rig::session_b}});
  EXPECT_EQ(rig.account.http.count("userNotificationManager"), 3);
}

TEST_F(InvitationAlerts, PassOnAMessageFromSomeoneElse) {
  rig.lists(
      {{200, Rig::notifications({Rig::notification("NUC3", Rig::session_a)})}});
  PushServer server{
      {R"({"dataType":"np:service:gamingLoungeGroups","to":{"onlineId":"player"},)"
       R"("body":{"extendedData":{"data":{"value":{"pushEventId":"1001",)"
       R"("groupId":"group-1","accountId":"222"}}}}})",
       R"({"dataType":"np:service:gamingLoungeGroups","from":{"onlineId":"player"},)"
       R"("body":{"extendedData":{"data":{"value":{"pushEventId":"101",)"
       R"("groupId":"group-1","threadId":"thread-1","messageUid":"m-1"}}}}})",
       R"({"dataType":"np:service:gamingLoungeGroups","from":{"onlineId":"Friend"},)"
       R"("body":{"extendedData":{"data":{"value":{"pushEventId":"101",)"
       R"("groupId":"group-2","threadId":"thread-2","messageUid":"m-2"}}}}})"}};
  Arrivals arrivals;
  rig.watch(server, arrivals, [&] { return arrivals.anything(); });
  EXPECT_EQ(arrivals.heard(), (std::vector<MessageAlert>{MessageAlert{
                                  "Friend", "group-2", "thread-2"}}));
  EXPECT_TRUE(arrivals.seen().empty());
  EXPECT_EQ(rig.account.http.count("userNotificationManager"), 1);
}

TEST_F(InvitationAlerts, AnnounceAPushedPartyInvitationOnce) {
  const auto before =
      Rig::notifications({Rig::notification("NUC3", Rig::session_a)});
  std::vector<Reply> replies(3, Reply{200, before});
  replies.push_back(
      {200, Rig::notifications({Rig::notification("NUC3", Rig::session_a),
                                Rig::notification("NUC3", Rig::session_b)})});
  rig.lists(std::move(replies));
  PushServer server{{Rig::invitation_push("party-1"),
                     Rig::invitation_push("party-1"),
                     Rig::invitation_push(Rig::session_a)}};
  Arrivals arrivals;
  rig.watch(server, arrivals, [&] { return !arrivals.seen().empty(); });
  EXPECT_EQ(arrivals.invited(), (std::vector<PartyInvitationAlert>{
                                    PartyInvitationAlert{"Host", "party-1"}}));
  EXPECT_EQ(arrivals.seen(), Sessions{std::string{Rig::session_b}});
  EXPECT_EQ(rig.account.http.count("userNotificationManager"), 4);
}

TEST_F(InvitationAlerts, AnnounceAListedPartyInvitationAsAParty) {
  rig.lists(
      {{200, Rig::notifications({Rig::notification("NUC3", Rig::session_a)})},
       {200,
        Rig::notifications({Rig::notification("NUC3", Rig::session_a),
                            Rig::notification("NUC527", Rig::session_b)})}});
  PushServer server{
      {R"({"dataType":"psn:sessionManager:sys:v2:ps:invitations:created"})"}};
  Arrivals arrivals;
  rig.watch(server, arrivals, [&] { return !arrivals.invited().empty(); });
  EXPECT_EQ(arrivals.invited(),
            (std::vector<PartyInvitationAlert>{
                PartyInvitationAlert{"Friend", std::string{Rig::session_b}}}));
  EXPECT_TRUE(arrivals.seen().empty());
}

TEST_F(InvitationAlerts, StillAlertForAPartyListedBeforeTheWatch) {
  const auto listed =
      Rig::notifications({Rig::notification("NUC527", Rig::session_b, "Read")});
  rig.lists({{200, listed}, {200, listed}});
  PushServer server{{Rig::invitation_push(Rig::session_b),
                     Rig::invitation_push(Rig::session_b)}};
  Arrivals arrivals;
  rig.watch(server, arrivals, [&] {
    return !arrivals.invited().empty() &&
           rig.account.http.count("userNotificationManager") >= 3;
  });
  EXPECT_EQ(arrivals.invited(),
            (std::vector<PartyInvitationAlert>{
                PartyInvitationAlert{"Host", std::string{Rig::session_b}}}));
  EXPECT_TRUE(arrivals.seen().empty());
}

}  // namespace
}  // namespace consoles_test
