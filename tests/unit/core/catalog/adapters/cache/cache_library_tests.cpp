#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/cache/shelf_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;
using libchiaking::catalog::HeldAs;
using libchiaking::catalog::LibraryState;

class CachedLibrary : public ::testing::Test, protected CacheCases {
 protected:
  static auto ps5_games(Time now) -> libchiaking::catalog::OwnedPs5Games {
    using libchiaking::catalog::Ps5Entitlement;
    Ps5Entitlement::Facts held_facts{
        .id = *libchiaking::catalog::OwnershipId::parse(rivet)};
    held_facts.product = product(rivet);
    held_facts.sku =
        libchiaking::catalog::SkuId::parse("EP0109-PPSA07028_00-RIVET-E001");
    held_facts.title = libchiaking::catalog::TitleId::parse("PPSA07028_00");
    held_facts.concept_id = libchiaking::catalog::ConceptId::parse("10001");
    held_facts.name = "Rivet & Bolt: Split Worlds";
    held_facts.active = true;
    held_facts.entitlement_type = Ps5Entitlement::unified_entitlement;
    held_facts.feature_type = Ps5Entitlement::full_game_feature;
    held_facts.package_type = "PSGD";
    held_facts.streaming_supported = true;
    held_facts.preorder_core = true;
    held_facts.playable_from = "2031-01-01T00:00:00Z";
    auto trial_facts = held_facts;
    trial_facts.id =
        *libchiaking::catalog::OwnershipId::parse("EP0109-PPSA07028_00-TRIAL");
    trial_facts.feature_type = Ps5Entitlement::trial_feature;
    trial_facts.sku.reset();
    trial_facts.playable_from.reset();
    return libchiaking::catalog::OwnedPs5Games{
        {Ps5Entitlement{held_facts}, Ps5Entitlement{trial_facts}},
        LibraryState::partial,
        {2, 1},
        now};
  }

  CacheShelf shelf;
  libchiaking::catalog::Library owned_titles{
      library(first_account, shelf.now() - std::chrono::milliseconds{1234})};
};

TEST_F(CachedLibrary, RoundTrips) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{owned_titles}));
  const auto read = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(read);
  ASSERT_TRUE(read.value());
  const auto& back = read.value()->library();
  EXPECT_EQ(back, owned_titles);
  EXPECT_EQ(back.account(), first_account);
  EXPECT_EQ(back.fetched_at(), owned_titles.fetched_at());
  ASSERT_EQ(back.size(), 4U);
  ASSERT_EQ(back.bundles().size(), 1U);
  EXPECT_EQ(back.bundles()[0].siblings.size(), 2U);
  EXPECT_EQ(back.titles()[0].release().time(),
            owned_titles.fetched_at() + std::chrono::hours{48});
  EXPECT_TRUE(back.titles()[1].release().is_unannounced());
  EXPECT_EQ(back.titles()[1].held_as(), HeldAs::trial_access);
  EXPECT_TRUE(back.titles()[2].release().is_unrestricted());
  EXPECT_EQ(back.titles()[2].held_as(), HeldAs::disc_upgrade);
  EXPECT_FALSE(back.titles()[2].product_id());
  EXPECT_TRUE(back.titles()[0].concept_id());
  EXPECT_EQ(back.titles()[3].held_as(), HeldAs::other_game);
  EXPECT_EQ(back.titles()[3].name(), "Owned \xC3\xA9");
}

TEST_F(CachedLibrary, IsKeptByAccountWithoutRegionOrStreaming) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{owned_titles}));
  const auto text = shelf.text("user/1111111111111111111/library");
  EXPECT_TRUE(text.starts_with(
      R"({"record":"library","schema":5,"writtenAtMs":1800000000000,"account":"1111111111111111111","body":{)"))
      << text;
  EXPECT_NE(text.find(R"("ps5Ownership":"fetched")"), std::string::npos);
  EXPECT_EQ(text.find("\"region\""), std::string::npos);
  EXPECT_EQ(text.find("\"streams\""), std::string::npos);
}

TEST_F(CachedLibrary, IsWrittenByteForByte) {
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{owned_titles}));
  EXPECT_EQ(
      shelf.text("user/1111111111111111111/library"),
      R"({"record":"library","schema":5,"writtenAtMs":1800000000000,"account":"11)"
      R"(11111111111111111","body":{"fetchedAtMs":1799999998766,"titles":[{"id":")"
      R"(EP0109-PPSA07028_00-FAKE0110TITLE000","name":"Owned é","platform":"ps5",)"
      R"("service":"ps5_cloud","heldAs":"full_game","releaseAtMs":1800172798766,")"
      R"(product":"EP0109-PPSA07028_00-FAKE0110TITLE000","concept":"90000016","co)"
      R"(ver":"https://image.api.playstation.com/rift/cover.png"},{"id":"UP0114-P)"
      R"(PSA37004_00-FAKE0071TITLE000","name":"Owned é","platform":"ps5","service)"
      R"(":"ps5_cloud","heldAs":"trial_access","releaseUnannounced":true,"cover":)"
      R"(""},{"id":"UP0114-CUSA07009_00-FAKE0052TITLE000","name":"Owned é","platf)"
      R"(orm":"ps4","service":"old_service","heldAs":"disc_upgrade","cover":""},{)"
      R"("id":"EP0106-PPSA07037_00-FAKE0016TITLE000","name":"Owned é","platform":)"
      R"("ps5","service":"ps5_cloud","heldAs":"other_game","cover":""}],"bundles")"
      R"(:[{"product":"EP0109-PPSA07028_00-FAKE0109TITLE000","siblings":["EP0109-)"
      R"(PPSA07028_00-FAKE0110TITLE000","UP0114-PPSA37004_00-FAKE0071TITLE000"]}])"
      R"(,"ps5Ownership":"fetched"}})");
}

TEST_F(CachedLibrary, RoundTripsThePs5Entitlements) {
  const auto ps5 = ps5_games(shelf.now());
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{owned_titles, ps5}));
  const auto again = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(again);
  ASSERT_TRUE(again.value());
  EXPECT_EQ(again.value()->library(), owned_titles);
  EXPECT_EQ(again.value()->ps5_games(), ps5);
  EXPECT_NE(shelf.text("user/1111111111111111111/library")
                .find(R"("sku":"EP0109-PPSA07028_00-RIVET-E001")"),
            std::string::npos);
}

TEST_F(CachedLibrary, RoundTripsAnUnsurePs5Part) {
  for (const auto state : {LibraryState::partial, LibraryState::unavailable}) {
    const auto unsure = owned_titles.with_ps5_part({}, state);
    const auto kept_titles = state == LibraryState::unavailable
                                 ? owned_titles.size()
                                 : std::size_t{1};
    EXPECT_EQ(unsure.ps5_ownership(), state);
    EXPECT_EQ(unsure.size(), kept_titles);
    ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{unsure}));
    const auto held = shelf.cache.read<cache::LibraryRecord>(first_account);
    ASSERT_TRUE(held);
    ASSERT_TRUE(held.value());
    EXPECT_EQ(held.value()->library(), unsure);
  }
}

TEST_F(CachedLibrary, RoundTripsAPs5TitleNamedAsADemo) {
  auto demo = owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud);
  demo.name = "Rivet Demo";
  demo.product = product(rivet);
  const auto named = owned_titles.with_ps5_part(
      {OwnedTitle::from(std::move(demo)).value()}, LibraryState::fetched);
  EXPECT_EQ(named.size(), 2U);
  EXPECT_TRUE(named.find(*libchiaking::catalog::OwnershipId::parse(rivet)));
  ASSERT_TRUE(shelf.cache.write(cache::LibraryRecord{named}));
  const auto demo_read = shelf.cache.read<cache::LibraryRecord>(first_account);
  ASSERT_TRUE(demo_read);
  ASSERT_TRUE(demo_read.value());
  EXPECT_EQ(demo_read.value()->library(), named);
}

}  // namespace
}  // namespace catalog_test
