#include "tests/unit/core/messages/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace messages_test {
namespace {

using Reply = Rig::Reply;

class Conversations : public ::testing::Test {
 protected:
  void answers_a_group_and_a_direct_chat() {
    rig.http.on(
        "GET", Rig::groups_url("favorite"),
        {Reply{
            200,
            R"({"groups":[{"groupId":"g-1","groupType":0,"modifiedTimestamp":"1000",)"
            R"("members":[{"accountId":"7","onlineId":"player"},{"accountId":"9","onlineId":"Friend"}],)"
            R"("mainThread":{"threadId":"t-1","existsUnreadMessage":true,"latestMessage":)"
            R"({"messageUid":"m-1","messageType":1,"body":"hello","createdTimestamp":"900",)"
            R"("sender":{"accountId":"9","onlineId":"Friend"}}},"isFavorite":true}]})"}});
    rig.http.on(
        "GET", Rig::groups_url("notFavorite"),
        {Reply{
            200,
            R"({"groups":[{"groupId":"g-1","groupType":0},)"
            R"({"groupId":"g-2","groupType":1,"groupName":{"value":"Squad"},)"
            R"("modifiedTimestamp":2000,"members":[{"accountId":7},{"accountId":8,"onlineId":"Other"}],)"
            R"("mainThread":{"threadId":"t-2"}}]})"}});
  }

  Rig rig;
};

TEST_F(Conversations, ListsAGroupBeforeAnOlderDirectChat) {
  answers_a_group_and_a_direct_chat();
  const auto listed = rig.messages.conversations(*rig.user, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2U);
  const auto& group = listed->front();
  EXPECT_EQ(group.group_id, "g-2");
  EXPECT_TRUE(group.group);
  EXPECT_EQ(group.name, "Squad");
  EXPECT_EQ(group.thread_id, "t-2");
  EXPECT_EQ(group.updated, Rig::at(2000));
  ASSERT_EQ(group.members.size(), 1U);
  EXPECT_EQ(group.members.front().online_id, "Other");
  EXPECT_FALSE(group.unread);
  EXPECT_FALSE(group.latest);
}

TEST_F(Conversations, DescribeADirectChatWithoutTheUser) {
  answers_a_group_and_a_direct_chat();
  const auto listed = rig.messages.conversations(*rig.user, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed->size(), 2U);
  const auto& direct = listed->back();
  EXPECT_EQ(direct.group_id, "g-1");
  EXPECT_FALSE(direct.group);
  EXPECT_TRUE(direct.favorite);
  EXPECT_TRUE(direct.unread);
  ASSERT_TRUE(direct.latest);
  EXPECT_EQ(direct.latest->text, "hello");
  EXPECT_FALSE(direct.latest->from_user);
  EXPECT_EQ(direct.latest->sender, "Friend");
  ASSERT_EQ(direct.members.size(), 1U);
  EXPECT_EQ(direct.members.front().account_id, "9");
}

TEST_F(Conversations, AreReadWithTheStoreTokenAndALanguage) {
  answers_a_group_and_a_direct_chat();
  ASSERT_TRUE(rig.messages.conversations(*rig.user, {}));
  ASSERT_FALSE(rig.http.seen.empty());
  const auto& read = rig.http.seen.front();
  EXPECT_EQ(read.header("Authorization"), Rig::bearer(Rig::app_token));
  EXPECT_FALSE(read.header("Accept-Language").empty());
}

TEST_F(Conversations, AreEmptyWhenTheReplyHasNoList) {
  rig.http.on("GET", Rig::groups_url("favorite"), {Reply{200, "{}"}});
  rig.http.on("GET", Rig::groups_url("notFavorite"),
              {Reply{200, R"({"groups":null})"}});
  const auto listed = rig.messages.conversations(*rig.user, {});
  ASSERT_TRUE(listed);
  EXPECT_TRUE(listed->empty());
}

TEST_F(Conversations, ReadTheNextPageAfterAFullOne) {
  std::string full;
  for (std::size_t i = 0; i < Rig::page_size; ++i)
    full +=
        (full.empty() ? "" : ",") + Rig::group_entry("f-" + std::to_string(i));
  rig.http.on("GET", Rig::groups_url("favorite"),
              {Reply{200, R"({"groups":[)" + full + "]}"}});
  rig.http.on(
      "GET", Rig::groups_url("favorite", Rig::page_size),
      {Reply{200, R"({"groups":[)" + Rig::group_entry("f-last") + "]}"}});
  rig.http.on("GET", Rig::groups_url("notFavorite"),
              {Reply{200, R"({"groups":[]})"}});
  const auto listed = rig.messages.conversations(*rig.user, {});
  ASSERT_TRUE(listed);
  EXPECT_EQ(listed->size(), Rig::page_size + 1);
  EXPECT_TRUE(std::ranges::any_of(*listed, [](const auto& conversation) {
    return conversation.group_id == "f-last";
  }));
  EXPECT_EQ(rig.http.seen.size(), 3U);
}

}  // namespace
}  // namespace messages_test
