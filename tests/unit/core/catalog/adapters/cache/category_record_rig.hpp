#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace catalog_test {

class CategoryRecordLoader {
 public:
  using Record = libchiaking::catalog::cache::PremiumCategoryRecord;
  using Demand = Record::Demand;
  using Step = Record::Step;
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  using StoreLocale = libchiaking::catalog::StoreLocale;
  using PremiumCategory = libchiaking::catalog::cloud_stream::PremiumCategory;
  using ForSaleCategory = libchiaking::catalog::cloud_stream::ForSaleCategory;
  using PremiumProduct = libchiaking::catalog::cloud_stream::PremiumProduct;
  using PremiumSku = libchiaking::catalog::cloud_stream::PremiumSku;
  using PremiumEntitlement =
      libchiaking::catalog::cloud_stream::PremiumEntitlement;

  struct Failed {};
  struct Cancelled {};

  struct Loaded {
    Step step;
    Record record;
  };

  static constexpr std::chrono::minutes minute{1};
  static constexpr std::chrono::hours hour{1};

  CategoryRecordLoader() { clock.now_us = 1'800'000'000ULL * 1'000'000ULL; }

  static auto locale(std::string_view text) -> StoreLocale {
    auto parsed = StoreLocale::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto included_game(std::size_t number) -> PremiumProduct {
    const auto title = "EP1-PPSA" + std::to_string(10000 + number) + "_00-";
    const auto game = title + "GAME";
    const auto concept_id = number % 2 == 0
                                ? libchiaking::catalog::ConceptId::of(
                                      static_cast<std::uint64_t>(number))
                                : std::nullopt;
    return PremiumProduct{PremiumProduct::Facts{
        .id = *libchiaking::catalog::ProductId::parse(game),
        .name = "Game " + std::to_string(number),
        .master_image_url = "https://image/" + std::to_string(number) + ".png",
        .concept_id = concept_id,
        .skus = {PremiumSku{
                     libchiaking::catalog::SkuId::parse(title + "SKU"),
                     true,
                     {PremiumEntitlement{
                          {.id = libchiaking::catalog::OwnershipId::parse(game),
                           .package_type = "PSGD",
                           .feature = PremiumEntitlement::Feature::full_game,
                           .streaming_supported = true}},
                      PremiumEntitlement{
                          {.unreadable_id = true,
                           .package_type = "PSTRACK",
                           .feature = PremiumEntitlement::Feature::unknown}}}},
                 PremiumSku{std::nullopt, false, {}}}}};
  }

  static auto titles(const StoreLocale& store_locale, Time fetched_at,
                     std::size_t count, std::size_t first = 1)
      -> PremiumCategory {
    std::vector<PremiumProduct> products;
    for (std::size_t index = 0; index < count; ++index)
      products.push_back(included_game(first + index));
    return PremiumCategory{store_locale,
                           std::move(products),
                           {.server_total = count + 3,
                            .products_read = count + 3,
                            .complete = true},
                           fetched_at};
  }

  static auto game_for_sale(std::size_t number) -> PremiumProduct {
    const auto title = "EP1-PPSA" + std::to_string(20000 + number) + "_00-";
    const auto game = title + "GAME";
    return PremiumProduct{PremiumProduct::Facts{
        .id = *libchiaking::catalog::ProductId::parse(game),
        .name = "Sold " + std::to_string(number),
        .master_image_url =
            "https://image/sold" + std::to_string(number) + ".png",
        .concept_id = libchiaking::catalog::ConceptId::of(
            static_cast<std::uint64_t>(900 + number)),
        .skus = {PremiumSku{
            libchiaking::catalog::SkuId::parse(title + "SKU"),
            false,
            {PremiumEntitlement{
                {.id = libchiaking::catalog::OwnershipId::parse(game),
                 .package_type = "PSGD",
                 .feature = PremiumEntitlement::Feature::full_game,
                 .streaming_supported = true}}}}}}};
  }

  static auto for_sale(const StoreLocale& store_locale, Time fetched_at,
                       std::vector<PremiumProduct> products)
      -> ForSaleCategory {
    const auto count = products.size();
    return ForSaleCategory{
        store_locale,
        std::move(products),
        {.server_total = count, .products_read = count, .complete = true},
        fetched_at};
  }

  auto now() const -> Time { return cache.now(); }

  auto load(Demand demand, std::string_view store_locale = "en-GB") -> Loaded {
    const auto asked = locale(store_locale);
    auto read = cache.read<Record>(asked);
    EXPECT_TRUE(read);
    auto record =
        read.value() ? std::move(*read.value()) : Record::nothing_held(asked);
    const auto step = record.next_step(now(), demand);
    if (step != Step::fetch) return {step, std::move(record)};
    ++fetches;
    if (std::holds_alternative<Cancelled>(next))
      return {step, std::move(record)};
    auto answered =
        std::holds_alternative<Failed>(next)
            ? record.with_failure(now())
            : record.with_answer(
                  titles(asked, now(), std::get<std::size_t>(next)), now());
    EXPECT_TRUE(cache.write(answered));
    return {step, std::move(answered)};
  }

  void save(Time fetched_at, std::string_view store_locale = "en-GB") {
    const auto saved = locale(store_locale);
    auto read = cache.read<Record>(saved);
    ASSERT_TRUE(read);
    const auto failed_at =
        read.value() ? read.value()->failed_at() : std::nullopt;
    auto record = Record::nothing_held(saved).with_answer(
        titles(saved, fetched_at, 80, 5000), fetched_at);
    EXPECT_TRUE(
        cache.write(failed_at ? record.with_failure(*failed_at) : record));
  }

  auto saved_titles(Time fetched_at,
                    std::string_view store_locale = "en-GB") const
      -> PremiumCategory {
    return titles(locale(store_locale), fetched_at, 80, 5000);
  }

  auto stored(std::string_view store_locale = "en-GB")
      -> std::optional<Record> {
    auto read = cache.read<Record>(locale(store_locale));
    EXPECT_TRUE(read);
    return std::move(read).value();
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore store{threading};
  libchiaking::testing::ManualClock clock;
  libchiaking::catalog::cache::Cache cache{store, clock};
  int fetches{};
  std::variant<std::size_t, Failed, Cancelled> next{std::size_t{82}};
};

}  // namespace catalog_test
