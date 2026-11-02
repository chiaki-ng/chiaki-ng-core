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

class CustomData1 : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 16> zeros{};

  static auto data(std::string_view text) {
    return Notification::parse(text).custom_data1();
  }

  static auto body_of(std::string_view value) -> std::string {
    return std::string{
               R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":")"} +
           std::string{value} + R"("}}})";
  }

  template <class Reason>
  static auto refused_as(std::string_view text) -> testing::AssertionResult {
    const auto read = data(text);
    if (read) return testing::AssertionFailure() << text << " was read";
    if (!read.error().template is<Reason>())
      return testing::AssertionFailure()
             << text << " was refused as " << read.error().name();
    return testing::AssertionSuccess();
  }
};

TEST_F(CustomData1, IsTheBase64OfTheBase64OfSixteenBytes) {
  EXPECT_EQ(data(body_of("QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09")).value(), zeros);
  const std::array<std::byte, 16> expected{
      std::byte{0x00}, std::byte{0x11}, std::byte{0xaa}, std::byte{0xbb},
      std::byte{0x22}, std::byte{0x33}, std::byte{0xcc}, std::byte{0xdd},
      std::byte{0x44}, std::byte{0x55}, std::byte{0xee}, std::byte{0xff},
      std::byte{0x66}, std::byte{0x77}, std::byte{0x88}, std::byte{0x99}};
  EXPECT_EQ(data(body_of("QUJHcXV5SXp6TjFFVmU3L1puZUltUT09")).value(),
            expected);
  const auto one = data(body_of("RVJFUkVSRVJFUkVSRVJFUkVSRVJFUT09"));
  ASSERT_TRUE(one);
  for (const auto byte : one.value()) EXPECT_EQ(byte, std::byte{0x11});
}

TEST_F(CustomData1, IsReadWhateverTheKeyOrderOrExtraFields) {
  EXPECT_EQ(
      data(
          R"({"body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09","extra":[1,null]}},"dataType":"psn:sessionManager:sys:rps:customData1:updated"})")
          .value(),
      zeros);
  EXPECT_EQ(
      data(
          R"({"body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}},"dataType":"psn:sessionManager:sys:rps:customData1:updated"})")
          .value(),
      zeros);
}

TEST_F(CustomData1, IsReadFromTheLastDuplicateKey) {
  EXPECT_TRUE(data(
      R"({"body":{"data":{"customData1":"0000000000000000000000000000000g","customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}},"dataType":"psn:sessionManager:sys:rps:customData1:updated"})"));
}

TEST_F(CustomData1, IgnoresBytesBeyondTheSixteenth) {
  for (const std::string_view extra : {"QUFFQ0F3UUZCZ2NJQ1FvTERBME9EeEE9",
                                       "QUFFQ0F3UUZCZ2NJQ1FvTERBME9EeEFS"}) {
    const auto result = data(body_of(extra));
    ASSERT_TRUE(result) << extra;
    for (std::size_t i = 0; i < 16; ++i)
      EXPECT_EQ(result.value()[i], static_cast<std::byte>(i))
          << extra << " byte " << i;
  }
}

TEST_F(CustomData1, IsMissingFromOtherShapes) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(
      R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}}})"));
  for (
      const std::string_view bad :
      {R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated"})",
       R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{}})",
       R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":123}}})",
       R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":null}}})"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(bad));
}

TEST_F(CustomData1, RejectsTheWrongLengthOrAlphabet) {
  for (const std::string_view bad :
       {"0000000000000000000000000000000", "000000000000000000000000000000000",
        "0000000000000000000000000000000g"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidCustomData1>(body_of(bad)));
}

TEST_F(CustomData1, RejectsAnInvalidInnerEncoding) {
  for (const std::string_view invalid :
       {"CkFBQUFBQUFBQUFBQUFBQUFBQUFBQUE9", "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT0/",
        "00000000000000000000000000000000",
        "QUFFQ0F3UUZCZ2NJQ1FvTERBME9EeEFSRWc9PQ==",
        "Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/Pz8/", "QUFBQUFBQUFBQUFBQUFBQUFBQUEKCgoK",
        "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT0-"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidCustomData1>(body_of(invalid)));
}

TEST_F(CustomData1, RejectsInvalidJson) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidJson>(
      R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09")"));
}

}  // namespace
}  // namespace psn_test
