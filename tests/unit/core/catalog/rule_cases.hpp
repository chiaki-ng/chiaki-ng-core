#pragma once

#include "src/core/catalog/model/cloud_stream/for_sale_category.hpp"
#include "src/core/catalog/model/cloud_stream/premium_category.hpp"
#include "src/core/catalog/model/library/owned_ps5_games.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/model/my_games/my_games_assembly.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/detail/catalog/explanation.hpp>
#include <libchiaking/detail/catalog/launch_verdict.hpp>
#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/my_game.hpp>
#include <libchiaking/detail/catalog/my_games.hpp>
#include <libchiaking/detail/catalog/streamable_list.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {

class RuleCases {
 public:
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  using ProductId = libchiaking::catalog::ProductId;
  using Shelf = libchiaking::catalog::Shelf;
  using OwnedRecord = libchiaking::catalog::OwnedRecord;
  using Ps5Entitlement = libchiaking::catalog::Ps5Entitlement;
  using PremiumEntitlement =
      libchiaking::catalog::cloud_stream::PremiumEntitlement;

  static constexpr libchiaking::AccountId account{42};
  static constexpr std::int64_t library_fetched_ms = 2000;

  struct Listed {
    std::string_view product;
    std::string_view name;
    Shelf shelf{Shelf::all_ps5};
    std::string_view concept_id{};
    bool ps5{true};
    bool ps4{false};
    std::optional<bool> streams{true};
    std::string_view cover{};
  };

  struct Held {
    std::string_view id;
    std::string_view product;
    std::string_view name;
    std::string_view platform{"ps5"};
    int feature{OwnedRecord::full_game_feature};
    std::string_view package{"PS5GD"};
    bool active{true};
    bool details{true};
    std::string_view sku{};
    std::optional<std::string> active_date{};
    bool preorder{};
    std::string_view concept_id{};
    std::string_view cover{};
  };

  struct Ps5Held {
    std::string_view id;
    std::string_view product;
    std::string_view name;
    std::int64_t feature{Ps5Entitlement::full_game_feature};
    std::string_view package{Ps5Entitlement::game_package};
    std::optional<bool> streams{true};
    bool active{true};
    std::int64_t entitlement_type{Ps5Entitlement::unified_entitlement};
    std::string_view title{};
    std::string_view sku{};
    std::string_view concept_id{};
    bool preorder_core{};
    std::optional<std::string> playable_from{};
    std::optional<bool> play_time_used_up{};
  };

  struct Granted {
    std::string_view id;
    PremiumEntitlement::Feature feature{PremiumEntitlement::Feature::full_game};
    std::string_view package{PremiumEntitlement::game_package};
    std::optional<bool> streams{true};
  };

  struct Sold {
    std::string_view sku;
    bool branded{true};
    std::vector<Granted> entitlements{};
  };

  struct Offered {
    std::string_view product;
    std::string_view name;
    std::vector<Sold> skus{};
    std::string_view concept_id{};
    std::string_view image{};
  };

  static auto at_ms(std::int64_t value) -> Time {
    return Time{std::chrono::milliseconds{value}};
  }

  static auto product(std::string_view text) -> ProductId {
    auto parsed = ProductId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto number(std::string_view text)
      -> libchiaking::catalog::TitleNumber {
    auto parsed = libchiaking::catalog::TitleNumber::from(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto en_gb() -> libchiaking::catalog::StoreLocale {
    auto locale = libchiaking::catalog::StoreLocale::parse("en-GB");
    EXPECT_TRUE(locale);
    return std::move(locale).value();
  }

  static auto on_old_service(std::string_view id, std::string_view name,
                             std::string_view concept_id = {}) -> Listed {
    return Listed{.product = id,
                  .name = name,
                  .shelf = Shelf::old_service,
                  .concept_id = concept_id,
                  .ps5 = false};
  }

  static auto raw_of(std::initializer_list<Listed> listed,
                     libchiaking::catalog::ListOrigin origin =
                         libchiaking::catalog::ListOrigin::account_service)
      -> libchiaking::catalog::RawListing {
    const auto locale = en_gb();
    libchiaking::catalog::RawListing raw{
        {locale, locale, origin, true, at_ms(1000)}};
    for (const auto& item : listed) {
      libchiaking::catalog::ListedEntry entry{.product = product(item.product)};
      entry.name = std::string{item.name};
      entry.shelf = item.shelf;
      entry.concept_id =
          libchiaking::catalog::ConceptId::parse(item.concept_id);
      entry.on_ps5 = item.ps5;
      entry.on_ps4 = item.ps4;
      entry.streaming_supported = item.streams;
      entry.cover_url = std::string{item.cover};
      raw.add(std::move(entry));
    }
    return raw;
  }

  static auto list_of(std::initializer_list<Listed> listed,
                      libchiaking::catalog::ListOrigin origin =
                          libchiaking::catalog::ListOrigin::account_service)
      -> libchiaking::catalog::StreamableList {
    auto list = raw_of(listed, origin).filter_raw_streamable();
    EXPECT_TRUE(list);
    return std::move(list).value();
  }

  static auto record(const Held& held) -> OwnedRecord {
    OwnedRecord::Fields fields;
    fields.id = std::string{held.id};
    fields.product_id = std::string{held.product};
    fields.name = std::string{held.name};
    fields.has_game_details = held.details;
    fields.active = held.active;
    fields.feature_type = held.feature;
    fields.package_type = std::string{held.package};
    fields.sku_type = std::string{held.sku};
    fields.platform_id = std::string{held.platform};
    fields.active_date = held.active_date;
    fields.preorder = held.preorder;
    fields.cover_url = std::string{held.cover};
    fields.concept_id = std::string{held.concept_id};
    return OwnedRecord{std::move(fields)};
  }

  static auto library_of(std::initializer_list<Held> held)
      -> libchiaking::catalog::Library {
    std::vector<OwnedRecord> records;
    for (const auto& item : held) records.push_back(record(item));
    return OwnedRecord::library_of(account, records, at_ms(library_fetched_ms));
  }

  static auto cloud_streaming(
      libchiaking::users::CloudBenefits::Authorisation authorisation,
      std::vector<std::string_view> plans = {})
      -> libchiaking::users::CloudStreaming {
    std::vector<libchiaking::users::BenefitPlanId> held;
    for (const auto plan : plans)
      held.push_back(*libchiaking::users::BenefitPlanId::parse(plan));
    std::vector<libchiaking::users::CloudBenefits::Owner> owners;
    owners.push_back(
        {.account_id = "1", .tier_label = "TIER_30", .plans = std::move(held)});
    return libchiaking::users::CloudStreaming::of(
        libchiaking::users::CloudBenefits{authorisation, std::nullopt,
                                          std::move(owners), at_ms(2000)});
  }

  static auto streaming_allowed() -> libchiaking::users::CloudStreaming {
    return cloud_streaming(
        libchiaking::users::CloudBenefits::Authorisation::granted,
        {"PLAN-PREMIUM"});
  }

  static auto streaming_refused() -> libchiaking::users::CloudStreaming {
    return cloud_streaming(
        libchiaking::users::CloudBenefits::Authorisation::denied);
  }

  static auto ps5_entitlement(const Ps5Held& held) -> Ps5Entitlement {
    Ps5Entitlement::Facts facts{
        .id = *libchiaking::catalog::OwnershipId::parse(held.id)};
    facts.product = ProductId::parse(held.product);
    facts.sku = libchiaking::catalog::SkuId::parse(held.sku);
    facts.title = libchiaking::catalog::TitleId::parse(held.title);
    facts.concept_id = libchiaking::catalog::ConceptId::parse(held.concept_id);
    facts.name = std::string{held.name};
    facts.active = held.active;
    facts.entitlement_type = held.entitlement_type;
    facts.feature_type = held.feature;
    facts.package_type = std::string{held.package};
    facts.streaming_supported = held.streams;
    facts.preorder_core = held.preorder_core;
    facts.playable_from = held.playable_from;
    facts.play_time_used_up = held.play_time_used_up;
    return Ps5Entitlement{std::move(facts)};
  }

  static auto ps5_games(std::initializer_list<Ps5Held> held,
                        libchiaking::catalog::LibraryState state =
                            libchiaking::catalog::LibraryState::fetched)
      -> libchiaking::catalog::OwnedPs5Games {
    std::vector<Ps5Entitlement> records;
    for (const auto& item : held) records.push_back(ps5_entitlement(item));
    return libchiaking::catalog::OwnedPs5Games{
        std::move(records), state, {held.size(), 0}, at_ms(library_fetched_ms)};
  }

  static auto premium_product(const Offered& offered)
      -> libchiaking::catalog::cloud_stream::PremiumProduct {
    namespace cloud_stream = libchiaking::catalog::cloud_stream;
    std::vector<cloud_stream::PremiumSku> skus;
    for (const auto& sold : offered.skus) {
      std::vector<PremiumEntitlement> granted;
      for (const auto& entitlement : sold.entitlements)
        granted.emplace_back(PremiumEntitlement::Facts{
            .id = libchiaking::catalog::OwnershipId::parse(entitlement.id),
            .package_type = std::string{entitlement.package},
            .feature = entitlement.feature,
            .streaming_supported = entitlement.streams});
      skus.emplace_back(libchiaking::catalog::SkuId::parse(sold.sku),
                        sold.branded, std::move(granted));
    }
    cloud_stream::PremiumProduct::Facts facts{.id = product(offered.product)};
    facts.name = std::string{offered.name};
    facts.master_image_url = std::string{offered.image};
    facts.concept_id =
        libchiaking::catalog::ConceptId::parse(offered.concept_id);
    facts.skus = std::move(skus);
    return cloud_stream::PremiumProduct{std::move(facts)};
  }

  static auto included(std::string_view id, std::string_view name,
                       std::string_view concept_id = {}) -> Offered {
    return Offered{
        .product = id,
        .name = name,
        .skus = {Sold{.sku = "SKU1", .entitlements = {Granted{.id = id}}}},
        .concept_id = concept_id};
  }

  static auto category_of(std::initializer_list<Offered> offered)
      -> libchiaking::catalog::cloud_stream::PremiumCategory {
    std::vector<libchiaking::catalog::cloud_stream::PremiumProduct> products;
    for (const auto& item : offered) products.push_back(premium_product(item));
    const auto locale = en_gb();
    const auto count = products.size();
    return libchiaking::catalog::cloud_stream::PremiumCategory{
        locale, std::move(products), {count, count, true}, at_ms(1500)};
  }

  static auto for_sale_of(std::initializer_list<Offered> offered)
      -> libchiaking::catalog::cloud_stream::ForSaleCategory {
    std::vector<libchiaking::catalog::cloud_stream::PremiumProduct> products;
    for (const auto& item : offered) products.push_back(premium_product(item));
    const auto locale = en_gb();
    const auto count = products.size();
    return libchiaking::catalog::cloud_stream::ForSaleCategory{
        locale, std::move(products), {count, count, true}, at_ms(1600)};
  }

  static auto plausible_category_of(std::initializer_list<Offered> offered)
      -> libchiaking::catalog::cloud_stream::PremiumCategory {
    std::vector<libchiaking::catalog::cloud_stream::PremiumProduct> products;
    for (const auto& item : offered) products.push_back(premium_product(item));
    for (std::size_t index = 0;
         index <
         libchiaking::catalog::cloud_stream::PremiumCategory::fewest_plausible;
         ++index) {
      const auto id =
          "EP0100-PPSA" + std::to_string(90000 + index) + "_00-FILLER";
      products.push_back(premium_product(included(id, "Filler")));
    }
    const auto locale = en_gb();
    const auto count = products.size();
    return libchiaking::catalog::cloud_stream::PremiumCategory{
        locale, std::move(products), {count, count, true}, at_ms(1500)};
  }

  static auto games_of(const libchiaking::catalog::Library& library,
                       const libchiaking::catalog::StreamableList& list,
                       const libchiaking::users::Membership& membership = {},
                       const std::optional<libchiaking::users::CloudStreaming>&
                           streaming = std::nullopt)
      -> libchiaking::catalog::MyGames {
    return libchiaking::catalog::MyGamesAssembly::games_of(
        {.library = library, .list = &list}, membership, streaming, {}, 0,
        at_ms(3000));
  }

  static auto ps5_games_of(
      const libchiaking::catalog::OwnedPs5Games* owned,
      const libchiaking::catalog::cloud_stream::PremiumCategory* category,
      const std::optional<libchiaking::users::CloudStreaming>& streaming =
          std::nullopt,
      const libchiaking::users::Membership& membership = {},
      const libchiaking::catalog::StreamableList* list = nullptr,
      const libchiaking::catalog::cloud_stream::ForSaleCategory* for_sale =
          nullptr,
      std::span<const ProductId> used_trials = {})
      -> libchiaking::catalog::MyGames {
    auto library = library_of({});
    const auto state = owned != nullptr
                           ? owned->state()
                           : libchiaking::catalog::LibraryState::unavailable;
    library = library.with_ps5_part(
        owned != nullptr ? owned->games()
                         : std::vector<libchiaking::catalog::OwnedTitle>{},
        state);
    return libchiaking::catalog::MyGamesAssembly::games_of(
        {.library = library,
         .list = list,
         .ps5 = owned,
         .category = category,
         .for_sale = for_sale,
         .used_trials = used_trials,
         .used_trials_revision = used_trials.size()},
        membership, streaming, {}, 0, at_ms(3000));
  }

  static auto game_of(const libchiaking::catalog::MyGames& games,
                      std::string_view id)
      -> const libchiaking::catalog::MyGame& {
    const auto* game = games.find(product(id));
    EXPECT_NE(game, nullptr) << id;
    if (game == nullptr) throw std::out_of_range{std::string{id}};
    return *game;
  }

  static auto failed_rule(const libchiaking::catalog::Explanation& explanation)
      -> std::optional<libchiaking::catalog::Explanation::Rule> {
    const auto* failed = explanation.first_failure();
    return failed != nullptr ? std::optional{failed->rule} : std::nullopt;
  }

  static auto has_step(const libchiaking::catalog::Explanation& explanation,
                       libchiaking::catalog::Explanation::Rule rule) -> bool {
    return std::ranges::find(explanation.steps(), rule,
                             &libchiaking::catalog::Explanation::Step::rule) !=
           explanation.steps().end();
  }
};

}  // namespace catalog_test
