#include "tests/unit/core/cloud/gk/spec_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using JsonTree = SpecRig::JsonTree;
using Invalid = SpecRig::Invalid;

class GkSpecAuthorization : public ::testing::Test {
 protected:
  static auto without_ps3_code(std::string spec) -> std::string {
    const auto pos = spec.find("\"ps3AuthCode\":\"\"");
    EXPECT_NE(pos, std::string::npos);
    if (pos != std::string::npos)
      spec.erase(pos, sizeof("\"ps3AuthCode\":\"\"") - 1);
    return spec;
  }

  static auto refused(
      const libchiaking::Expected<libchiaking::SecretString, Invalid>& patched,
      Invalid reason) -> testing::AssertionResult {
    if (patched) return testing::AssertionFailure() << "the spec was signed";
    if (patched.error() != reason)
      return testing::AssertionFailure()
             << "refused as " << testing::PrintToString(patched.error());
    return testing::AssertionSuccess();
  }
};

class GkCloudStreamAuthSpec : public GkSpecAuthorization {
 protected:
  void SetUp() override {
    const auto encoded = SpecRig::json_of(gk::LaunchSpec::cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+09:00").value(), 1080));
    ASSERT_TRUE(encoded);
    preauth = encoded.value();
  }

  auto patch(std::string_view spec, std::string_view gk_code,
             std::string_view server) const {
    return SpecRig::patch(CloudService::cloud_stream, spec, gk_code, server);
  }

  std::string preauth;
};

TEST_F(GkCloudStreamAuthSpec, LeavesTheSpecAsItIsForEmptyCodes) {
  const auto patched = patch(preauth, "", "");
  ASSERT_TRUE(patched);
  EXPECT_EQ(patched.value().view(), preauth);
}

TEST_F(GkCloudStreamAuthSpec, FillsTheClientAndStreamServerCodes) {
  const auto patched = patch(preauth, "synthetic-gk", "synthetic-server");
  ASSERT_TRUE(patched);
  const auto parsed = SpecRig::object(patched.value().view());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(JsonTree::text_at(*parsed, "gkCloudAuthCode"), "synthetic-gk");
  const auto* ps3 = JsonTree::member(*parsed, "ps3AuthCode");
  ASSERT_NE(ps3, nullptr);
  EXPECT_TRUE(JsonTree::text(*ps3).empty());
  EXPECT_EQ(JsonTree::text_at(*parsed, "streamServerAuthCode"),
            "synthetic-server");
}

TEST_F(GkCloudStreamAuthSpec, EscapesTheCodes) {
  constexpr std::string_view escaped_code = "a\"b\\c\nd\x01";
  const auto patched = patch(preauth, escaped_code, "s\"\\\x02/");
  ASSERT_TRUE(patched);
  EXPECT_NE(patched.value().view().find(
                "\"gkCloudAuthCode\":\"a\\\"b\\\\c\\nd\\u0001\""),
            std::string_view::npos);
  EXPECT_NE(patched.value().view().find(
                "\"streamServerAuthCode\":\"s\\\"\\\\\\u0002/\""),
            std::string_view::npos);
  const auto parsed = JsonTree::parse(patched.value().view());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(JsonTree::text_at(parsed.value(), "gkCloudAuthCode"), escaped_code);
  EXPECT_EQ(JsonTree::text_at(parsed.value(), "streamServerAuthCode"),
            "s\"\\\x02/");
}

TEST_F(GkCloudStreamAuthSpec, RefusesASpecOfAnotherShape) {
  EXPECT_TRUE(refused(patch(without_ps3_code(preauth), "x", "y"),
                      Invalid::unexpected_shape));
  EXPECT_TRUE(
      refused(patch(preauth + preauth, "x", "y"), Invalid::unexpected_shape));
  EXPECT_TRUE(refused(patch("{\"streamServerAuthCode\":\"\",\"ps3AuthCode\":"
                            "\"\",\"gkCloudAuthCode\":\"\"}",
                            "x", "y"),
                      Invalid::unexpected_shape));
}

TEST_F(GkCloudStreamAuthSpec, RefusesASpecOrCodeOverTheBodyLimit) {
  EXPECT_TRUE(
      refused(patch(std::string(gk::Client::body_limit + 1, 'x'), "x", "y"),
              Invalid::oversize));
  EXPECT_TRUE(
      refused(patch(preauth, std::string(gk::Client::body_limit + 1, 'x'), "y"),
              Invalid::oversize));
  EXPECT_TRUE(
      refused(patch(preauth, std::string(gk::Client::body_limit, 'x'), "y"),
              Invalid::oversize));
}

TEST_F(GkCloudStreamAuthSpec, KeepsTheEscapedEntitlementWhenSigning) {
  const auto odd = gk::LaunchSpec::cloud_stream(
      "e\x1f\xFF", gk::Language{"en-US"},
      gk::TimeZone::parse("UTC+09:00").value(), 1080);
  ASSERT_TRUE(odd);
  EXPECT_TRUE(odd.value().json().starts_with(
      "{\"entitlementId\":\"e\\u001F\xEF\xBF\xBD\","));
  const auto patched = odd.value().authorized(GkRig::codes("g\x1f", "s"));
  ASSERT_TRUE(patched);
  EXPECT_TRUE(patched.value().view().starts_with(
      "{\"entitlementId\":\"e\\u001F\xEF\xBF\xBD\","));
  EXPECT_NE(patched.value().view().find("\"gkCloudAuthCode\":\"g\\u001F\""),
            std::string_view::npos);
}

class GkLegacyCloudStreamAuthSpec : public GkSpecAuthorization {
 protected:
  void SetUp() override {
    const auto encoded = SpecRig::json_of(gk::LaunchSpec::legacy_cloud_stream(
        "synthetic-entitlement", gk::Language{"en-US"},
        gk::TimeZone::parse("UTC+09:00").value(), 1080));
    ASSERT_TRUE(encoded);
    preauth = encoded.value();
  }

  auto patch(std::string_view spec, std::string_view gk_code,
             std::string_view server) const {
    return SpecRig::patch(CloudService::legacy_cloud_stream, spec, gk_code,
                          server);
  }

  std::string preauth;
};

TEST_F(GkLegacyCloudStreamAuthSpec, FillsThePs3CodeWithTheServerCode) {
  const auto patched = patch(preauth, "synthetic-gk", "synthetic-server");
  ASSERT_TRUE(patched);
  const auto parsed = SpecRig::object(patched.value().view());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(JsonTree::text_at(*parsed, "gkCloudAuthCode"), "synthetic-gk");
  EXPECT_EQ(JsonTree::text_at(*parsed, "ps3AuthCode"), "synthetic-server");
  EXPECT_EQ(JsonTree::text_at(*parsed, "streamServerAuthCode"),
            "synthetic-server");
  EXPECT_NE(patched.value().view().find("\"ps3AuthCode\":\"synthetic-server\""),
            std::string_view::npos);
}

TEST_F(GkLegacyCloudStreamAuthSpec, EscapesTheServerCodeInBothFields) {
  const auto patched = patch(preauth, "g\"q", "s\"\\\x01");
  ASSERT_TRUE(patched);
  EXPECT_NE(patched.value().view().find("\"ps3AuthCode\":\"s\\\"\\\\\\u0001\""),
            std::string_view::npos);
  const auto parsed = JsonTree::parse(patched.value().view());
  ASSERT_TRUE(parsed);
  EXPECT_EQ(JsonTree::text_at(parsed.value(), "ps3AuthCode"), "s\"\\\x01");
  EXPECT_EQ(JsonTree::text_at(parsed.value(), "streamServerAuthCode"),
            "s\"\\\x01");
}

TEST_F(GkLegacyCloudStreamAuthSpec, LeavesTheSpecAsItIsForEmptyCodes) {
  const auto patched = patch(preauth, "", "");
  ASSERT_TRUE(patched);
  EXPECT_EQ(patched.value().view(), preauth);
}

TEST_F(GkLegacyCloudStreamAuthSpec, RefusesASpecOfAnotherShape) {
  EXPECT_TRUE(refused(patch(without_ps3_code(preauth), "x", "y"),
                      Invalid::unexpected_shape));
  EXPECT_TRUE(
      refused(patch(preauth + preauth, "x", "y"), Invalid::unexpected_shape));
}

TEST_F(GkLegacyCloudStreamAuthSpec, SignsLargeCodesUpToTheBodyLimit) {
  const auto large =
      patch(preauth, std::string(8 * 1024, 'x'), std::string(8 * 1024, 'y'));
  ASSERT_TRUE(large);
  EXPECT_GT(large.value().size(), std::size_t{16 * 1024});
  EXPECT_TRUE(
      refused(patch(preauth, std::string(gk::Client::body_limit / 2, 'x'),
                    std::string(gk::Client::body_limit / 2, 'y')),
              Invalid::oversize));
}

}  // namespace
}  // namespace cloud_test
