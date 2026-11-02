#include "src/core/psn/rest/push/push.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::Notification;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;

class MemberDeviceUid : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 32> zeros{};

  static auto members_body(std::string_view uid) -> std::string {
    return std::string{
               R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":")"} +
           std::string{uid} + R"("}]}}})";
  }

  static auto uid(std::string_view text) {
    return Notification::parse(text).member_device_uid();
  }

  template <class Reason>
  static auto refused_as(std::string_view text) -> testing::AssertionResult {
    const auto read = uid(text);
    if (read) return testing::AssertionFailure() << text << " was read";
    if (!read.error().template is<Reason>())
      return testing::AssertionFailure()
             << text << " was refused as " << read.error().name();
    return testing::AssertionSuccess();
  }
};

TEST_F(MemberDeviceUid, IsReadAsHex) {
  EXPECT_EQ(uid(members_body(std::string(64, '0'))).value(), zeros);
  const auto mixed =
      uid(members_body(std::string(32, 'a') + std::string(32, 'F')));
  ASSERT_TRUE(mixed);
  for (std::size_t index = 0; index < 32; ++index)
    EXPECT_EQ(mixed.value()[index],
              index < 16 ? std::byte{0xaa} : std::byte{0xff})
        << index;
}

TEST_F(MemberDeviceUid, IsReadWhateverTheKeyOrder) {
  EXPECT_EQ(
      uid(R"({"body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}},"dataType":"psn:sessionManager:sys:rps:members:created"})")
          .value(),
      zeros);
}

TEST_F(MemberDeviceUid, IsSixtyFourHexDigits) {
  for (const auto& bad :
       {std::string(63, 'a'), std::string(65, 'a'), std::string(63, 'a') + "g",
        std::string(63, '0') + "z"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidDeviceUid>(members_body(bad)));
}

TEST_F(MemberDeviceUid, IsMissingFromOtherShapes) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}}})"));
  for (
      const std::string_view bad :
      {R"({"dataType":"psn:sessionManager:sys:rps:members:created"})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":[]}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[]}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":["x"]}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{}]}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":123}]}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":null}]}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:members:created","dataType":"x"})"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(bad));
}

TEST_F(MemberDeviceUid, RejectsInvalidJson) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidJson>("{"));
}

}  // namespace
}  // namespace psn_test
