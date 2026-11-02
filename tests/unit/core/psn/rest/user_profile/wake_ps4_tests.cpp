#include "src/core/psn/rest/user_profile/user_profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using libchiaking::psn::rest::UserProfile;
using Reply = RestRig::Reply;

class WakePs4 : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 16> zero{};
  static constexpr std::array<std::byte, 16> mixed{
      std::byte{0},  std::byte{1},  std::byte{2},  std::byte{3},
      std::byte{4},  std::byte{5},  std::byte{6},  std::byte{7},
      std::byte{8},  std::byte{9},  std::byte{10}, std::byte{11},
      std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15}};

  auto wakes(std::string_view base, std::string_view online_id) {
    return profile.wake_ps4(
        base, online_id, RestRig::session(RestRig::session_text), zero, zero);
  }

  RestRig rig;
  UserProfile profile{rig.http, RestRig::bearer()};
};

TEST_F(WakePs4, SendsTheRequestExactly) {
  rig.fake.on("POST",
              "https://profile.example.test/base/v1/users/user_17/"
              "remoteConsole/wakeUp?platform=PS4",
              {Reply{200, ""}});
  EXPECT_TRUE(profile.wake_ps4("https://profile.example.test/base", "user_17",
                               RestRig::session(RestRig::session_text), mixed,
                               zero));
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(
      rig.fake.seen[0].body,
      R"json({"data":{"clientType":"Windows","data1":"AAECAwQFBgcICQoLDA0ODw==","data2":"AAAAAAAAAAAAAAAAAAAAAA==","roomId":0,"protocolVer":"10.0","sessionId":"01234567-89ab-cdef-0123-456789abcdef"},"dataTypeSuffix":"remotePlay"})json");
  EXPECT_EQ(RestRig::headers_of(rig.fake.seen[0]),
            (std::vector<std::string>{
                "Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                "Host: profile.example.test", "Connection: Keep-Alive",
                "Content-Type: application/json; charset=utf-8",
                "User-Agent: RpNetHttpUtilImpl"}));
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(WakePs4, EscapesTheOnlineIdAndNamesAnIpv6Host) {
  rig.fake.on("POST",
              "HTTPS://[2001:db8::1]:8443/base/v1/users/a%20b%2Fc%3Fd%23e%25/"
              "remoteConsole/wakeUp?platform=PS4",
              {Reply{200, ""}});
  EXPECT_TRUE(profile.wake_ps4(
      "HTTPS://[2001:db8::1]:8443/base", "a b/c?d#e%",
      RestRig::session("01234567-89AB-CDEF-0123-456789ABCDEF"), zero, zero));
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_NE(
      rig.fake.seen[0].body.find(
          R"json("sessionId":"01234567-89AB-CDEF-0123-456789ABCDEF")json"),
      std::string::npos)
      << rig.fake.seen[0].body;
  const auto headers = RestRig::headers_of(rig.fake.seen[0]);
  ASSERT_GT(headers.size(), 1U);
  EXPECT_EQ(headers[1], "Host: [2001:db8::1]:8443");
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(WakePs4, AcceptsAnOnlineIdOf128Characters) {
  rig.fake.on("POST",
              "https://profile.example.test/v1/users/" + std::string(128, 'x') +
                  "/remoteConsole/wakeUp?platform=PS4",
              {Reply{200, ""}});
  EXPECT_TRUE(wakes("https://profile.example.test", std::string(128, 'x')));
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(WakePs4, RefusesAnUnsafeBaseUrlUnsent) {
  for (const std::string_view bad :
       {"http://profile.example.test", "https://user@profile.example.test",
        "https://profile.example.test/", "https://profile.example.test?x=1",
        "https://profile.example.test#frag",
        "https://profile.example.test\\evil", "https://profile.example.test:0",
        "https://profile.example.test:65536", "https://[::1]:", "https://[::1",
        "https://[fe80::1%25en0]"}) {
    const auto result = wakes(bad, "user");
    ASSERT_FALSE(result) << bad;
    EXPECT_TRUE(result.error().is<bad_request::InvalidUrl>()) << bad;
  }
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(WakePs4, RefusesAnUnsafeOnlineIdUnsent) {
  for (const std::string_view bad :
       {std::string_view{""}, std::string_view{"a\nb"},
        std::string_view{"a\x7f"}}) {
    const auto result = wakes("https://profile.example.test", bad);
    ASSERT_FALSE(result) << bad;
    EXPECT_TRUE(result.error().is<bad_request::InvalidOnlineId>()) << bad;
  }
  const auto long_id =
      wakes("https://profile.example.test", std::string(257, 'x'));
  ASSERT_FALSE(long_id);
  EXPECT_TRUE(long_id.error().is<bad_request::InvalidOnlineId>());
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace psn_test
