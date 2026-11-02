#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/refusal_memory.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/details_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace catalog_test {
namespace {

namespace cache = libchiaking::catalog::cache;
using libchiaking::catalog::CacheEntry;
using libchiaking::catalog::CacheId;
using libchiaking::catalog::CacheSummary;
using libchiaking::catalog::every_cache;
using libchiaking::catalog::StoreLocale;
using Reason = libchiaking::catalog::CachesReason;
using Reply = DetailsRig::Reply;

class EveryCatalogCache : public ::testing::Test {
 protected:
  static constexpr libchiaking::AccountId owner{1234567890123456789ULL};

  void SetUp() override {
    rig.script_whole_store();
    rig.http.on("GET", std::string{DetailsRig::logo_file},
                {Reply{200, "logo..."}});
    rig.http.on("GET", "pegi/16.png", {Reply{200, "icon"}});
    user = rig.user();
  }

  static auto in_order(std::span<const CacheSummary> all)
      -> testing::AssertionResult {
    if (all.size() != every_cache.size())
      return testing::AssertionFailure()
             << all.size() << " caches listed, not " << every_cache.size();
    for (std::size_t at = 0; at < all.size(); ++at)
      if (all[at].id != every_cache[at])
        return testing::AssertionFailure()
               << "cache " << at << " is out of order";
    return testing::AssertionSuccess();
  }

  auto summary_of(CacheId id) -> CacheSummary {
    const auto all = rig.catalog.caches();
    EXPECT_TRUE(all);
    EXPECT_TRUE(in_order(all.value()));
    const auto found = std::ranges::find(all.value(), id, &CacheSummary::id);
    if (found == all.value().end()) throw std::out_of_range{"no summary"};
    return *found;
  }

  auto all_empty() -> testing::AssertionResult {
    for (const auto id : every_cache) {
      const auto summary = summary_of(id);
      if (summary.entries != 0 || summary.bytes != 0)
        return testing::AssertionFailure()
               << "cache " << static_cast<int>(id) << " holds "
               << summary.entries << " entries in " << summary.bytes
               << " bytes";
    }
    return testing::AssertionSuccess();
  }

  void seed() {
    {
      cache::Cache seeded{rig.records, rig.wall};
      ASSERT_TRUE(seeded.write(
          cache::LibraryRecord{libchiaking::catalog::OwnedRecord::library_of(
              owner, {}, rig.now())}));
      ASSERT_TRUE(seeded.write(
          cache::PremiumCategoryRecord::nothing_held(StoreLocale::english_us())
              .with_failure(rig.now())));
      ASSERT_TRUE(seeded.write(
          cache::ForSaleCategoryRecord::nothing_held(StoreLocale::english_us())
              .with_failure(rig.now())));
      libchiaking::catalog::RefusalMemory refusals{seeded, rig.threading};
      ASSERT_TRUE(refusals.remember(
          owner, DetailsRig::product(DetailsRig::remaster),
          libchiaking::catalog::StreamRefusal::Kind::no_game_for_entitlement,
          {}));
      const auto unreadable =
          libchiaking::platform::RecordName::parse("streamable/us-en");
      ASSERT_TRUE(unreadable);
      ASSERT_TRUE(rig.records.write_atomic(*unreadable,
                                           DetailsRig::bytes_of("not json")));
    }
    const auto shown = rig.catalog.details_of(
        *user, DetailsRig::product(DetailsRig::remaster));
    ASSERT_TRUE(shown);
    ASSERT_TRUE(
        rig.catalog.image_of(DetailsRig::image(DetailsRig::concept_logo)));
    ASSERT_TRUE(
        rig.catalog.image_of(DetailsRig::image(DetailsRig::rating_icon)));
    ASSERT_TRUE(rig.catalog.image_of(
        DetailsRig::image(std::string{DetailsRig::rating_icon} + "?w=64")));
  }

  DetailsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user;
};

TEST_F(EveryCatalogCache, IsEmptyAtFirst) { EXPECT_TRUE(all_empty()); }

TEST_F(EveryCatalogCache, IsSummarisedByEntriesAndBytes) {
  ASSERT_NO_FATAL_FAILURE(seed());
  for (const auto id : {CacheId::streamable_lists, CacheId::included_titles,
                        CacheId::for_sale_titles, CacheId::libraries,
                        CacheId::refusals, CacheId::game_details}) {
    const auto summary = summary_of(id);
    EXPECT_EQ(summary.entries, 1U) << static_cast<int>(id);
    EXPECT_GT(summary.bytes, 0U) << static_cast<int>(id);
  }
  const auto pictures = summary_of(CacheId::images);
  EXPECT_EQ(pictures.entries, 3U);
  EXPECT_EQ(pictures.bytes, 7U + 4U + 4U);
}

TEST_F(EveryCatalogCache, DescribesAKeptLibrary) {
  ASSERT_NO_FATAL_FAILURE(seed());
  const auto libraries = rig.catalog.cache_entries(CacheId::libraries);
  ASSERT_TRUE(libraries);
  ASSERT_EQ(libraries.value().size(), 1U);
  const CacheEntry& library = libraries.value().front();
  EXPECT_EQ(library.cache, CacheId::libraries);
  EXPECT_EQ(library.account, owner);
  EXPECT_FALSE(library.locale);
  EXPECT_FALSE(library.product);
  EXPECT_FALSE(library.image);
  EXPECT_EQ(library.bytes, summary_of(CacheId::libraries).bytes);
  EXPECT_EQ(library.stored_at, rig.now());
  EXPECT_EQ(library.expires_at, rig.now() + std::chrono::hours{24});
}

TEST_F(EveryCatalogCache, DescribesKeptRefusals) {
  ASSERT_NO_FATAL_FAILURE(seed());
  const auto refusals = rig.catalog.cache_entries(CacheId::refusals);
  ASSERT_TRUE(refusals);
  ASSERT_EQ(refusals.value().size(), 1U);
  EXPECT_EQ(refusals.value().front().account, owner);
  EXPECT_EQ(refusals.value().front().expires_at,
            rig.now() + std::chrono::hours{24});
}

TEST_F(EveryCatalogCache, DescribesKeptCategoriesWithoutAnExpiry) {
  ASSERT_NO_FATAL_FAILURE(seed());
  for (const auto id : {CacheId::included_titles, CacheId::for_sale_titles}) {
    const auto kept = rig.catalog.cache_entries(id);
    ASSERT_TRUE(kept) << static_cast<int>(id);
    ASSERT_EQ(kept.value().size(), 1U) << static_cast<int>(id);
    EXPECT_EQ(kept.value().front().locale, StoreLocale::english_us())
        << static_cast<int>(id);
    EXPECT_FALSE(kept.value().front().expires_at) << static_cast<int>(id);
    EXPECT_FALSE(kept.value().front().account) << static_cast<int>(id);
  }
}

TEST_F(EveryCatalogCache, DescribesAnUnreadableListByItsSize) {
  ASSERT_NO_FATAL_FAILURE(seed());
  const auto lists = rig.catalog.cache_entries(CacheId::streamable_lists);
  ASSERT_TRUE(lists);
  ASSERT_EQ(lists.value().size(), 1U);
  EXPECT_EQ(lists.value().front().bytes, 8U);
  EXPECT_FALSE(lists.value().front().locale);
  EXPECT_FALSE(lists.value().front().expires_at);
}

TEST_F(EveryCatalogCache, DescribesKeptGameDetails) {
  ASSERT_NO_FATAL_FAILURE(seed());
  const auto kept = rig.catalog.cache_entries(CacheId::game_details);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept.value().size(), 1U);
  const CacheEntry& game = kept.value().front();
  EXPECT_EQ(game.locale, StoreLocale::english_us());
  EXPECT_EQ(game.product, DetailsRig::product(DetailsRig::remaster));
  EXPECT_FALSE(game.account);
  EXPECT_FALSE(game.image);
  EXPECT_EQ(game.stored_at, rig.now());
  EXPECT_EQ(game.expires_at, rig.now() + std::chrono::hours{24});
}

TEST_F(EveryCatalogCache, DescribesKeptImages) {
  ASSERT_NO_FATAL_FAILURE(seed());
  const auto images = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(images);
  EXPECT_EQ(images.value().size(), 3U);
  const auto logo = DetailsRig::image(DetailsRig::concept_logo);
  for (const auto& entry : images.value()) {
    EXPECT_EQ(entry.cache, CacheId::images);
    EXPECT_TRUE(entry.image);
    EXPECT_FALSE(entry.account);
    EXPECT_FALSE(entry.product);
    EXPECT_FALSE(entry.expires_at);
    EXPECT_EQ(entry.stored_at, rig.now());
    EXPECT_EQ(entry.bytes, entry.image == logo ? 7U : 4U);
  }
}

TEST_F(EveryCatalogCache, ClearsGameDetailsWithTheirPictures) {
  ASSERT_NO_FATAL_FAILURE(seed());
  EXPECT_TRUE(rig.catalog.clear_cache(CacheId::game_details));
  EXPECT_EQ(summary_of(CacheId::game_details).entries, 0U);
  EXPECT_FALSE(rig.has_record(DetailsRig::details_record));
  const auto unowned = rig.catalog.cache_entries(CacheId::images);
  ASSERT_TRUE(unowned);
  ASSERT_EQ(unowned.value().size(), 1U);
  EXPECT_EQ(unowned.value().front().image,
            DetailsRig::image(std::string{DetailsRig::rating_icon} + "?w=64"));
  EXPECT_EQ(summary_of(CacheId::libraries).entries, 1U);

  const auto asked = rig.http.requests();
  EXPECT_TRUE(
      rig.catalog.details_of(*user, DetailsRig::product(DetailsRig::remaster)));
  EXPECT_EQ(rig.http.requests(), asked + 1);
}

TEST_F(EveryCatalogCache, ClearsOneCacheAtATime) {
  ASSERT_NO_FATAL_FAILURE(seed());
  EXPECT_TRUE(rig.catalog.clear_cache(CacheId::libraries));
  EXPECT_EQ(summary_of(CacheId::libraries).entries, 0U);
  EXPECT_EQ(summary_of(CacheId::refusals).entries, 1U);

  EXPECT_TRUE(rig.catalog.clear_cache(CacheId::for_sale_titles));
  EXPECT_EQ(summary_of(CacheId::for_sale_titles).entries, 0U);
  EXPECT_EQ(summary_of(CacheId::included_titles).entries, 1U);
}

TEST_F(EveryCatalogCache, IsClearedAllAtOnce) {
  ASSERT_NO_FATAL_FAILURE(seed());
  EXPECT_TRUE(rig.catalog.clear_caches());
  EXPECT_TRUE(all_empty());
  EXPECT_TRUE(rig.records.names().empty());
  EXPECT_TRUE(rig.sent_no_credential());
}

TEST_F(EveryCatalogCache, ReportsAStoreThatCannotBeListed) {
  rig.records.fail_lists = true;
  const auto unlisted = rig.catalog.caches();
  ASSERT_FALSE(unlisted);
  EXPECT_EQ(unlisted.error().reason(), Reason::storage);
}

}  // namespace
}  // namespace catalog_test
