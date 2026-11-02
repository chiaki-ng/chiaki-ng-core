#include "src/core/psn/rest/push/push.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::Notification;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;

class SessionCreatedOnlineId : public ::testing::Test {
 protected:
  static constexpr std::string_view prefix =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":")";

  static auto online_id(std::string_view text) {
    return Notification::parse(text).session_created_online_id();
  }

  template <class Reason>
  static auto refused_as(std::string_view text) -> testing::AssertionResult {
    const auto read = online_id(text);
    if (read)
      return testing::AssertionFailure() << text << " gave " << read.value();
    if (!read.error().template is<Reason>())
      return testing::AssertionFailure()
             << text << " was refused as " << read.error().name();
    return testing::AssertionSuccess();
  }

  static auto named(const std::string& id) -> std::string {
    return std::string{prefix} + id + "\"}}";
  }
};

TEST_F(SessionCreatedOnlineId, IsReadFromTheRecipient) {
  EXPECT_EQ(
      online_id(
          R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":"user-42"}})")
          .value(),
      "user-42");
  EXPECT_EQ(
      online_id(
          R"({"to":{"onlineId":"name@_😀"},"dataType":"psn:sessionManager:sys:remotePlaySession:created"})")
          .value(),
      "name@_\xF0\x9F\x98\x80");
  EXPECT_EQ(
      online_id(
          R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","ignored":[1,null],"to":{"extra":true,"onlineId":"abc"}})")
          .value(),
      "abc");
}

TEST_F(SessionCreatedOnlineId, IsMissingFromOtherShapes) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","to":{"onlineId":"user-42"}})"));
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created"})"));
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{}})"));
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":123}})"));
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":null}})"));
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","dataType":"x"})"));
}

TEST_F(SessionCreatedOnlineId, IsAtMost256PrintableCharacters) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidOnlineId>(named("")));
  EXPECT_TRUE(
      refused_as<bad_reply::InvalidOnlineId>(named(std::string(257, 'a'))));
  EXPECT_EQ(online_id(named(std::string(256, 'a'))).value(),
            std::string(256, 'a'));
  EXPECT_TRUE(refused_as<bad_reply::InvalidOnlineId>(named(R"(bad\u0001id)")));
  EXPECT_TRUE(refused_as<bad_reply::InvalidOnlineId>(named(R"(bad\u007fid)")));
}

TEST_F(SessionCreatedOnlineId, RejectsInvalidJson) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidJson>("{"));
}

}  // namespace
}  // namespace psn_test
