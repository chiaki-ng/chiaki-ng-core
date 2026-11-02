#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "tests/support/setup.hpp"

#include <libchiaking/detail/catalog/catalog.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

class HeldCatalog {
 public:
  struct Listed {
    std::string_view product;
    std::string_view name{"Synthetic Game"};
    bool old_service{};
    bool owned{};
  };

  static constexpr int audience_age = 34;

  static void hold(libchiaking::platform::RecordStore& records,
                   const libchiaking::platform::ClockProvider& wall,
                   libchiaking::AccountId account, std::string_view region,
                   std::initializer_list<Listed> listed) {
    namespace catalog = libchiaking::catalog;
    catalog::cache::Cache cache{records, wall};
    const auto now = cache.now();
    const auto locale = test_support::Setup::value(
        catalog::StoreLocale::parse(region), "the region parses");

    catalog::RawListing raw{
        {locale, locale, catalog::ListOrigin::account_service, true, now}};
    std::vector<catalog::Ps5Entitlement> owned;
    for (const auto& item : listed) {
      const auto product = test_support::Setup::value(
          catalog::ProductId::parse(item.product), "the product id parses");
      catalog::ListedEntry entry{.product = product};
      entry.name = std::string{item.name};
      entry.shelf = item.old_service ? catalog::Shelf::old_service
                                     : catalog::Shelf::all_ps5;
      entry.on_ps5 = !item.old_service;
      entry.streaming_supported = true;
      raw.add(std::move(entry));
      if (!item.owned) continue;
      catalog::Ps5Entitlement::Facts facts{
          .id = *catalog::OwnershipId::parse(item.product)};
      facts.product = product;
      facts.name = std::string{item.name};
      facts.active = true;
      facts.entitlement_type = catalog::Ps5Entitlement::unified_entitlement;
      facts.feature_type = catalog::Ps5Entitlement::full_game_feature;
      facts.package_type = std::string{catalog::Ps5Entitlement::game_package};
      facts.streaming_supported = true;
      owned.emplace_back(std::move(facts));
    }
    const auto list_record = test_support::Setup::value(
        catalog::cache::StreamableListRecord::of(test_support::Setup::value(
            raw.filter_raw_streamable(), "the list filters")),
        "the list record is made");
    test_support::Setup::require(static_cast<bool>(cache.write(list_record)),
                                 "the list is cached");

    const catalog::OwnedPs5Games ps5{
        std::move(owned), catalog::LibraryState::fetched, {}, now};
    const catalog::cache::LibraryRecord library{
        catalog::OwnedRecord::library_of(account, {}, now)
            .with_ps5_part(ps5.games(), catalog::LibraryState::fetched),
        ps5};
    test_support::Setup::require(static_cast<bool>(cache.write(library)),
                                 "the library is cached");

    const auto tags =
        catalog::cache::PremiumCategoryRecord::nothing_held(locale)
            .for_age(audience_age)
            .with_answer(
                catalog::cloud_stream::PremiumCategory{locale, {}, {}, now},
                now);
    test_support::Setup::require(static_cast<bool>(cache.write(tags)),
                                 "the tags are cached");
  }
};

}  // namespace play_test
