#include "src/core/psn/rest/user_profile/user_profile.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

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

class ProfileBaseUrl : public ::testing::Test {
 protected:
  static constexpr std::string_view profile_url =
      "https://asm.np.community.playstation.net/asm/v1/apps/me/baseUrls/"
      "userProfile";

  static auto profile_from(std::string_view body) -> libchiaking::Expected<
      std::string, libchiaking::psn::rest::UserProfile::ProfileBaseUrlError> {
    RestRig rig;
    rig.fake.on("GET", std::string{profile_url},
                {Reply{200, std::string{body}}});
    UserProfile profile{rig.http, RestRig::bearer()};
    auto result = profile.profile_base_url();
    EXPECT_EQ(rig.fake.seen.size(), 1U) << body;
    if (!rig.fake.seen.empty()) {
      EXPECT_EQ(RestRig::headers_of(rig.fake.seen[0]),
                (std::vector<std::string>{
                    "Authorization: Bearer SYNTHETIC_TEST_TOKEN",
                    "Host: asm.np.community.playstation.net",
                    "Connection: Keep-Alive",
                    "Content-Type: application/json; charset=utf-8",
                    "User-Agent: RpNetHttpUtilImpl"}))
          << body;
      EXPECT_TRUE(rig.fake.seen[0].body.empty()) << body;
    }
    return result;
  }

  template <class Reason>
  static auto refused_as(std::string_view body) -> testing::AssertionResult {
    const auto decoded = profile_from(body);
    if (decoded)
      return testing::AssertionFailure() << body << " gave " << decoded.value();
    if (!decoded.error().template is<Reason>())
      return testing::AssertionFailure()
             << body << " was refused as " << decoded.error().reason_name();
    return testing::AssertionSuccess();
  }
};

TEST_F(ProfileBaseUrl, KeepsAnEscapedPath) {
  const auto direct =
      profile_from(R"({"url":"https://profile.example.test/base%2Fpath"})");
  ASSERT_TRUE(direct);
  EXPECT_EQ(direct.value(), "https://profile.example.test/base%2Fpath");
}

TEST_F(ProfileBaseUrl, UnescapesSlashesAndKeepsThePort) {
  const auto escaped = profile_from(
      R"({"other":[1,true],"url":"https:\/\/profile.example.test:8443\/base"})");
  ASSERT_TRUE(escaped);
  EXPECT_EQ(escaped.value(), "https://profile.example.test:8443/base");
}

TEST_F(ProfileBaseUrl, KeepsAnIpv6Host) {
  const auto ipv6 =
      profile_from(R"({"url":"HTTPS://[2001:db8::1]:8443/base"})");
  ASSERT_TRUE(ipv6);
  EXPECT_EQ(ipv6.value(), "HTTPS://[2001:db8::1]:8443/base");
}

TEST_F(ProfileBaseUrl, RejectsUnsafeUrls) {
  for (const std::string_view bad :
       {R"({"url":"http://profile.example.test"})",
        R"({"url":"https://user@profile.example.test"})",
        R"({"url":"https://profile.example.test/base?q=1"})",
        R"({"url":"https://profile.example.test/base#fragment"})",
        R"({"url":"https://profile.example.test:0"})",
        R"({"url":"https://profile.example.test:65536"})",
        R"({"url":"https://[::1]:/base"})",
        R"({"url":"https://profile.example.test:abc"})",
        R"({"url":"https://..."})",
        R"({"url":"https://profile.example.test\\evil"})"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidUrl>(bad));
}

TEST_F(ProfileBaseUrl, RejectsANonTextUrl) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(R"({"url":123})"));
}

TEST_F(ProfileBaseUrl, KeepsTheLastDuplicateUrl) {
  EXPECT_EQ(profile_from(R"({"url":"https://a.test","url":"https://b.test"})")
                .value(),
            "https://b.test");
}

TEST_F(ProfileBaseUrl, RejectsInvalidJson) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidJson>("{"));
}

}  // namespace
}  // namespace psn_test
