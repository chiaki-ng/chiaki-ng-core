#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/usecases/clearance_rig.hpp"
#include "tests/unit/core/catalog/usecases/cloud_stream/launch_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_test {
namespace {

class CloudStreamClearanceQuestions : public ::testing::Test,
                                      protected CloudStreamCases {
 protected:
  static constexpr std::string_view by_concept =
      "operationName=metGetConceptById";
  static constexpr std::string_view unlisted =
      R"({"data":{"productRetrieve":null},"errors":[{"message":"Store Front: There is no required storefront for this product.","path":["productRetrieve"],"extensions":{"type":"NimbusNormalizedError","statusCode":400,"errorCode":3162368}}]})";
};

TEST_F(CloudStreamClearanceQuestions, AsksOwnershipOfTheExactEntitlement) {
  CloudStreamLaunch launch;
  launch.rig.ps5_record_answers(200, show_record());
  const auto seen = launch.seen(owning_nothing());
  const auto owned =
      launch.clearance.ownership(*launch.user, seen, product(show), {});
  ASSERT_TRUE(owned);
  EXPECT_TRUE(owned.value().is_owned());
  EXPECT_EQ(owned.value().game()->ownership()->id(), show);
  EXPECT_EQ(launch.rig.http.count(ClearanceRig::ps5_record), 1);
  EXPECT_EQ(launch.rig.http.count(ClearanceRig::owned_lookup), 0);
}

TEST_F(CloudStreamClearanceQuestions, AsksTheStorefrontOnceAndKeepsIt) {
  CloudStreamLaunch launch;
  launch.rig.storefront_says(
      show,
      R"(,"compatibilityNoticesByPlatform":{"PS5":[{"type":"STREAMING_SUPPORTED","targetPlatforms":["PS5"],"value":"Streaming supported"}]})");
  const auto answer = launch.clearance.storefront(*launch.user, product(show),
                                                  std::nullopt, {});
  ASSERT_TRUE(answer);
  EXPECT_TRUE(answer.value().says_it_streams());
  const auto again = launch.clearance.storefront(*launch.user, product(show),
                                                 std::nullopt, {});
  EXPECT_TRUE(again);
  EXPECT_EQ(launch.rig.http.count(ClearanceRig::store_app), 1);
  EXPECT_EQ(launch.rig.http.count(ClearanceRig::library_token), 0);
}

TEST_F(CloudStreamClearanceQuestions,
       LeavesAProductWithoutAStorefrontOrConceptUnanswered) {
  CloudStreamLaunch without_concept;
  without_concept.rig.http.on("GET", std::string{ClearanceRig::store_app},
                              {{200, std::string{unlisted}}});
  const auto unanswered = without_concept.clearance.storefront(
      *without_concept.user, product(show), std::nullopt, {});
  EXPECT_FALSE(unanswered);
  EXPECT_EQ(without_concept.rig.http.count(std::string{by_concept}), 0);
}

TEST_F(CloudStreamClearanceQuestions,
       AsksAboutAProductWithoutAStorefrontByItsConcept) {
  CloudStreamLaunch with_concept;
  with_concept.rig.http.on("GET", std::string{ClearanceRig::store_app},
                           {{200, std::string{unlisted}}});
  with_concept.rig.http.on("GET", std::string{by_concept},
                           {{200, R"({"data":{"conceptRetrieve":null}})"}});
  const auto concept_id = libchiaking::catalog::ConceptId::parse("10001234");
  ASSERT_TRUE(concept_id);
  const auto asked = with_concept.clearance.storefront(
      *with_concept.user, product(show), concept_id, {});
  EXPECT_FALSE(asked);
  EXPECT_EQ(with_concept.rig.http.count(ClearanceRig::store_app), 1);
  EXPECT_EQ(with_concept.rig.http.count(std::string{by_concept}), 1);
}

}  // namespace
}  // namespace catalog_test
