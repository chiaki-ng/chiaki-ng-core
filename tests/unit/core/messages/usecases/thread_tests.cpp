#include "tests/unit/core/messages/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace messages_test {
namespace {

using ChatMessage = Rig::ChatMessage;
using Reason = Rig::Reason;
using Reply = Rig::Reply;

class ChatThread : public ::testing::Test {
 protected:
  Rig rig;
};

TEST_F(ChatThread, ReadsOldestFirstAndKnowsItsStart) {
  rig.http.on(
      "GET", Rig::messages_url(),
      {Reply{
          200,
          R"({"messages":[)"
          R"({"messageUid":"m-3","messageType":1,"body":"later","createdTimestamp":"3000",)"
          R"("sender":{"accountId":"7","onlineId":"player"}},)"
          R"({"messageUid":"m-2","messageType":1013,"body":"","createdTimestamp":"2000",)"
          R"("sender":{"accountId":"9","onlineId":"Friend"}},)"
          R"({"messageUid":"m-1","messageType":8,"body":"see this","createdTimestamp":"1000",)"
          R"("sender":{"accountId":"9","onlineId":"Friend"}},)"
          R"({"messageUid":"m-0","messageType":2006,"createdTimestamp":"500"}]})"}});
  const auto thread = rig.messages.thread(*rig.user, "g-1", "t-1", "", {});
  ASSERT_TRUE(thread);
  EXPECT_TRUE(thread->complete);
  ASSERT_EQ(thread->messages.size(), 3U);
  const auto& messages = thread->messages;
  EXPECT_EQ(messages[0].id, "m-1");
  EXPECT_EQ(messages[0].text, "see this");
  EXPECT_EQ(messages[1].kind, ChatMessage::Kind::other);
  EXPECT_TRUE(messages[1].text.empty());
  EXPECT_TRUE(messages[2].from_user);
  EXPECT_EQ(messages[2].text, "later");
  EXPECT_EQ(messages[2].sent, Rig::at(3000));
}

TEST_F(ChatThread, ReadsAnOlderPageBeforeAMessage) {
  rig.http.on("GET", Rig::messages_url() + "&before=m-1&limit=50",
              {Reply{200, R"({"messages":[]})"}});
  const auto thread = rig.messages.thread(*rig.user, "g-1", "t-1", "m-1", {});
  ASSERT_TRUE(thread);
  EXPECT_TRUE(thread->messages.empty());
  EXPECT_FALSE(thread->complete);
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(ChatThread, RejectsAMissingGroupWithoutAsking) {
  const auto empty = rig.messages.thread(*rig.user, "", "t-1", "", {});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error().reason, Reason::rejected);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ChatThread, KeepsUnusualMessagesWithoutText) {
  rig.http.on(
      "GET", Rig::messages_url(),
      {Reply{
          200,
          R"({"messages":[)"
          R"({"messageUid":"a","messageType":-100,"body":"x","createdTimestamp":"1"},)"
          R"({"messageUid":"b","messageType":"1","body":"y","createdTimestamp":"2"},)"
          R"({"messageUid":"c","messageType":1013,"createdTimestamp":"3"},)"
          R"({"messageUid":"d","messageType":3,"body":"photo caption","createdTimestamp":"4",)"
          R"("sender":{"accountId":7}},)"
          R"("broken",)"
          R"({"messageUid":"e","messageType":1,"body":"   ","createdTimestamp":"5"}]})"}});
  const auto thread = rig.messages.thread(*rig.user, "g-1", "t-1", "", {});
  ASSERT_TRUE(thread);
  ASSERT_EQ(thread->messages.size(), 5U);
  EXPECT_FALSE(thread->complete);
  const auto& messages = thread->messages;
  EXPECT_EQ(messages[0].kind, ChatMessage::Kind::other);
  EXPECT_TRUE(messages[0].text.empty());
  EXPECT_EQ(messages[1].kind, ChatMessage::Kind::other);
  EXPECT_EQ(messages[2].kind, ChatMessage::Kind::other);
  EXPECT_EQ(messages[3].kind, ChatMessage::Kind::text);
  EXPECT_EQ(messages[3].text, "photo caption");
  EXPECT_TRUE(messages[3].from_user);
  EXPECT_TRUE(messages[3].sender.empty());
  EXPECT_EQ(messages[4].kind, ChatMessage::Kind::other);
}

TEST_F(ChatThread, TrimsSendersAndTimesAndDropsUnreadableNumbers) {
  rig.http.on(
      "GET", Rig::messages_url(),
      {Reply{
          200,
          R"({"messages":[)"
          R"({"messageUid":"a","messageType":1,"body":"x","createdTimestamp":" 3000\t",)"
          R"("sender":{"accountId":" 7 "}},)"
          R"({"messageUid":"b","messageType":1,"body":"y","createdTimestamp":4000,)"
          R"("sender":{"accountId":-7}},)"
          R"({"messageUid":"c","messageType":1,"body":"z","createdTimestamp":1.5,)"
          R"("sender":{"accountId":"007"}},)"
          R"({"messageUid":"d","messageType":1,"body":"w","createdTimestamp":"-5",)"
          R"("sender":{"accountId":7e0}}]})"}});
  const auto thread = rig.messages.thread(*rig.user, "g-1", "t-1", "", {});
  ASSERT_TRUE(thread);
  ASSERT_EQ(thread->messages.size(), 4U);
  std::vector<std::string> senders;
  std::vector<bool> own;
  for (const auto& message : thread->messages) {
    senders.push_back(message.sender_account_id);
    own.push_back(message.from_user);
  }
  EXPECT_EQ(senders, (std::vector<std::string>{"7", "007", "7", ""}));
  EXPECT_EQ(own, (std::vector<bool>{true, false, true, false}));
  EXPECT_EQ(thread->messages[0].sent, Rig::at(-5));
  EXPECT_EQ(thread->messages[1].sent, Rig::at(0));
  EXPECT_EQ(thread->messages[2].sent, Rig::at(3000));
  EXPECT_EQ(thread->messages[3].sent, Rig::at(4000));
}

TEST_F(ChatThread, RenewsARefusedTokenOnceAndReadsAgain) {
  rig.renews_store_access();
  rig.http.on("GET", Rig::messages_url(),
              {Reply{401, R"({"error":{"code":2240526}})"},
               Reply{200, R"({"messages":[]})"}});
  EXPECT_TRUE(rig.messages.thread(*rig.user, "g-1", "t-1", "", {}));
  EXPECT_EQ(rig.http.requests_to(Rig::messages_url()), 2);
  ASSERT_FALSE(rig.http.seen.empty());
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            Rig::bearer(Rig::renewed_token));
}

}  // namespace
}  // namespace messages_test
