#include "tests/unit/core/messages/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace messages_test {
namespace {

using Reply = Rig::Reply;

class ChatIds : public ::testing::Test {
 protected:
  Rig rig;
};

TEST_F(ChatIds, AreReadAsPsWritesThem) {
  const std::string thread =
      std::string{Rig::base} +
      "/v1/members/me/groups/~25C2A5.1/threads/"
      "~25C2A5.1/messages?sharedTypes=profile,party&"
      "includeFields=sharedUrlInfo&before=m%2F1&limit=50";
  rig.http.on("GET", thread, {Reply{200, R"({"messages":[]})"}});
  EXPECT_TRUE(
      rig.messages.thread(*rig.user, "~25C2A5.1", "~25C2A5.1", "m/1", {}));
  EXPECT_TRUE(rig.http.unmatched.empty());
}

TEST_F(ChatIds, AreEncodedWhereNeededWhenSending) {
  rig.http.on(
      "POST",
      std::string{Rig::base} + "/v1/groups/g%2F1/threads/t%201/messages",
      {Reply{200, R"({"messageUid":"m-2"})"}});
  EXPECT_TRUE(rig.messages.send(*rig.user, "g/1", "t 1", "hi", {}));
  EXPECT_TRUE(rig.http.unmatched.empty());
}

}  // namespace
}  // namespace messages_test
