#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_walk_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::ListOrigin;
using libchiaking::catalog::PageCount;
using libchiaking::catalog::Unanswered;
using libchiaking::catalog::ps::PublicStoreWalk;
using Strings = std::vector<std::string>;

class PublicStoreWalkFallback : public ::testing::Test, public StoreWalkRig {};

TEST_F(PublicStoreWalkFallback, ReadsTheAccountCountryStore) {
  http.on(
      "GET", "container/DE/de/19/STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"EP-CUSA00003_00-Y","name":"Y","container_type":"product"}]})"}});
  const auto walked_publicly = fallback("de-DE");
  ASSERT_TRUE(walked_publicly);
  EXPECT_TRUE(walked_publicly.value().source().complete);
  EXPECT_EQ(walked_publicly.value().source().origin,
            ListOrigin::regional_store);
  EXPECT_EQ(walked_publicly.value().source().settled_locale, locale("de-DE"));
  EXPECT_EQ(walked_publicly.value().entries().size(), 1U);
  EXPECT_EQ(http.count("container/GB/"), 0);
}

TEST_F(PublicStoreWalkFallback, WalksTheSharedStoreForACountryWithoutOne) {
  http.on("GET", "container/JP/ja/19/STORE-MSF192014-APOLLOROOT",
          {{404, "{}"}});
  http.on(
      "GET", "container/GB/en/19/STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"EP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  const auto walked_publicly = fallback("ja-JP");
  ASSERT_TRUE(walked_publicly);
  EXPECT_TRUE(walked_publicly.value().source().complete);
  EXPECT_EQ(walked_publicly.value().source().origin, ListOrigin::public_store);
  EXPECT_EQ(walked_publicly.value().source().region, locale("ja-JP"));
  EXPECT_EQ(walked_publicly.value().entries().size(), 1U);
  EXPECT_EQ(http.count("container/JP/ja/"), 1);
}

TEST_F(PublicStoreWalkFallback, IsUnavailableWhenNoStoreAnswers) {
  http.on("GET", "STORE-MSF192014-APOLLOROOT", {{404, "{}"}});
  const auto missing = fallback("ja-JP");
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason(), Unanswered::Reason::unavailable);
}

TEST_F(PublicStoreWalkFallback, SaysWhenItIsIncomplete) {
  http.on(
      "GET", "STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"CHILD","container_type":"container"}]})"}});
  http.on("GET", "/CHILD?", {{500, ""}});
  const auto result = fallback("en-GB");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().source().complete);
  EXPECT_TRUE(result.value().entries().empty());
}

TEST_F(PublicStoreWalkFallback, BoundsItsRequestsByThePageBudget) {
  http.on(
      "GET", "STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":3,"links":[{"id":"BIG1","container_type":"container"},{"id":"BIG2","container_type":"container"},{"id":"BIG3","container_type":"container"}]})"}});
  http.on(
      "GET", "/BIG",
      {{200,
        R"({"total_results":999999,"links":[{"id":"P","container_type":"product"}]})"}});
  const auto result = fallback("en-GB");
  ASSERT_TRUE(result);
  EXPECT_EQ(http.count("/container/"), PublicStoreWalk::page_budget);
  EXPECT_EQ(http.count("/BIG1?"), 100);
  EXPECT_EQ(http.count("/BIG2?"), 98);
  EXPECT_EQ(http.count("/BIG3?"), 1);
  EXPECT_FALSE(result.value().source().complete);
}

TEST_F(PublicStoreWalkFallback, VisitsAtMostFortyChildren) {
  std::string links;
  for (int index = 0; index < 45; ++index)
    links += std::string{index ? "," : ""} + "{\"id\":\"KID" +
             std::to_string(index) + "\",\"container_type\":\"container\"}";
  http.on("GET", "STORE-MSF192014-APOLLOROOT",
          {{200, "{\"total_results\":45,\"links\":[" + links + "]}"}});
  http.on("GET", "/KID", {{200, R"({"total_results":0,"links":[]})"}});
  const auto result = fallback("en-GB");
  ASSERT_TRUE(result);
  EXPECT_EQ(http.count("/KID"), PublicStoreWalk::child_limit);
  EXPECT_FALSE(result.value().source().complete);
}

TEST_F(PublicStoreWalkFallback, PagesByAHundred) {
  http.on(
      "GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=0&size=100",
      {{200,
        R"({"total_results":150,"links":[{"id":"A1","container_type":"product"}]})"}});
  http.on(
      "GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=100&size=100",
      {{200,
        R"({"total_results":150,"links":[{"id":"A2","container_type":"product"},{"id":"A1","container_type":"product"}]})"}});
  const auto result = fallback("en-GB");
  ASSERT_TRUE(result);
  EXPECT_EQ(http.count("APOLLOROOT"), 2);
  EXPECT_TRUE(result.value().source().complete);
  EXPECT_EQ(result.value().entries().size(), 2U);
  ASSERT_FALSE(http.seen.empty());
  EXPECT_EQ(
      http.seen.front().url,
      "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/GB/en/"
      "19/STORE-MSF192014-APOLLOROOT"
      "?useOffers=true&gkb=1&gkb2=1&start=0&size=100");
}

TEST_F(PublicStoreWalkFallback, ExpectsNoPagesFromANegativeTotal) {
  const auto page = [](std::string total) {
    return "{\"total_results\":" + total +
           R"(,"links":[{"id":"A1","container_type":"product"}]})";
  };
  http.on("GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=0&size=100",
          {{200, page("-100")}, {200, page("-1000")}});
  for (const std::string total : {"-100", "-1000"}) {
    SCOPED_TRACE(total);
    std::optional<std::uint32_t> expected;
    bool told = false;
    const auto result = PublicStoreWalk{client, wall}.walk(
        locale("en-GB"),
        PageCount{[&expected, &told](std::uint32_t,
                                     std::optional<std::uint32_t> pages) {
          told = true;
          expected = pages;
        }});
    ASSERT_TRUE(result);
    EXPECT_TRUE(told);
    EXPECT_FALSE(expected);
  }
}

TEST_F(PublicStoreWalkFallback, StartsKnownPagesAndChildrenTogether) {
  http.on(
      "GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=0&size=100",
      {{200,
        R"({"total_results":250,"links":[{"id":"K1","container_type":"container"},{"id":"K2","container_type":"container"}]})"}});
  http.on(
      "GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=100&size=100",
      {{200,
        R"({"total_results":250,"links":[{"id":"R1","container_type":"product"}]})"}});
  http.on(
      "GET", "APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=200&size=100",
      {{200,
        R"({"total_results":250,"links":[{"id":"R2","container_type":"product"}]})"}});
  http.on(
      "GET", "/K1?useOffers=true&gkb=1&gkb2=1&start=0&size=100",
      {{200,
        R"({"total_results":150,"links":[{"id":"A","container_type":"product"}]})"}});
  http.on(
      "GET", "/K1?useOffers=true&gkb=1&gkb2=1&start=100&size=100",
      {{200,
        R"({"total_results":150,"links":[{"id":"B","container_type":"product"}]})"}});
  http.on(
      "GET", "/K2?useOffers=true&gkb=1&gkb2=1&start=0&size=100",
      {{200,
        R"({"total_results":1,"links":[{"id":"C","container_type":"product"}]})"}});
  const auto result = fallback("en-GB");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().source().complete);
  EXPECT_EQ(result.value().entries().size(), 5U);
  EXPECT_EQ(
      http.events_naming("APOLLOROOT?"),
      (Strings{"start APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=0",
               "perform APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=0",
               "start APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=100",
               "start APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=200",
               "perform APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=100",
               "perform APOLLOROOT?useOffers=true&gkb=1&gkb2=1&start=200"}));
  EXPECT_EQ(http.events_naming("/K"),
            (Strings{"start /K1?useOffers=true&gkb=1&gkb2=1&start=0",
                     "start /K2?useOffers=true&gkb=1&gkb2=1&start=0",
                     "perform /K1?useOffers=true&gkb=1&gkb2=1&start=0",
                     "start /K1?useOffers=true&gkb=1&gkb2=1&start=100",
                     "perform /K1?useOffers=true&gkb=1&gkb2=1&start=100",
                     "perform /K2?useOffers=true&gkb=1&gkb2=1&start=0"}));
}

}  // namespace
}  // namespace catalog_test
