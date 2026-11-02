#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/catalog/scripted_ps.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/catalog_types.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_api_test {

namespace catalog = libchiaking::catalog;
namespace users = libchiaking::users;

using catalog::Catalog;
using catalog::CloudAccess;
using catalog::FetchActivity;
using catalog::FetchState;
using catalog::FetchStep;
using catalog::HideReason;
using catalog::LaunchCheck;
using catalog::LaunchRefusal;
using catalog::ListOrigin;
using catalog::OwnershipAnswer;
using catalog::Platform;
using catalog::PlayPath;
using catalog::ServiceGeneration;
using catalog::Standing;
using catalog::StepOutcome;
using catalog::StorefrontAnswer;
using catalog::StreamRefusal;
using libchiaking::AccountId;
using users::Tier;
using Rig = test_support::catalog::CatalogRig;
using Reply = Rig::Reply;

class PsReplies {
 public:
  static constexpr std::string_view classic =
      "EP-CUSA00001_00-LEGACY_CLOUD_STREAM";
  static constexpr std::string_view comet = "EP-PPSA10001_00-GAME";
  static constexpr std::string_view rivet =
      "EP0109-PPSA07028_00-FAKE0110TITLE000";
  static constexpr std::string_view listed_only = "EP-PPSA10002_00-LISTED";
  static constexpr std::string_view bus_listing =
      "EP0106-PPSA07037_00-FAKE0016TITLE000";
  static constexpr std::string_view bus_claim_id =
      "EP0106-PPSA07037_00-FAKE0014TITLE000";
  static constexpr std::string_view for_sale_only =
      "EP0109-PPSA47002_00-FAKE0017TITLE000";
  static constexpr std::string_view sold_trial =
      "EP0109-PPSA47003_00-FAKE0132TITLE000";
  static constexpr std::string_view sold_offline =
      "EP0109-PPSA47004_00-FAKE0080TITLE000";
  static constexpr std::string_view nowhere = "EP-PPSA77003_00-NOWHERE";

  static constexpr std::string_view store_app =
      "operationName=metGetConceptByProductIdQuery";
  static constexpr std::string_view subscriptions =
      "operationName=getSubscriptions";
  static constexpr std::string_view cloud_benefits =
      "premium/features/cloudgame";
  static constexpr std::string_view plus_services = "services/pssubscriptions";
  static constexpr std::string_view tag_grid =
      "/api/catalog/v2/categories/a23e4f41-6e78-4dab-95e2-af1c10e4ab96?";
  static constexpr std::string_view store_grid = Rig::store_streaming_category;
  static constexpr std::string_view owned_list = "entitlement_type=5";
  static constexpr std::string_view owned_lookup = "internal_entitlements/";
  static constexpr std::string_view ps5_owned_list =
      "internal/entitlements?isActive=true&gameMetaPackageType=PSGD";
  static constexpr std::string_view ps5_owned_lookup = "internal/entitlements/";
  static constexpr std::string_view old_service_stores = "/user/stores";
  static constexpr std::string_view all_ps5_list = "categoryList=all-ps5-list";
  static constexpr std::string_view device_check = "/oauth/authorizeCheck";

  static inline const std::string comet_listed =
      R"({"productId":"EP-PPSA10001_00-GAME","name":"Comet","conceptId":1,"device":["PS5"],"streamingSupported":true})";
  static inline const std::string rivet_listed =
      R"({"productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","name":"Rivet & Bolt: Split Worlds","conceptId":2,"device":["PS5"],"streamingSupported":true})";
  static inline const std::string listed_only_listed =
      R"({"productId":"EP-PPSA10002_00-LISTED","name":"Listed","conceptId":3,"device":["PS5"],"streamingSupported":true})";
  static inline const std::string bus_listed =
      R"({"productId":"EP0106-PPSA07037_00-FAKE0016TITLE000","name":"Coach Simulator 21 Next Stop","conceptId":90000014,"device":["PS5"],"streamingSupported":true})";

  static inline const std::string rivet_owned =
      R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000","productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","skuId":"EP0109-PPSA07028_00-FAKE0110TITLE000-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Rivet & Bolt: Split Worlds","packageType":"PSGD"},"streamingSupported":true})";
  static inline const std::string comet_owned =
      R"({"id":"EP-PPSA10001_00-GAME","productId":"EP-PPSA10001_00-GAME","skuId":"EP-PPSA10001_00-GAME-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Comet","packageType":"PSGD"},"streamingSupported":true})";
  static inline const std::string comet_add_on =
      R"({"id":"EP-PPSA10001_00-GAME","productId":"EP-PPSA10001_00-GAME","skuId":"EP-PPSA10001_00-GAME-E001","activeFlag":true,"entitlementType":5,"featureType":0,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Comet","packageType":"PSGD"},"streamingSupported":true})";
  static inline const std::string bus_claim =
      R"({"id":"EP0106-PPSA07037_00-FAKE0014TITLE000","productId":"EP0106-PPSA07037_00-FAKE0014TITLE000","skuId":"EP0106-PPSA07037_00-FAKE0014TITLE000-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Coach Simulator 21 Next Stop","packageType":"PSGD"},"streamingSupported":true})";
  static inline const std::string other_bus =
      R"({"id":"EP0106-PPSA07034_00-FAKE0015TITLE000","productId":"EP0106-PPSA07034_00-FAKE0015TITLE000","skuId":"EP0106-PPSA07034_00-FAKE0015TITLE000-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Coach Simulator 18","packageType":"PSGD"},"streamingSupported":true})";
  static inline const std::string dated_rivet_owned =
      R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000","productId":"EP0109-PPSA07028_00-FAKE0110TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,"activeDate":"2031-01-01T00:00:00.000Z","preorderCoreFlag":true,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":"Rivet & Bolt: Split Worlds","packageType":"PSGD"},"streamingSupported":true})";

  static inline const std::string streams_notice =
      R"(,"compatibilityNoticesByPlatform":{"PS5":[{"type":"STREAMING_SUPPORTED","targetPlatforms":["PS5"],"value":"Streaming supported"}]})";
  static inline const std::string no_stream_notice =
      R"(,"compatibilityNoticesByPlatform":{"PS5":[{"type":"OFFLINE_PLAY","targetPlatforms":["PS5"],"value":"Offline"}]},"descriptions":[{"type":"STREAMING_COMPATIBILITY_NOTICE","value":"Not offered for cloud streaming"}])";

  static inline const std::string comet_library_offer =
      R"(,"mobilectas":[{"type":"DOWNLOAD_PS_PLUS_GAME_CATALOG","action":{"type":"BACKGROUND_PURCHASE_AND_DOWNLOAD","param":[{"name":"skuId","values":["EP-PPSA10001_00-GAME-E008"]},{"name":"entitlementIds","values":["EP-PPSA10001_00-GAME","EP-PPSA10001_00-0000000000000000"]},{"name":"isEntitledViaSubscription","values":["false"]}]}}])";

  struct StoreConcept {
    std::string_view asked;
    std::string_view sold;
    std::string_view sold_facts{};
    std::string_view concept_facts{};
    std::string_view owned_titles{};

    auto reply() const -> std::string {
      return R"({"data":{"productRetrieve":{"id":")" + std::string{asked} +
             R"(","concept":{"id":"900002")" + std::string{concept_facts} +
             R"(,"ownedTitles":[)" + std::string{owned_titles} +
             R"(],"selectableProducts":{"purchasedTitles":[],"purchasableProducts":[{"id":")" +
             std::string{sold} + "\"" + std::string{sold_facts} + "}]}}}}}";
    }
  };

  static auto comma_joined(std::initializer_list<std::string_view> parts)
      -> std::string {
    std::string text;
    for (const auto part : parts) {
      if (!text.empty()) text += ',';
      text += part;
    }
    return text;
  }

  static auto store_list(std::initializer_list<std::string_view> games)
      -> std::string {
    return "[{\"games\":[" + comma_joined(games) + "]}]";
  }

  static auto owned_page(std::initializer_list<std::string_view> records)
      -> std::string {
    return R"({"revisionId":1,"start":0,"totalResults":)" +
           std::to_string(records.size()) + R"(,"entitlements":[)" +
           comma_joined(records) + "]}";
  }

  static auto owned_ps5(std::string_view id, std::string_view name)
      -> std::string {
    const std::string quoted{id};
    return R"({"id":")" + quoted + R"(","productId":")" + quoted +
           R"(","skuId":")" + quoted +
           R"(-E001","activeFlag":true,"entitlementType":5,"featureType":3,"entitlementAttributes":[{"platformId":"ps5"}],"gameMeta":{"name":")" +
           std::string{name} +
           R"(","packageType":"PSGD"},"streamingSupported":true})";
  }

  static auto store_product(std::string_view id, std::string_view facts)
      -> std::string {
    const std::string quoted = "\"" + std::string{id} + "\"";
    return R"({"data":{"productRetrieve":{"id":)" + quoted +
           R"(,"concept":{"id":"900002","ownedTitles":[],"selectableProducts":{"purchasedTitles":[],"purchasableProducts":[{"id":)" +
           quoted + std::string{facts} + "}]}}}}}";
  }

  static auto subscription_under(std::string_view label, std::string_view sku)
      -> std::string {
    return R"({"data":{"fetchSubscriptions":{"subscriptions":[{"active":true,"endDate":"2031-01-01T00:00:00.000Z",)"
           R"("id":")" +
           std::string{sku} +
           R"(","productName":"PlayStation Plus","subscriptionType":"PS_PLUS",)"
           R"("tierLabel":")" +
           std::string{label} + R"("}]}}})";
  }

  static auto subscription(std::string_view label) -> std::string {
    return subscription_under(label, "IP9101-NPIA90006_00-PLUS");
  }

  static auto stream_offer(const std::string& number, std::string_view label)
      -> std::string {
    return R"({"id":"EP0109-)" + number + "_00-" + std::string{label} +
           R"(","skus":[{"id":"EP0109-)" + number +
           R"(_00-SKU","contentCollections":[{"branding":"PS_PLUS_STREAM_PS5"}],"entitlements":[{"id":"EP0109-)" +
           number +
           R"(_00-GAME","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true}]}]})";
  }

  static auto tagged_grid(int count, std::string_view first_number)
      -> std::string {
    std::string products = stream_offer(std::string{first_number}, "TAGGED");
    for (int index = 1; index < count; ++index)
      products +=
          "," + stream_offer("PPSA" + std::to_string(30000 + index), "FILLER");
    return R"({"totalNumProducts":)" + std::to_string(count) +
           R"(,"products":[)" + products + "]}";
  }

  static auto premium_offer(std::string_view id, std::string_view name,
                            std::string_view entitlement = {},
                            std::string_view concept_id = "1") -> std::string {
    const auto granted = entitlement.empty() ? id : entitlement;
    return R"({"id":")" + std::string{id} + R"(","name":")" +
           std::string{name} + R"(","concept":{"id":")" +
           std::string{concept_id} + R"("},"skus":[{"id":")" + std::string{id} +
           R"(-E001","contentCollections":[{"branding":"PS_PLUS_STREAM_PS5"}],"entitlements":[{"id":")" +
           std::string{granted} +
           R"(","packageType":"PSGD","featureType":"FULL_GAME","streamingSupported":true}]}]})";
  }

  static auto premium_grid(std::initializer_list<std::string> offers)
      -> std::string {
    std::string products;
    std::size_t count = 0;
    for (const auto& offer : offers) {
      products += (count++ == 0 ? "" : ",") + offer;
    }
    for (int index = 0; index < 85; ++index, ++count)
      products +=
          (count == 0 ? "" : ",") +
          stream_offer("PPSA" + std::to_string(30000 + index), "FILLER");
    return R"({"totalNumProducts":)" + std::to_string(count) +
           R"(,"products":[)" + products + "]}";
  }

  static auto sale_offer(std::string_view id, std::string_view name,
                         std::string_view feature = "FULL_GAME",
                         std::string_view streams = "true") -> std::string {
    return R"({"id":")" + std::string{id} + R"(","name":")" +
           std::string{name} +
           R"(","concept":{"id":"7"},"media":{"images":[{"type":"MASTER","url":"https://image/)" +
           std::string{name} + R"(.png"}]},"skus":[{"id":")" + std::string{id} +
           R"(-E001","entitlements":[{"id":")" + std::string{id} +
           R"(","packageType":"PSGD","featureType":")" + std::string{feature} +
           R"(","streamingSupported":)" + std::string{streams} + "}]}]}";
  }
};

class RecordedFetch {
 public:
  RecordedFetch() = default;
  RecordedFetch(const RecordedFetch&) = delete;
  auto operator=(const RecordedFetch&) -> RecordedFetch& = delete;

  auto observer() const -> const catalog::FetchObserver& { return observer_; }

  auto states() const -> const std::vector<FetchState>& { return states_; }

  auto steps() const -> std::vector<FetchStep> {
    std::vector<FetchStep> seen;
    for (const auto& state : states_)
      if (seen.empty() || seen.back() != state.step())
        seen.push_back(state.step());
    return seen;
  }

  auto activities() const -> std::vector<FetchActivity> {
    std::vector<FetchActivity> seen;
    for (const auto& state : states_)
      if (state.activity() &&
          (seen.empty() || seen.back() != *state.activity()))
        seen.push_back(*state.activity());
    return seen;
  }

  auto did(FetchActivity activity) const -> bool {
    return std::ranges::contains(activities(), activity);
  }

  auto outcome_of(FetchStep step) const -> std::optional<StepOutcome> {
    if (states_.empty()) return std::nullopt;
    for (const auto& passed : states_.back().passed())
      if (passed.step == step) return passed.outcome;
    return std::nullopt;
  }

 private:
  std::vector<FetchState> states_;
  catalog::FetchObserver observer_ = [this](const FetchState& state) {
    states_.push_back(state);
  };
};

class CatalogApiTest : public ::testing::Test, public PsReplies {
 protected:
  using Games = libchiaking::Expected<std::shared_ptr<const catalog::MyGames>,
                                      catalog::GamesFailure>;

  static inline const std::vector<FetchStep> every_step{
      FetchStep::checking_sign_in,  FetchStep::cloud_list,
      FetchStep::owned_library,     FetchStep::premium_category,
      FetchStep::for_sale_category, FetchStep::streaming_access,
      FetchStep::matching,          FetchStep::trusted_time,
      FetchStep::exclusions,        FetchStep::ready,
  };

  static auto product(std::string_view id) -> catalog::ProductId {
    return Rig::product(id);
  }

  template <class Failure = catalog::GamesFailure>
  static auto unanswered() -> Failure {
    return Failure{Failure::Reason::cancelled};
  }

  template <class Work>
  auto started(Work work) -> libchiaking::platform::Thread {
    return libchiaking::platform::Thread::start(
               shared_threading, "catalog test",
               libchiaking::platform::ThreadRole::unspecified, std::move(work))
        .value();
  }

  void pause_a_moment() {
    libchiaking::platform::Sleeper{shared_threading, steady_clock}.sleep_for(
        std::chrono::milliseconds{100});
  }

  auto restarted(Rig& rig) -> Catalog {
    return libchiaking::testing::ServiceBuilder::build<Catalog>(
        rig.http, rig.records, shared_threading, rig.clock, rig.wall,
        rig.server, rig.budget);
  }

  static void store_sells(Rig& rig, std::initializer_list<std::string> offers) {
    std::string products;
    for (const auto& offer : offers)
      products += (products.empty() ? "" : ",") + offer;
    rig.http.on(
        "GET", std::string{store_grid},
        {{200, R"({"totalNumProducts":)" + std::to_string(offers.size()) +
                   R"(,"products":[)" + products + "]}"}});
  }

  static void premium_includes(Rig& rig,
                               std::initializer_list<std::string> offers) {
    rig.http.on("GET", std::string{tag_grid}, {{200, premium_grid(offers)}});
  }

  static void old_service_opens(Rig& rig, std::string_view country = "GB",
                                std::string_view language = "en") {
    rig.http.on("GET", "response_type=code",
                {{302, "",
                  test_support::catalog::ScriptedPs::location(
                      "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                      "grc-response.html?code=CODE1")}});
    rig.http.on("POST", "/user/session",
                {{200,
                  R"({"header":{"status_code":"0x0000"},"data":{"country":")" +
                      std::string{country} + R"(","language":")" +
                      std::string{language} + R"("}})",
                  {{"Set-Cookie", "JSESSIONID=JS1; Path=/"}}}});
  }

  static void old_service_lists(
      Rig& rig,
      std::string_view shelf =
          R"({"links":[{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"}]})") {
    old_service_opens(rig);
    rig.http.on(
        "GET", std::string{old_service_stores},
        {{200,
          R"({"header":{"status_code":"0x0000"},"data":{"base_url":"https://psnow.playstation.com/store/container/GB/en/19/ROOT"}})"}});
    rig.http.on(
        "GET", "ROOT?size=100",
        {{200,
          R"({"links":[{"name":"A - B","url":"https://psnow.playstation.com/store/cat/AB"}]})"}});
    rig.http.on("GET", "cat/AB?start=0&size=500", {{200, std::string{shelf}}});
  }

  static void ps5_store_lists(Rig& rig, std::string all_ps5,
                              std::string plus_games = "[]") {
    rig.http.on("GET", std::string{all_ps5_list}, {{200, std::move(all_ps5)}});
    rig.http.on("GET", "categoryList=plus-games-list",
                {{200, std::move(plus_games)}});
    rig.http.on("GET", "imagic/gameslist", {{200, "[]"}});
    rig.http.on("GET", std::string{ps5_owned_list}, {{200, owned_page({})}});
  }

  static void ps5_records_answer_for_themselves(Rig& rig,
                                                std::string_view page) {
    constexpr std::string_view opening = R"({"id":")";
    constexpr std::string_view closing = R"("streamingSupported":true})";
    for (auto at = page.find(opening); at != std::string_view::npos;
         at = page.find(opening, at + 1)) {
      const auto start = at + opening.size();
      const auto id = page.substr(start, page.find('"', start) - start);
      if (!page.substr(start + id.size()).starts_with(R"(","productId")"))
        continue;
      const auto end = page.find(closing, start) + closing.size();
      rig.http.before_everything(
          "GET", std::string{ps5_owned_lookup} + std::string{id} + "?",
          {{200, std::string{page.substr(at, end - at)}}});
    }
  }

  static void library_holds(Rig& rig, std::string page) {
    rig.http.on(
        "GET", "response_type=token",
        {{302, "",
          test_support::catalog::ScriptedPs::location(
              "https://x/"
              "grc-response.html#access_token=TOKEN1&expires_in=3600")}});
    rig.http.on("GET", std::string{owned_lookup}, {{404, "{}"}});
    rig.http.on("GET", std::string{ps5_owned_lookup}, {{404, "{}"}});
    rig.http.on("GET", std::string{owned_list}, {{200, page}});
    ps5_records_answer_for_themselves(rig, page);
    rig.http.on("GET", std::string{ps5_owned_list}, {{200, std::move(page)}});
  }

  static void library_pages(Rig& rig, std::vector<Reply> replies) {
    rig.http.on("GET", std::string{owned_list}, replies);
    rig.http.on("GET", std::string{ps5_owned_list}, std::move(replies));
  }

  static void ps_as_usual(Rig& rig) {
    old_service_lists(rig);
    ps5_store_lists(rig, store_list({comet_listed, rivet_listed}),
                    store_list({comet_listed}));
    library_holds(rig, owned_page({rivet_owned}));
    premium_includes(rig, {premium_offer(comet, "Comet")});
  }

  static void membership_is(Rig& rig, std::string_view label) {
    rig.http.on("GET", std::string{subscriptions},
                {{200, subscription(label)}});
  }

  static void cloud_streaming_allowed(Rig& rig) {
    rig.http.on(
        "GET", std::string{cloud_benefits},
        {{200,
          R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1","tierLabel":"TIER_30","benefitPlanIds":["PLAN-PREMIUM"]}]})"}});
  }

  static void cloud_streaming_denied(Rig& rig, std::string_view plus_tier) {
    rig.http.on("GET", std::string{cloud_benefits},
                {{200, R"({"authorization":false,"cloudgameOwnerInfos":[]})"}});
    rig.http.on(
        "GET", std::string{plus_services},
        {{200,
          R"([{"type":"PSPLUS","state":{"active":true,"status":"SUBSCRIBED"},"currentTier":{"tierLabel":")" +
              std::string{plus_tier} + R"("}}])"}});
  }

  static void storefront_says(Rig& rig, std::string_view id,
                              std::string_view facts) {
    rig.http.on("GET", std::string{store_app},
                {{200, store_product(id, facts)}});
  }

  static auto loaded(Catalog& service, users::PsUser& user)
      -> std::shared_ptr<const catalog::MyGames> {
    auto games = service.process_library_and_list(user);
    EXPECT_TRUE(games) << "the games did not load";
    return std::move(games).value();
  }

  static auto loaded(Rig& rig, users::PsUser& user)
      -> std::shared_ptr<const catalog::MyGames> {
    return loaded(rig.catalog, user);
  }

  static auto game_of(const catalog::MyGames& games, std::string_view id)
      -> const catalog::MyGame& {
    const auto* game = games.find(product(id));
    EXPECT_NE(game, nullptr) << "no game " << id;
    if (game == nullptr) throw std::out_of_range{std::string{id}};
    return *game;
  }

  static auto is_filler(const catalog::MyGame& game) -> bool {
    return game.product_id().text().ends_with("_00-FILLER");
  }

  static auto shown(const catalog::MyGames& games) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        games.visible(),
        [](const catalog::MyGame& game) { return !is_filler(game); }));
  }

  static auto held_back(const catalog::MyGames& games) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        games.hidden(),
        [](const catalog::MyGame& game) { return !is_filler(game); }));
  }

  static auto refusal_kind(const catalog::LaunchVerdict& verdict)
      -> std::optional<LaunchRefusal::Kind> {
    if (!verdict.refusal()) return std::nullopt;
    return verdict.refusal()->kind();
  }

  libchiaking::platform::DefaultThreadingProvider& shared_threading{
      test_support::process_threading()};
  libchiaking::platform::SteadyClock steady_clock;
};

}  // namespace catalog_api_test
