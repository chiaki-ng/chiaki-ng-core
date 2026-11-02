#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/user_profile/account_profile.hpp"
#include "src/core/psn/types.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::PlusStatus;
using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::ProfileDetails;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using Reply = ClientRig::Reply;

class AccountProfile : public ::testing::Test {
 protected:
  static constexpr std::string_view profile_url =
      "https://m.np.playstation.com/api/userProfile/v1/internal/users/"
      "578437695752307201/profiles";
  static constexpr std::array<std::byte, 8> account_bytes{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
      std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};

  static auto profile_json(std::string_view is_plus,
                           std::string_view avatars = {}) -> std::string {
    std::string json{"{\"onlineId\":\"Synthetic\""};
    if (!is_plus.empty()) json.append(",\"isPlus\":").append(is_plus);
    if (!avatars.empty()) json.append(",\"avatars\":").append(avatars);
    json.push_back('}');
    return json;
  }

  auto fetch(Reply reply, std::string_view token = "MOBILE_ACCESS_TOKEN")
      -> libchiaking::Expected<
          ProfileDetails,
          libchiaking::psn::rest::AccountProfile::DetailsError> {
    rig.http.rules.clear();
    rig.http.seen.clear();
    rig.http.on("GET", std::string{profile_url}, {std::move(reply)});
    libchiaking::psn::rest::AccountProfile profile{
        rig.client, libchiaking::psn::auth::AppToken{token}};
    return profile.details(libchiaking::AccountId::from_bytes(account_bytes));
  }

  auto details(std::string_view body) -> libchiaking::Expected<
      ProfileDetails, libchiaking::psn::rest::AccountProfile::DetailsError> {
    return fetch(Reply{200, std::string{body}});
  }

  ClientRig rig;
};

TEST_F(AccountProfile, RequestsTheProfileAndReadsAnActiveMembership) {
  const auto fetched = fetch(Reply{200, profile_json("true")});
  ASSERT_TRUE(fetched);
  EXPECT_EQ(fetched.value().online_id, "Synthetic");
  EXPECT_FALSE(fetched.value().avatar_url);
  EXPECT_EQ(fetched.value().plus_status, PlusStatus::active);
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& seen = rig.http.seen.front();
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url, profile_url);
  ASSERT_EQ(seen.headers.size(), 2U);
  EXPECT_EQ(seen.headers[0].name, "Authorization");
  EXPECT_EQ(seen.headers[0].value, "Bearer MOBILE_ACCESS_TOKEN");
  EXPECT_EQ(seen.headers[1].name, "Accept");
  EXPECT_EQ(seen.headers[1].value, "*/*");
  EXPECT_EQ(seen.total_timeout, std::chrono::seconds{30});
  EXPECT_EQ(seen.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(seen.maximum_response_bytes, 16'000'000U);
  EXPECT_TRUE(seen.body.empty());
}

TEST_F(AccountProfile, ReadsAnAbsentOrUnstatedMembership) {
  const auto absent = details(profile_json("false"));
  ASSERT_TRUE(absent);
  EXPECT_EQ(absent.value().plus_status, PlusStatus::absent);
  const auto unknown = details(profile_json(""));
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown.value().plus_status, PlusStatus::unknown);
  const auto null_value = details(profile_json("null"));
  ASSERT_TRUE(null_value);
  EXPECT_EQ(null_value.value().plus_status, PlusStatus::unknown);
}

TEST_F(AccountProfile, HasNoAvatarWhenNoneIsListed) {
  const auto null_avatars = details(profile_json("true", "null"));
  ASSERT_TRUE(null_avatars);
  EXPECT_FALSE(null_avatars.value().avatar_url);
  const auto no_avatars = details(profile_json("true", "[]"));
  ASSERT_TRUE(no_avatars);
  EXPECT_FALSE(no_avatars.value().avatar_url);
}

TEST_F(AccountProfile, PrefersTheMediumThenTheLargeThenTheFirstAvatar) {
  const auto preferred = details(profile_json(
      "true",
      R"([{"size":"l","url":"https://avatar.example/l"},{"size":"s","url":"https://avatar.example/s"},)"
      R"({"size":"m","url":"http://avatar.example:80/m?size=1"}])"));
  ASSERT_TRUE(preferred);
  EXPECT_EQ(preferred.value().avatar_url, "https://avatar.example/m?size=1");
  const auto large = details(profile_json(
      "true",
      R"([{"size":"s","url":"https://avatar.example/s"},{"size":"l","url":"https://avatar.example/l"}])"));
  ASSERT_TRUE(large);
  EXPECT_EQ(large.value().avatar_url, "https://avatar.example/l");
  const auto first = details(profile_json(
      "true",
      R"([{"size":"xl","url":"https://avatar.example/xl"},{"size":"xs","url":"https://avatar.example/xs"}])"));
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().avatar_url, "https://avatar.example/xl");
}

TEST_F(AccountProfile, UpgradesAnHttpAvatarOnTheDefaultPortToHttps) {
  const auto default_port = details(profile_json(
      "false", R"([{"size":"m","url":"http://avatar.example/m"}])"));
  ASSERT_TRUE(default_port);
  EXPECT_EQ(default_port.value().avatar_url, "https://avatar.example/m");
  const auto https = details(profile_json(
      "true", R"([{"size":"m","url":"https://avatar.example:8443/m"}])"));
  ASSERT_TRUE(https);
  EXPECT_EQ(https.value().avatar_url, "https://avatar.example:8443/m");
  const auto literal = details(profile_json(
      "true", R"([{"size":"m","url":"HTTP://[2001:db8::1]:80/m#x"}])"));
  ASSERT_TRUE(literal);
  EXPECT_EQ(literal.value().avatar_url, "https://[2001:db8::1]/m#x");
}

TEST_F(AccountProfile, RefusesAnInvalidAvatarUrl) {
  for (const std::string_view url :
       {"http://avatar.example:81/m", "https://user:pass@avatar.example/m",
        "https:///missing-host", "ftp://avatar.example/m", "/relative/path",
        "https://avatar.example:/m", "https://avatar.example:65536/m",
        "https://a:1:2/m", "https://[::1/m", "https://[]/m",
        "https://[zz::1]/m", "https://[::1]x/m", "https://avatar%2e.example/m",
        "https://avatar.example/a b", "://avatar.example/m"}) {
    SCOPED_TRACE(url);
    std::string body{
        "{\"onlineId\":\"Synthetic\",\"avatars\":[{\"size\":"
        "\"m\",\"url\":\""};
    body.append(url).append("\"}]}");
    const auto result = details(body);
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().is<bad_reply::InvalidUrl>());
  }
}

TEST_F(AccountProfile, RefusesAProfileOfTheWrongShape) {
  for (const std::string_view body :
       {"[]", "{}", "{\"onlineId\":\"\"}", "{\"onlineId\":7}",
        "{\"onlineId\":\"Synthetic\",\"isPlus\":\"true\"}",
        "{\"onlineId\":\"Synthetic\",\"isPlus\":1}",
        "{\"onlineId\":\"Synthetic\",\"avatars\":{}}",
        "{\"onlineId\":\"Synthetic\",\"avatars\":[{}]}",
        "{\"onlineId\":\"Synthetic\",\"avatars\":[7]}",
        "{\"onlineId\":\"Synthetic\",\"avatars\":[{\"size\":\"m\"}]}",
        "{\"onlineId\":\"Synthetic\",\"avatars\":[{\"size\":1,\"url\":\"https:"
        "//avatar.example/m\"}]}"}) {
    SCOPED_TRACE(body);
    const auto result = details(body);
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().is<bad_reply::InvalidSchema>());
  }
}

TEST_F(AccountProfile, RefusesInvalidJson) {
  const auto invalid = details("{");
  ASSERT_FALSE(invalid);
  EXPECT_TRUE(invalid.error().is<bad_reply::InvalidJson>());
}

TEST_F(AccountProfile, ReportsAnErrorStatus) {
  for (const int status : {401, 403, 429, 503}) {
    SCOPED_TRACE(status);
    const auto fetched = fetch(Reply{status, ""});
    ASSERT_FALSE(fetched);
    EXPECT_EQ(fetched.error().http_status(), status);
    EXPECT_EQ(fetched.error().auth_refused(), status == 401 || status == 403);
  }
}

TEST_F(AccountProfile, ReportsATransportFailure) {
  const auto transport = fetch(Reply::transport());
  ASSERT_FALSE(transport);
  EXPECT_TRUE(transport.error().failure());
  EXPECT_FALSE(transport.error().is_cancelled());
}

TEST_F(AccountProfile, SendsNothingOnceStopped) {
  rig.stop();
  const auto cancelled = fetch(Reply{200, profile_json("true")});
  ASSERT_FALSE(cancelled);
  EXPECT_TRUE(cancelled.error().is_cancelled());
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(AccountProfile, RefusesAnUnsafeTokenBeforeSending) {
  for (const std::string_view token :
       {"bad\"token", "bad token", "bad\ntoken", "bad;token", ""}) {
    SCOPED_TRACE(token);
    const auto invalid = fetch(Reply{200, profile_json("true")}, token);
    ASSERT_FALSE(invalid);
    ASSERT_TRUE(invalid.error().failure());
    EXPECT_EQ(invalid.error().failure()->kind(),
              libchiaking::net::http::Failure::Kind::invalid_request);
    EXPECT_TRUE(rig.http.seen.empty());
  }
}

}  // namespace
}  // namespace psn_test
