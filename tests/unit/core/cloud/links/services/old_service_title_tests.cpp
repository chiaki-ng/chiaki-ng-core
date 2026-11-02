#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "src/core/utils/json/json.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace cloud_test {
namespace {

using libchiaking::play::OldServiceTitle;

class OldServiceTitleSelection : public ::testing::Test {
 protected:
  static constexpr std::string_view product = "EP0000-CUSA12345_00-TITLE";

  static auto selects(std::string_view json, std::string_view expected)
      -> testing::AssertionResult {
    const auto parsed = libchiaking::json::read<OldServiceTitle::Reply>(json);
    if (!parsed) return testing::AssertionFailure() << "the reply is not JSON";
    const auto selected = OldServiceTitle::decode(parsed.value(), product);
    if (!selected) return testing::AssertionFailure() << "nothing was selected";
    if (selected->entitlement() != expected)
      return testing::AssertionFailure()
             << "selected " << selected->entitlement();
    return testing::AssertionSuccess();
  }
};

TEST_F(OldServiceTitleSelection, PrefersAStreamingLicence) {
  EXPECT_TRUE(selects(
      R"({"default_sku":{"id":"A","entitlements":[{"id":"WRONG","packageType":"PS4GS"},{"id":"CUSA12345-MATCH","packageType":"PS4GS"},{"id":"LICENSE4","license_type":4}]}})",
      "LICENSE4"));
}

TEST_F(OldServiceTitleSelection, PrefersTheStreamingPackageOfTheProduct) {
  EXPECT_TRUE(selects(
      R"({"default_sku":{"id":"A","entitlements":[{"id":"WRONG","packageType":"PS4GS"},{"id":"CUSA12345-MATCH","packageType":"PS4GS"}]}})",
      "CUSA12345-MATCH"));
}

TEST_F(OldServiceTitleSelection, LooksInTheOtherSkusForAStreamingPackage) {
  EXPECT_TRUE(selects(
      R"({"default_sku":{"id":"A","entitlements":[{"id":"CUSA12345-GD","packageType":"PS4GD"}]},"skus":[{"id":"B","entitlements":[{"id":"OTHER-GS","packageType":"PS4GS"}]}]})",
      "OTHER-GS"));
}

TEST_F(OldServiceTitleSelection, FallsBackToTheDownloadPackageOfTheProduct) {
  EXPECT_TRUE(selects(
      R"({"default_sku":{"id":"A","entitlements":[{"id":"OTHER-GD","packageType":"PS4GD"},{"id":"CUSA12345-GD","packageType":"PS4GD"}]}})",
      "CUSA12345-GD"));
}

TEST(OldServiceTitleRegions, TellsALegacyTitleByItsProductCode) {
  EXPECT_FALSE(OldServiceTitle::is_legacy("EP0-CUSA12345_00-CLASSIC"));
  EXPECT_TRUE(OldServiceTitle::is_legacy("EP0-NPEB12345_00-CUSA"));
}

}  // namespace
}  // namespace cloud_test
