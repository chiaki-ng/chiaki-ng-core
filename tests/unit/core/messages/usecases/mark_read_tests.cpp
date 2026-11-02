#include "tests/unit/core/messages/rig.hpp"

#include <gtest/gtest.h>

namespace messages_test {
namespace {

using Reply = Rig::Reply;

class MarkingRead : public ::testing::Test {
 protected:
  Rig rig;
};

TEST_F(MarkingRead, MarksAConversationReadUpToAMessage) {
  rig.http.on("PATCH", Rig::thread_url("/v1/members/me/groups/"),
              {Reply{204, ""}});
  EXPECT_TRUE(rig.messages.mark_read(*rig.user, "g-1", "t-1", "m-3", {}));
  ASSERT_FALSE(rig.http.seen.empty());
  const auto& patch = rig.http.seen.back();
  EXPECT_EQ(patch.method, "PATCH");
  EXPECT_EQ(patch.body, R"({"readMessageUid":"m-3"})");
  EXPECT_EQ(rig.http.seen.size(), 1U);
}

TEST_F(MarkingRead, RefusesAMissingMessageWithoutAsking) {
  rig.http.on("PATCH", Rig::thread_url("/v1/members/me/groups/"),
              {Reply{204, ""}});
  EXPECT_FALSE(rig.messages.mark_read(*rig.user, "g-1", "t-1", "", {}));
  EXPECT_TRUE(rig.http.seen.empty());
}

}  // namespace
}  // namespace messages_test
