#include "tests/unit/core/messages/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace messages_test {
namespace {

using ChatMessage = Rig::ChatMessage;
using Reason = Rig::Reason;
using Reply = Rig::Reply;

class MessageSending : public ::testing::Test {
 protected:
  void answers_sends() {
    rig.http.on("POST", Rig::send_url(),
                {Reply{200, R"({"messageUid":"m-9"})"}});
  }

  static auto repeated(std::string_view character, std::size_t times)
      -> std::string {
    std::string text;
    for (std::size_t i = 0; i < times; ++i) text += character;
    return text;
  }

  Rig rig;
};

TEST_F(MessageSending, ReturnsTheTrimmedTextAsTheUsers) {
  answers_sends();
  const auto sent =
      rig.messages.send(*rig.user, "g-1", "t-1", "  hi there ", {});
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent->id, "m-9");
  EXPECT_EQ(sent->text, "hi there");
  EXPECT_TRUE(sent->from_user);
  EXPECT_EQ(sent->kind, ChatMessage::Kind::text);
  EXPECT_EQ(sent->sent,
            Rig::at(static_cast<std::int64_t>(Rig::wall_start_us / 1000)));
  ASSERT_FALSE(rig.http.seen.empty());
  const auto& post = rig.http.seen.back();
  EXPECT_EQ(post.method, "POST");
  EXPECT_EQ(post.body, R"({"messageType":1,"body":"hi there"})");
  EXPECT_EQ(post.header("Content-Type"), "application/json; charset=utf-8");
}

TEST_F(MessageSending, RefusesBlankOverlongOrMalformedTextWithoutAsking) {
  answers_sends();
  const auto blank = rig.messages.send(*rig.user, "g-1", "t-1", "   ", {});
  ASSERT_FALSE(blank);
  EXPECT_EQ(blank.error().reason, Reason::rejected);
  const auto long_text = std::string(Rig::Messages::longest_text + 1, 'a');
  EXPECT_FALSE(rig.messages.send(*rig.user, "g-1", "t-1", long_text, {}));
  EXPECT_FALSE(rig.messages.send(*rig.user, "g-1", "t-1", "\xff\xfe", {}));
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(MessageSending, MeasuresTheLongestTextInCharactersRatherThanBytes) {
  answers_sends();
  const auto wide = repeated("\xe3\x81\x82", Rig::Messages::longest_text);
  EXPECT_TRUE(rig.messages.send(*rig.user, "g-1", "t-1", wide, {}));
  EXPECT_EQ(rig.http.seen.size(), 1U);
  const auto emoji =
      repeated("\xf0\x9f\x98\x80", Rig::Messages::longest_text / 2 + 1);
  EXPECT_FALSE(rig.messages.send(*rig.user, "g-1", "t-1", emoji, {}));
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(MessageSending, RejectsAForbiddenMessageWithoutRenewingTheToken) {
  rig.http.on(
      "POST", Rig::send_url(),
      {Reply{403, R"({"error":{"code":2416389,"message":"Forbidden"}})"}});
  const auto sent = rig.messages.send(*rig.user, "g-1", "t-1", "hi", {});
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error().reason, Reason::rejected);
  EXPECT_EQ(sent.error().http_status, 403);
  EXPECT_EQ(rig.http.seen.size(), 1U);
  rig.http.on("GET", Rig::messages_url(), {Reply{200, R"({"messages":[]})"}});
  EXPECT_TRUE(rig.messages.thread(*rig.user, "g-1", "t-1", "", {}));
  ASSERT_FALSE(rig.http.seen.empty());
  EXPECT_EQ(rig.http.seen.back().header("Authorization"),
            Rig::bearer(Rig::app_token));
}

}  // namespace
}  // namespace messages_test
