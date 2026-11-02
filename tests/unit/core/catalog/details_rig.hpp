#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "tests/support/catalog/scripted_ps.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/catalog_types.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace catalog_test {

class GatedStore final : public libchiaking::platform::HttpProvider {
 public:
  static constexpr std::chrono::seconds patience{3};

  GatedStore(libchiaking::platform::HttpProvider& inner, int together)
      : inner_(inner), together_(together) {}

  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    {
      libchiaking::platform::Lock lock{mutex_};
      ++arrived_;
      changed_.notify_all();
      if (!changed_.wait_for(lock, patience,
                             [this] { return arrived_ >= together_; })) {
        waited_alone_ = true;
        return libchiaking::unexpected(
            libchiaking::platform::HttpError::disconnected);
      }
      if (once_together) once_together();
    }
    return inner_.perform(request, stop);
  }

  auto arrived() const -> int {
    const libchiaking::platform::Lock lock{mutex_};
    return arrived_;
  }

  auto waited_alone() const -> bool {
    const libchiaking::platform::Lock lock{mutex_};
    return waited_alone_;
  }

  std::function<void()> once_together{};

 private:
  libchiaking::platform::HttpProvider& inner_;
  int together_;
  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::platform::SteadyClock steady_;
  mutable libchiaking::platform::Mutex mutex_{threading_};
  libchiaking::platform::ConditionVariable changed_{threading_, steady_};
  int arrived_{};
  bool waited_alone_{};
};

class DetailsRig : public test_support::catalog::CatalogRig {
 public:
  using Catalog = libchiaking::catalog::Catalog;
  using GameDetails = libchiaking::catalog::GameDetails;
  using ImageRef = libchiaking::catalog::ImageRef;
  using Unanswered = libchiaking::catalog::Unanswered;
  using Bytes = std::vector<std::byte>;
  using Json = test_support::JsonTree;
  using Fixtures = test_support::psn::Fixtures;

  static constexpr std::string_view remaster =
      "EP0109-PPSA17001_00-FAKE0056TITLE000";
  static constexpr std::string_view sold_fixture =
      "catalog_products_FAKE0056TITLE000.json";
  static constexpr std::string_view details_query =
      "age=34&includeConcept=true";
  static constexpr std::string_view store_products =
      "https://m.np.playstation.com/api/catalog/v2/products?";
  static constexpr std::string_view concept_media_query =
      "operationName=conceptRetrieveForMedia";
  static constexpr std::string_view concept_stars_query =
      "operationName=wcaConceptStarRatingRetrive";
  static constexpr std::string_view concept_age_query =
      "operationName=conceptRetrieveForContentRating";
  static constexpr std::string_view concept_info_query =
      "operationName=conceptRetrieveForGameInfo";
  static constexpr std::array concept_queries{
      concept_media_query, concept_stars_query, concept_age_query,
      concept_info_query};
  static constexpr std::string_view complete_edition =
      "EP0109-CUSA17003_00-FAKE0059TITLE000";
  static constexpr std::string_view other_edition =
      "EP0109-CUSA17003_00-FAKE0060TITLE000";
  static constexpr std::string_view ghost_ship =
      "EP0109-CUSA37002_00-FAKE0050TITLE000";
  static constexpr std::string_view ghost_reserved =
      "EP0109-CUSA37002_00-FAKE0107TITLE000";
  static constexpr std::string_view ghost_ps5 =
      "EP0109-PPSA07036_00-FAKE0049TITLE000";
  static constexpr std::string_view concept_record =
      "details/en-gb/concept-900002";
  static constexpr std::string_view us_concept_record =
      "details/en-us/concept-900002";
  static constexpr std::string_view ghost_record =
      "details/en-gb/concept-900001";
  static constexpr std::string_view graphql =
      "https://web.np.playstation.com/api/graphql/v1/op?";
  static constexpr std::string_view details_record =
      "details/en-us/ep0109-ppsa17001_00-fake0056title000";
  static constexpr std::string_view owned_list = "entitlement_type=5";
  static constexpr std::string_view app_library = "gameMetaPackageType=PS4GS";
  static constexpr std::string_view ps5_library = "gameMetaPackageType=PSGD";
  static constexpr std::string_view app_library_url =
      "https://m.np.playstation.com/api/entitlement/v2/users/me/internal/"
      "entitlements"
      "?isActive=true&gameMetaPackageType=PS4GS&limit=20&offset=0&fields="
      "conceptMeta,"
      "titleMeta";
  static constexpr std::string_view owned_ghost_row =
      R"({"id":"EP0109-CUSA37002_00-FAKE0107TITLE000","product_id":"EP0109-CUSA37002_00-FAKE0050TITLE000","active_flag":true,"feature_type":3,"entitlement_attributes":[{"platform_id":"ps4"}],"game_meta":{"name":"Ghost of Isles","package_type":"PS4GS","type":"PS4GS"}})";
  static constexpr std::string_view owned_ps5_row =
      R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000","productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"name":"Rivet","packageType":"PSGD"},"streamingSupported":true})";
  static constexpr std::string_view concept_background =
      "https://image.api.playstation.com/vulcan/ap/rnd/202010/0221/"
      "1cwJLCDoAZuhEqavJarDP7ll.png";
  static constexpr std::string_view concept_hero =
      "https://image.api.playstation.com/vulcan/ap/rnd/202010/0221/"
      "oGoXCdtWZtctlkHZZrtr4wpp.png";
  static constexpr std::string_view concept_logo =
      "https://image.api.playstation.com/vulcan/ap/rnd/202010/0221/"
      "paGPGKzDTGWSsQCVQZTWelTt.png";
  static constexpr std::string_view concept_cover =
      "https://image.api.playstation.com/vulcan/ap/rnd/202010/0221/"
      "cdL0GuG7hnjZNRGnp91koowV.png";
  static constexpr std::string_view product_cover =
      "https://image.api.playstation.com/vulcan/ap/rnd/202409/2716/"
      "5fd726c4304def9de2e215b236b2d7662333080833657ca5.jpg";
  static constexpr std::string_view product_logo_stem =
      "https://image.api.playstation.com/vulcan/ap/rnd/202409/2716/"
      "3fd03cc38ddde9d4b8322cbed5e793283e2f80e5d5ea8105.";
  static constexpr std::string_view product_trailer =
      "https://vulcan.dl.playstation.net/img/rnd/202410/2917/"
      "c6d21172c71fcb5e03174a848b11b5608961d174cf4a923c.mp4";
  static constexpr std::string_view rating_icon =
      "https://image.api.playstation.com/grc/images/ratings/4k/pegi/16.png";
  static constexpr std::string_view logo_file = "paGPGKzDTGWSsQCVQZTWelTt.png";
  static constexpr std::string_view hero_file = "oGoXCdtWZtctlkHZZrtr4wpp.png";
  static constexpr std::string_view background_file =
      "1cwJLCDoAZuhEqavJarDP7ll.png";

  static auto started(libchiaking::platform::ThreadingProvider& threading,
                      std::function<void()> work)
      -> libchiaking::platform::Thread {
    return libchiaking::platform::Thread::start(
               threading, "catalog-test",
               libchiaking::platform::ThreadRole::unspecified, std::move(work))
        .value();
  }

  static void pause_for(libchiaking::platform::ThreadingProvider& threading,
                        std::chrono::milliseconds time) {
    const libchiaking::platform::SteadyClock steady;
    libchiaking::platform::Sleeper sleeper{threading, steady};
    static_cast<void>(sleeper.sleep_for(time));
  }

  static auto concept_id(std::string_view text)
      -> libchiaking::catalog::ConceptId {
    auto parsed = libchiaking::catalog::ConceptId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto british() -> libchiaking::catalog::StoreLocale {
    auto parsed = libchiaking::catalog::StoreLocale::parse("en-GB");
    EXPECT_TRUE(parsed);
    return std::move(parsed).value();
  }

  static auto gb() -> libchiaking::users::Region {
    return libchiaking::users::Region{"GB", "en"};
  }

  static auto image(std::string_view url) -> ImageRef {
    auto parsed = ImageRef::at(url);
    EXPECT_TRUE(parsed) << url;
    return std::move(parsed).value();
  }

  static auto layers_of(const GameDetails& details)
      -> std::optional<libchiaking::catalog::LayeredArt> {
    if (!details.key_art) return std::nullopt;
    const auto* layers = std::get_if<libchiaking::catalog::LayeredArt>(
        &details.key_art->choice());
    return layers ? std::optional{*layers} : std::nullopt;
  }

  static auto game_hub_of(const GameDetails& details)
      -> std::optional<libchiaking::catalog::GameHubArt> {
    if (!details.key_art) return std::nullopt;
    const auto* cover = std::get_if<libchiaking::catalog::GameHubArt>(
        &details.key_art->choice());
    return cover ? std::optional{*cover} : std::nullopt;
  }

  static auto bytes_of(std::string_view text) -> Bytes {
    const auto raw = std::as_bytes(std::span{text.data(), text.size()});
    return {raw.begin(), raw.end()};
  }

  static auto details_query_of(std::string_view product_id,
                               std::string_view country) -> std::string {
    return "productIds=" + std::string{product_id} +
           "&country=" + std::string{country} + "&language=en&" +
           std::string{details_query};
  }

  static auto sold_as(std::string_view product_id) -> std::string {
    auto body = Fixtures::text(sold_fixture);
    for (auto at = body.find(remaster); at != std::string::npos;
         at = body.find(remaster, at + product_id.size()))
      body.replace(at, remaster.size(), product_id);
    return body;
  }

  static auto not_sold(std::string_view product_id) -> std::string {
    return R"([{"id":")" + std::string{product_id} +
           R"(","country":"GB","language":"en","revisionIds":{},"bucketPrices":{},)"
           R"("contentTypeId":0,"errorCode":3166081}])";
  }

  static auto sold_without(std::string_view level, const char* list,
                           std::string_view type) -> std::string {
    auto root = Fixtures::json(Fixtures::text(sold_fixture));
    auto& sold = root[0];
    auto& owner = level == "concept" ? sold["concept"] : sold;
    auto& shown = owner["media"][list];
    auto kept = Json::array();
    for (const auto& entry : Json::elements(shown))
      if (Json::text_at(entry, "type") != type)
        Json::elements(kept).push_back(entry);
    EXPECT_LT(Json::elements(kept).size(), Json::elements(shown).size())
        << type;
    shown = std::move(kept);
    return Json::dump(root);
  }

  static auto sold_lacking(
      std::initializer_list<std::pair<std::string_view, std::string_view>>
          removed) -> std::string {
    auto root = Fixtures::json(Fixtures::text(sold_fixture));
    for (const auto& [level, type] : removed) {
      auto& owner = level == "concept" ? root[0]["concept"] : root[0];
      auto& shown = owner["media"]["images"];
      auto kept = Json::array();
      for (const auto& entry : Json::elements(shown))
        if (Json::text_at(entry, "type") != type)
          Json::elements(kept).push_back(entry);
      EXPECT_LT(Json::elements(kept).size(), Json::elements(shown).size())
          << level << " " << type;
      shown = std::move(kept);
    }
    return Json::dump(root);
  }

  static auto streamed_copies() -> std::string {
    auto body = Fixtures::text("entitlements_v2_ps4gs_synthetic.json");
    constexpr std::string_view total = "\"totalResults\": 5";
    const auto at = body.find(total);
    EXPECT_NE(at, std::string::npos);
    return body.replace(at, total.size(), "\"totalResults\": 2");
  }

  auto restarted_over(libchiaking::platform::HttpProvider& provider)
      -> Catalog {
    return libchiaking::testing::ServiceBuilder::build<Catalog>(
        provider, records, threading, clock, wall, server, budget);
  }

  auto restarted() -> Catalog { return restarted_over(http); }

  void script_whole_store() {
    http.on("GET", std::string{details_query},
            {Reply{200, Fixtures::text(sold_fixture)}});
  }

  void the_store_sells_nothing() {
    http.on("GET", std::string{details_query}, {Reply{200, "[]"}});
  }

  void the_store_answers(std::string body) {
    http.on("GET", std::string{details_query}, {Reply{200, std::move(body)}});
  }

  void script_concept_store() {
    http.on("GET", std::string{concept_media_query},
            {Reply{200, Fixtures::text("concept_media_900002.json")}});
    http.on("GET", std::string{concept_stars_query},
            {Reply{200,
                   Fixtures::text("concept_star_rating_900002.derived.json")}});
    http.on("GET", std::string{concept_age_query},
            {Reply{200, Fixtures::text(
                            "concept_content_rating_900002.derived.json")}});
    http.on(
        "GET", std::string{concept_info_query},
        {Reply{200, Fixtures::text("concept_game_info_900002.derived.json")}});
  }

  void script_concept_store_as(std::string_view concept_text) {
    const auto renamed = [concept_text](std::string_view fixture) {
      auto body = Fixtures::text(fixture);
      constexpr std::string_view fixture_concept = "900002";
      for (auto at = body.find(fixture_concept); at != std::string::npos;
           at = body.find(fixture_concept, at + concept_text.size()))
        body.replace(at, fixture_concept.size(), concept_text);
      return body;
    };
    http.on("GET", std::string{concept_media_query},
            {Reply{200, renamed("concept_media_900002.json")}});
    http.on("GET", std::string{concept_stars_query},
            {Reply{200, renamed("concept_star_rating_900002.derived.json")}});
    http.on(
        "GET", std::string{concept_age_query},
        {Reply{200, renamed("concept_content_rating_900002.derived.json")}});
    http.on("GET", std::string{concept_info_query},
            {Reply{200, renamed("concept_game_info_900002.derived.json")}});
  }

  void hold_a_list_of_old_service_games(libchiaking::AccountId account) {
    namespace games = libchiaking::catalog;
    using games::Title;
    const auto grouped = games::ConceptId::parse("900002");
    ASSERT_TRUE(grouped);
    Title::Facts complete{.product = product(complete_edition)};
    complete.name = "Skyline Zero Ember Complete Edition";
    complete.platform = games::Platform::ps4;
    complete.service = games::ServiceGeneration::old_service;
    complete.concept_id = grouped;
    Title::Facts deluxe{.product = product(other_edition)};
    deluxe.name = "Skyline Zero Ember Digital Deluxe";
    deluxe.platform = games::Platform::ps4;
    deluxe.service = games::ServiceGeneration::old_service;
    deluxe.concept_id = grouped;
    Title::Facts ungrouped{.product = product(ghost_ship)};
    ungrouped.name = "Ghost Ship";
    ungrouped.platform = games::Platform::ps4;
    ungrouped.service = games::ServiceGeneration::old_service;

    const auto locale = games::StoreLocale::parse("en-GB");
    ASSERT_TRUE(locale);
    games::StreamableList::Facts facts{.region = *locale,
                                       .settled_locale = *locale};
    facts.origin = games::ListOrigin::account_service;
    facts.complete = true;
    facts.fetched_at = now();
    for (auto* title : {&complete, &deluxe, &ungrouped})
      facts.titles.push_back({Title::from(std::move(*title)).value(),
                              games::PlusInclusion::with_premium(
                                  games::PlusInclusion::Source::old_service)});
    auto list = games::StreamableList::of(std::move(facts));
    ASSERT_TRUE(list);
    const auto record =
        games::cache::StreamableListRecord::of(std::move(list.value()));
    ASSERT_TRUE(record);
    games::cache::Cache seeded{records, wall};
    EXPECT_TRUE(seeded.write(*record));
    EXPECT_TRUE(seeded.write(games::cache::LibraryRecord{
        games::OwnedRecord::library_of(account, {}, now())}));
  }

  void hold_matched_games(const libchiaking::catalog::StreamableList& list,
                          const libchiaking::catalog::Library& library) {
    wall.now_us = 3'000'000;
    clock.now_us = 3'000'000;
    const auto record =
        libchiaking::catalog::cache::StreamableListRecord::of(list);
    ASSERT_TRUE(record);
    libchiaking::catalog::cache::Cache seeded{records, wall};
    EXPECT_TRUE(seeded.write(*record));
    EXPECT_TRUE(
        seeded.write(libchiaking::catalog::cache::LibraryRecord{library}));
  }

  void ps_holds_the_library(std::string_view rows,
                            std::string_view ps5_rows = {}) {
    wall.now_us = 3'000'000;
    clock.now_us = 3'000'000;
    answers_the_library_token();
    http.on("GET", std::string{owned_list},
            {Reply{200, R"({"entitlements":[)" + std::string{rows} + "]}"}});
    http.on("GET", std::string{ps5_library},
            {Reply{200,
                   std::string{R"({"revisionId":1,"start":0,"totalResults":)"} +
                       (ps5_rows.empty() ? "0" : "1") + R"(,"entitlements":[)" +
                       std::string{ps5_rows} + "]}"}});
    const auto record = libchiaking::catalog::cache::StreamableListRecord::of(
        RuleCases::list_of({{.product = ghost_ps5,
                             .name = "Ghost of Isles DIRECTOR'S CUT",
                             .concept_id = "900001"}}));
    ASSERT_TRUE(record);
    libchiaking::catalog::cache::Cache seeded{records, wall};
    EXPECT_TRUE(seeded.write(*record));
  }

  void answers_the_library_token() {
    http.on(
        "GET", "response_type=token",
        {Reply{302, "",
               test_support::catalog::ScriptedPs::location(
                   "https://x/"
                   "grc-response.html#access_token=TOKEN1&expires_in=3600")}});
  }

  auto sent_no_credential() const -> testing::AssertionResult {
    int checked = 0;
    for (const auto& request : http.seen()) {
      const bool ours =
          request.url.starts_with(ImageRef::origins.front()) ||
          request.url.find(details_query) != std::string::npos ||
          std::ranges::any_of(concept_queries, [&](std::string_view query) {
            return request.url.find(query) != std::string::npos;
          });
      if (!ours) continue;
      ++checked;
      for (const auto& header : request.headers) {
        if (header.name == "Authorization" || header.name == "Cookie")
          return testing::AssertionFailure()
                 << request.url << " carried " << header.name;
        if (header.value.find("NPSSO") != std::string::npos)
          return testing::AssertionFailure()
                 << request.url << " carried the NPSSO in " << header.name;
        if (header.value.find("Bearer") != std::string::npos)
          return testing::AssertionFailure()
                 << request.url << " carried a bearer in " << header.name;
      }
    }
    if (checked == 0)
      return testing::AssertionFailure() << "no public request was made";
    return testing::AssertionSuccess();
  }

  auto made_only_the_four_public_requests(std::size_t since) const
      -> testing::AssertionResult {
    const auto seen = http.seen();
    if (seen.size() != since + 4)
      return testing::AssertionFailure()
             << seen.size() - since << " requests were made, not 4";
    for (auto at = since; at < seen.size(); ++at) {
      const auto& request = seen[at];
      if (request.method != "GET")
        return testing::AssertionFailure()
               << request.url << " was asked with " << request.method;
      if (!request.url.starts_with(graphql))
        return testing::AssertionFailure() << request.url << " is not GraphQL";
      if (!std::ranges::any_of(concept_queries, [&](std::string_view query) {
            return request.url.find(query) != std::string::npos;
          }))
        return testing::AssertionFailure()
               << request.url << " is not a concept query";
      for (const std::string_view elsewhere :
           {"m.np.playstation.com", "commerce.api.np", "account.sony.com",
            "psnow.playstation.com", "store.playstation.com"})
        if (request.url.find(elsewhere) != std::string::npos)
          return testing::AssertionFailure()
                 << request.url << " names " << elsewhere;
      for (const auto& header : request.headers)
        if (header.name == "Authorization" || header.name == "Cookie")
          return testing::AssertionFailure()
                 << request.url << " carried " << header.name;
    }
    return testing::AssertionSuccess();
  }
};

}  // namespace catalog_test
