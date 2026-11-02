#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/game_details_record.hpp"
#include "src/core/catalog/adapters/cache/image_shelf.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/refusal_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/storage/default.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {

class BrokenStore final : public libchiaking::platform::RecordStore {
 public:
  auto read(const libchiaking::platform::RecordName&)
      -> libchiaking::Expected<libchiaking::platform::StoredRecord,
                               libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::read_failed);
  }
  auto write_atomic(const libchiaking::platform::RecordName&,
                    std::span<const std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::write_failed);
  }
  auto erase(const libchiaking::platform::RecordName&)
      -> libchiaking::Expected<void,
                               libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::remove_failed);
  }
  auto erase_prefix(const libchiaking::platform::RecordName&)
      -> libchiaking::Expected<void,
                               libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::remove_failed);
  }
  auto list(const libchiaking::platform::RecordName&) -> libchiaking::Expected<
      std::vector<libchiaking::platform::RecordListing>,
      libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::read_failed);
  }
  auto erase_legacy_file(std::string_view)
      -> libchiaking::Expected<void,
                               libchiaking::platform::StorageError> override {
    return libchiaking::unexpected(
        libchiaking::platform::StorageError::remove_failed);
  }
};

class CacheCases {
 public:
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  using AccountId = libchiaking::AccountId;
  using Platform = libchiaking::catalog::Platform;
  using ServiceGeneration = libchiaking::catalog::ServiceGeneration;
  using ListOrigin = libchiaking::catalog::ListOrigin;
  using OwnedTitle = libchiaking::catalog::OwnedTitle;
  using Tier = libchiaking::users::Tier;

  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view rivet_deluxe =
      "EP0109-PPSA07028_00-FAKE0109TITLE000";
  static constexpr std::string_view show =
      "UP0114-PPSA37004_00-FAKE0071TITLE000";
  static constexpr std::string_view god_of_war =
      "UP0114-CUSA07009_00-FAKE0051TITLE000";
  static constexpr std::string_view bus =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr AccountId first_account{1111111111111111111ULL};
  static constexpr AccountId second_account{2222222222222222222ULL};

  static auto product(std::string_view text)
      -> libchiaking::catalog::ProductId {
    return *libchiaking::catalog::ProductId::parse(text);
  }

  static auto locale(std::string_view text)
      -> libchiaking::catalog::StoreLocale {
    return *libchiaking::catalog::StoreLocale::parse(text);
  }

  static auto image(std::string_view url) -> libchiaking::catalog::ImageRef {
    return *libchiaking::catalog::ImageRef::at(url);
  }

  static auto replaced(std::string text, std::string_view from,
                       std::string_view to) -> std::string {
    const auto at = text.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return text.replace(at, from.size(), to);
  }

  static auto streamable(Time fetched_at, bool complete = true,
                         ListOrigin origin = ListOrigin::account_service)
      -> libchiaking::catalog::StreamableList {
    using libchiaking::catalog::ListStreamability;
    using libchiaking::catalog::PlusInclusion;
    using libchiaking::catalog::StreamableList;
    using libchiaking::catalog::Title;
    using libchiaking::catalog::TitleNumber;
    Title::Facts rift{.product = product(rivet)};
    rift.name = "Rivet & Bolt: Split Worlds \xE2\x84\xA2 \"Launch\"";
    rift.platform = Platform::ps5;
    rift.service = ServiceGeneration::ps5_cloud;
    rift.streamability = ListStreamability::listed;
    rift.concept_id = libchiaking::catalog::ConceptId::parse("90000016");
    rift.cover_url = "https://image.api.playstation.com/rift/cover.png";
    rift.landscape_cover_url =
        "https://image.api.playstation.com/rift/wide.png";
    rift.concept_page = libchiaking::catalog::StorePage::at(
        "https://store.playstation.com/en-gb/concept/90000016",
        libchiaking::catalog::StorePage::Source::concept_page);
    rift.aliases = {product(rivet_deluxe)};

    Title::Facts classic{.product = product(god_of_war)};
    classic.name = "Storm of Ages III Remastered";
    classic.platform = Platform::ps4;
    classic.service = ServiceGeneration::old_service;
    classic.streamability = ListStreamability::unknown;

    Title::Facts marked{.product = product(show)};
    marked.name = "League Diamond 26";
    marked.platform = Platform::ps5;
    marked.service = ServiceGeneration::ps5_cloud;
    marked.streamability = ListStreamability::marked_unstreamable;

    Title::Facts tagged_only{.product = product(bus)};
    tagged_only.name = "Coach Simulator 21";
    tagged_only.platform = Platform::ps5;
    tagged_only.service = ServiceGeneration::ps5_cloud;

    StreamableList::Facts facts{.region = locale("en-GB"),
                                .settled_locale = locale("en-US")};
    facts.origin = origin;
    facts.complete = complete;
    facts.fetched_at = fetched_at;
    facts.titles = {
        {Title::from(std::move(rift)).value(),
         *PlusInclusion::from_tier(Tier::extra,
                                   PlusInclusion::Source::store_tags)},
        {Title::from(std::move(classic)).value(),
         PlusInclusion::with_premium(PlusInclusion::Source::old_service)},
        {Title::from(std::move(marked)).value(),
         PlusInclusion::not_included(PlusInclusion::Source::store_lists)},
    };
    facts.included_but_not_streamable = {
        {Title::from(std::move(tagged_only)).value(),
         *PlusInclusion::from_tier(Tier::essential,
                                   PlusInclusion::Source::store_lists)},
    };
    facts.marked_unstreamable = {*TitleNumber::from("PPSA37004"),
                                 *TitleNumber::from("CUSA99999")};
    return StreamableList::of(std::move(facts)).value();
  }

  static auto owned(std::string_view id, Platform platform,
                    ServiceGeneration service) -> OwnedTitle::Facts {
    OwnedTitle::Facts facts{.id =
                                *libchiaking::catalog::OwnershipId::parse(id)};
    facts.name = "Owned \xC3\xA9";
    facts.platform = platform;
    facts.service = service;
    return facts;
  }

  static auto library(AccountId account, Time fetched_at)
      -> libchiaking::catalog::Library {
    using libchiaking::catalog::Library;
    auto rift = owned(rivet, Platform::ps5, ServiceGeneration::ps5_cloud);
    rift.held = libchiaking::catalog::HeldAs::full_game;
    rift.product = product(rivet);
    rift.concept_id = libchiaking::catalog::ConceptId::parse("90000016");
    rift.cover_url = "https://image.api.playstation.com/rift/cover.png";
    rift.release = libchiaking::catalog::ReleaseDate::at(
        fetched_at + std::chrono::hours{48});

    auto baseball = owned(show, Platform::ps5, ServiceGeneration::ps5_cloud);
    baseball.held = libchiaking::catalog::HeldAs::trial_access;
    baseball.release = libchiaking::catalog::ReleaseDate::unannounced();

    auto classic = owned("UP0114-CUSA07009_00-FAKE0052TITLE000", Platform::ps4,
                         ServiceGeneration::old_service);
    classic.held = libchiaking::catalog::HeldAs::disc_upgrade;

    auto other = owned(bus, Platform::ps5, ServiceGeneration::ps5_cloud);

    return Library::of(account,
                       {OwnedTitle::from(std::move(rift)).value(),
                        OwnedTitle::from(std::move(baseball)).value(),
                        OwnedTitle::from(std::move(classic)).value(),
                        OwnedTitle::from(std::move(other)).value()},
                       {Library::Bundle{product(rivet_deluxe),
                                        {product(rivet), product(show)}}},
                       fetched_at)
        .value();
  }

  static auto included(const libchiaking::catalog::StoreLocale& store_locale,
                       Time fetched_at, std::size_t count = 90)
      -> libchiaking::catalog::cloud_stream::PremiumCategory {
    using libchiaking::catalog::cloud_stream::PremiumEntitlement;
    using libchiaking::catalog::cloud_stream::PremiumProduct;
    using libchiaking::catalog::cloud_stream::PremiumSku;
    std::vector<PremiumProduct> products;
    for (std::size_t index = 0; index < count; ++index) {
      const auto id = "EP1-PPSA" + std::to_string(10000 + index) + "_00-GAME";
      products.emplace_back(PremiumProduct::Facts{
          .id = *libchiaking::catalog::ProductId::parse(id),
          .name = "Game",
          .skus = {PremiumSku{
              libchiaking::catalog::SkuId::parse(id + "-E001"),
              true,
              {PremiumEntitlement{
                  {.id = libchiaking::catalog::OwnershipId::parse(id),
                   .package_type = "PSGD",
                   .feature = PremiumEntitlement::Feature::full_game,
                   .streaming_supported = true}}}}}});
    }
    return libchiaking::catalog::cloud_stream::PremiumCategory{
        store_locale,
        std::move(products),
        {.server_total = count, .products_read = count, .complete = true},
        fetched_at};
  }

  static auto refusals(AccountId account, Time now)
      -> libchiaking::catalog::cache::RefusalRecord {
    using libchiaking::catalog::StreamRefusal;
    return *libchiaking::catalog::cache::RefusalRecord::of(
        account,
        {StreamRefusal{account,
                       product(rivet),
                       StreamRefusal::Kind::no_game_for_entitlement,
                       now,
                       {Tier::premium,
                        StreamRefusal::Holding::held_as(
                            *libchiaking::catalog::OwnershipId::parse(rivet))}},
         StreamRefusal{account,
                       product(show),
                       StreamRefusal::Kind::refused_by_stream_server,
                       now - std::chrono::hours{3},
                       {std::nullopt, StreamRefusal::Holding::not_held()}},
         StreamRefusal{account,
                       product(bus),
                       StreamRefusal::Kind::refused_by_stream_server,
                       now,
                       {Tier::none, StreamRefusal::Holding::unknown()}}});
  }
};

class CacheShelf {
 public:
  CacheShelf() { clock.now_us = 1'800'000'000ULL * 1'000'000ULL; }

  auto now() const -> CacheCases::Time { return cache.now(); }

  auto text(std::string_view name) -> std::string {
    const auto stored =
        store.read(*libchiaking::platform::RecordName::parse(name));
    EXPECT_TRUE(stored) << name;
    const auto& data = stored.value().data;
    return {reinterpret_cast<const char*>(data.data()), data.size()};
  }

  void put(std::string_view name, std::string_view text) {
    EXPECT_TRUE(
        store.write_atomic(*libchiaking::platform::RecordName::parse(name),
                           std::as_bytes(std::span{text.data(), text.size()})))
        << name;
  }

  auto has(std::string_view name) -> bool {
    return static_cast<bool>(
        store.read(*libchiaking::platform::RecordName::parse(name)));
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore store{threading};
  libchiaking::testing::ManualClock clock;
  libchiaking::catalog::cache::Cache cache{store, clock};
};

}  // namespace catalog_test
