#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::CloudAssistedNavigation;
using Reply = RestRig::Reply;

class StartRemotePlay : public ::testing::Test {
 protected:
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";
  static constexpr std::array<std::byte, 32> uid{
      std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
      std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
      std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
      std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff},
      std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
      std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
      std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
      std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}};
  static constexpr std::array<std::byte, 32> zero_uid{};
  static constexpr std::array<std::byte, 16> zero{};
  static constexpr std::array<std::byte, 16> mixed{
      std::byte{0},  std::byte{1},  std::byte{2},  std::byte{3},
      std::byte{4},  std::byte{5},  std::byte{6},  std::byte{7},
      std::byte{8},  std::byte{9},  std::byte{10}, std::byte{11},
      std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15}};

  RestRig rig;
  CloudAssistedNavigation navigation{rig.http, RestRig::bearer()};
};

TEST_F(StartRemotePlay, SendsTheCommandBodyExactly) {
  rig.fake.on("POST", std::string{commands_url}, {Reply{200, "{}"}});
  EXPECT_TRUE(navigation.start_remote_play(
      RestRig::session(RestRig::session_text), 1234567890, uid, zero, mixed));
  EXPECT_TRUE(navigation.start_remote_play(
      RestRig::session(RestRig::session_text),
      std::numeric_limits<std::int64_t>::min(), uid, zero, zero));
  EXPECT_TRUE(navigation.start_remote_play(
      RestRig::session("01234567-89AB-CDEF-0123-456789ABCDEF"), 1, uid, zero,
      zero));
  ASSERT_EQ(rig.fake.seen.size(), 3U);
  EXPECT_EQ(
      rig.fake.seen[0].body,
      R"json({"commandDetail":{"commandType":"remotePlay","duid":"00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff","messageDestination":"SQS","parameters":{"initialParams":"{\"accountId\":1234567890,\"roomId\":0,\"sessionId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"clientType\":\"Windows\",\"data1\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"data2\":\"AAECAwQFBgcICQoLDA0ODw==\"}"},"platform":"PS5"}})json");
  EXPECT_NE(rig.fake.seen[1].body.find(
                R"json(\"accountId\":-9223372036854775808)json"),
            std::string::npos)
      << rig.fake.seen[1].body;
  EXPECT_NE(
      rig.fake.seen[2].body.find(
          R"json(\"sessionId\":\"01234567-89AB-CDEF-0123-456789ABCDEF\")json"),
      std::string::npos)
      << rig.fake.seen[2].body;
  EXPECT_EQ(
      RestRig::headers_of(rig.fake.seen[0]),
      (std::vector<std::string>{"Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                                "Content-Type: application/json; charset=utf-8",
                                "User-Agent: RpNetHttpUtilImpl"}));
}

TEST_F(StartRemotePlay, KeepsTheReceiptAndPollsItsStatus) {
  rig.fake.on("POST", std::string{commands_url},
              {Reply{202, R"({"commandId":"synthetic/receipt"})"},
               Reply{202, R"({"commandId":42})"}, Reply{202, "malformed"}});
  rig.fake.on(
      "GET",
      std::string{commands_url} +
          "?platform=PS5&commandIds=synthetic%2Freceipt&"
          "includeFields=actions,commandDetail",
      {Reply{
           200,
           R"({"commands":[{"commandStatus":"failed","error":{"code":1234,"message":"PRIVATE_MESSAGE"}}]})"},
       Reply{200, R"({"commands":[]})"}, Reply{200, "[]"}});
  const auto session = RestRig::session(RestRig::session_text);
  auto accepted =
      navigation.start_remote_play(session, 1, zero_uid, zero, zero);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(accepted.value());
  EXPECT_EQ(accepted.value()->view(), "synthetic/receipt");
  for (int index = 0; index < 2; ++index) {
    auto missing =
        navigation.start_remote_play(session, 1, zero_uid, zero, zero);
    ASSERT_TRUE(missing) << index;
    EXPECT_FALSE(missing.value()) << index;
  }
  const auto failed = navigation.poll_remote_play_command(
      accepted.value()->view(), std::chrono::milliseconds{2000});
  ASSERT_TRUE(failed);
  ASSERT_TRUE(failed.value().state);
  EXPECT_EQ(failed.value().state->status, "failed");
  EXPECT_EQ(failed.value().state->error_code, 1234);
  EXPECT_EQ(rig.fake.seen.back().total_timeout,
            std::chrono::milliseconds{2000});
  EXPECT_EQ(rig.fake.seen.back().url.find("commandStatus="), std::string::npos);
  const auto absent = navigation.poll_remote_play_command(
      accepted.value()->view(), std::chrono::milliseconds{2000});
  ASSERT_TRUE(absent);
  EXPECT_FALSE(absent.value().state);
  EXPECT_FALSE(navigation.poll_remote_play_command(
      accepted.value()->view(), std::chrono::milliseconds{2000}));
}

}  // namespace
}  // namespace psn_test
