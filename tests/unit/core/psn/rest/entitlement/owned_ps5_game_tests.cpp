#include "tests/unit/core/psn/rest/entitlement/library_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

class OwnedPs5Game : public LibraryFixture {
 protected:
  static constexpr std::string_view escaped =
      "EP0109-PPSA07015_00-FAKE0044TITLE000";

  void answers(Reply reply) {
    rig.fake.on("GET", game_url(escaped), {std::move(reply)});
  }
};

TEST_F(OwnedPs5Game, IsReadWhenOwned) {
  answers(Reply{
      200,
      R"({"id":"EP0109-PPSA07015_00-FAKE0044TITLE000","productId":"EP0109-PPSA07015_00-FAKE0044TITLE000",)"
      R"("activeFlag":true,"featureType":3,"entitlementType":5,"streamingSupported":true,)"
      R"("gameMeta":{"packageType":"PSGD","name":"Full Game Streams"},"skuMeta":{"playabilityDate":"2031-01-01T00:00:00Z"},)"
      R"("preorderCoreFlag":true})"});
  const auto found = library.owned_ps5_game(escaped);
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value());
  EXPECT_EQ(found.value()->streaming_supported, true);
  EXPECT_EQ(found.value()->preorder_core, true);
  EXPECT_EQ(found.value()->playability_date, "2031-01-01T00:00:00Z");
  EXPECT_TRUE(rig.fake.unmatched.empty());
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(OwnedPs5Game, IsAbsentWhenTheEscapedIdIsNotFound) {
  rig.fake.on("GET", game_url("A%20B%2FC"), {Reply{404, "{}"}});
  const auto absent = library.owned_ps5_game("A B/C");
  ASSERT_TRUE(absent);
  EXPECT_FALSE(absent.value());
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(OwnedPs5Game, IsAbsentWhenNotEntitled) {
  for (const std::string_view body :
       {R"({"error":{"code":3161955,"message":"not entitled"}})",
        R"({"error":{"code":"3161955"}})"}) {
    RestRig denied;
    denied.fake.on("GET", game_url(escaped), {Reply{400, std::string{body}}});
    LibraryEntitlements asking{denied.http, app_bearer()};
    const auto not_owned = asking.owned_ps5_game(escaped);
    ASSERT_TRUE(not_owned) << body;
    EXPECT_FALSE(not_owned.value()) << body;
  }
}

TEST_F(OwnedPs5Game, ReportsAnotherRejection) {
  answers(Reply{400, R"({"error":{"code":3161956}})"});
  const auto rejected = library.owned_ps5_game(escaped);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().http_status(), 400);
}

TEST_F(OwnedPs5Game, ReportsAServerError) {
  answers(Reply{500, ""});
  const auto failed = library.owned_ps5_game(escaped);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().http_status(), 500);
}

TEST_F(OwnedPs5Game, ReportsAnUnreadableRecord) {
  answers(Reply{200, R"({"productId":"x"})"});
  const auto unread = library.owned_ps5_game(escaped);
  ASSERT_FALSE(unread);
  EXPECT_EQ(unread.error().cause(), libchiaking::psn::rest::Cause::response);
}

}  // namespace
}  // namespace psn_test
