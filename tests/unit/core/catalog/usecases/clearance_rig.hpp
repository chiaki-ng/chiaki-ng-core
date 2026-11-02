#pragma once

#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/category_record.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/adapters/cache/library_record.hpp"
#include "src/core/catalog/adapters/cache/streamable_list_record.hpp"
#include "src/core/catalog/usecases/launch_checks.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/users/adapters/account_claims.hpp"
#include "tests/support/catalog/scripted_ps.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace catalog_test {

class ClearanceRig {
 public:
  using View = libchiaking::catalog::usecases::ListGames::View;
  using Reply = test_support::catalog::ScriptedPs::Reply;

  static constexpr std::string_view subscriptions =
      "operationName=getSubscriptions";
  static constexpr std::string_view store_app =
      "operationName=metGetConceptByProductIdQuery";
  static constexpr std::string_view owned_lookup = "internal_entitlements/";
  static constexpr std::string_view owned_list = "entitlement_type=5";
  static constexpr std::string_view library_token = "response_type=token";
  static constexpr std::string_view ps5_copies =
      "internal/entitlements?isActive=true&gameMetaPackageType=PSGD";
  static constexpr std::string_view ps5_record = "internal/entitlements/";
  static constexpr std::string_view cloud_benefits =
      "premium/features/cloudgame";
  static constexpr std::string_view plus_services = "services/pssubscriptions";
  static constexpr std::string_view feature_config =
      "portal/np/portal-config.json";
  static constexpr std::string_view resolver =
      "api/catalog/v2/products/cloudgame";
  static constexpr std::uint64_t now_us = 3'000'000;
  static constexpr int audience_age = 34;
  static constexpr std::string_view audience_claims =
      R"({"age":34,"legal_country":"GB","locale":"en-GB","is_child":false})";
  static constexpr std::string_view device =
      "000000070041008012345678901234567890123456789012";

  ClearanceRig() {
    wall.now_us = now_us;
    clock.now_us = now_us;
  }

  static auto ps5_game(std::string_view id, std::string_view name,
                       std::string_view streaming = "true") -> std::string {
    return R"({"id":")" + std::string{id} + R"(","productId":")" +
           std::string{id} +
           R"(","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"name":")" +
           std::string{name} +
           R"(","packageType":"PSGD"},"streamingSupported":)" +
           std::string{streaming} + "}";
  }

  static auto ps5_game_titled(std::string_view id, std::string_view title,
                              std::string_view extra = {}) -> std::string {
    return R"({"id":")" + std::string{id} + R"(","productId":")" +
           std::string{id} +
           R"(","activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"name":"Owned","packageType":"PSGD"},"titleMeta":{"titleId":")" +
           std::string{title} + R"("},"streamingSupported":true)" +
           std::string{extra} + "}";
  }

  static auto at(std::string_view time)
      -> libchiaking::catalog::ReleaseDate::Time {
    auto parsed = libchiaking::catalog::ReleaseDate::parse_time(time);
    EXPECT_TRUE(parsed) << time;
    return std::move(parsed).value();
  }

  static auto kind_of(const libchiaking::catalog::LaunchVerdict& verdict)
      -> libchiaking::catalog::LaunchRefusal::Kind {
    EXPECT_TRUE(verdict.refusal());
    if (!verdict.refusal()) throw std::logic_error{"the verdict is a launch"};
    return verdict.refusal()->kind();
  }

  auto user(bool states_its_audience = false)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    libchiaking::users::SavedSignIn saved{
        RuleCases::account, "player", libchiaking::psn::auth::Npsso{"NPSSO"}};
    if (states_its_audience)
      saved.context = libchiaking::users::AccountClaims::read(
          test_support::catalog::CatalogRig::token_carrying(audience_claims));
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app"},
        std::chrono::system_clock::time_point{std::chrono::hours{72}});
    saved.device = libchiaking::users::DeviceId::parse(device);
    EXPECT_TRUE(saved.device);
    saved.region = libchiaking::users::Region{"GB", "en"};
    return signed_in.as(std::move(saved));
  }

  auto holding(libchiaking::users::PsUser& user,
               const libchiaking::catalog::StreamableList& listed,
               const libchiaking::catalog::Library& library) -> View {
    const auto record =
        libchiaking::catalog::cache::StreamableListRecord::of(listed);
    EXPECT_TRUE(record);
    libchiaking::catalog::cache::Cache seeded{records, wall};
    EXPECT_TRUE(seeded.write(record.value()));
    EXPECT_TRUE(
        seeded.write(libchiaking::catalog::cache::LibraryRecord{library}));
    auto seen = list.held_view(user);
    EXPECT_TRUE(seen);
    return std::move(seen).value();
  }

  auto holding(
      libchiaking::users::PsUser& user,
      const libchiaking::catalog::StreamableList& listed,
      const libchiaking::catalog::OwnedPs5Games& owned,
      const libchiaking::catalog::cloud_stream::PremiumCategory& category)
      -> View {
    const auto record =
        libchiaking::catalog::cache::StreamableListRecord::of(listed);
    EXPECT_TRUE(record);
    const auto library =
        RuleCases::library_of({}).with_ps5_part(owned.games(), owned.state());
    libchiaking::catalog::cache::Cache seeded{records, wall};
    EXPECT_TRUE(seeded.write(record.value()));
    EXPECT_TRUE(seeded.write(
        libchiaking::catalog::cache::LibraryRecord{library, owned}));
    const auto now = libchiaking::catalog::cache::PremiumCategoryRecord::Time{
        std::chrono::milliseconds{
            static_cast<std::int64_t>(wall.now_us / 1000U)}};
    EXPECT_TRUE(seeded.write(
        libchiaking::catalog::cache::PremiumCategoryRecord::nothing_held(
            category.locale())
            .for_age(audience_age)
            .with_answer(category, now)));
    auto seen = list.held_view(user);
    EXPECT_TRUE(seen);
    return std::move(seen).value();
  }

  void membership_is(std::string_view label) {
    http.on(
        "GET", std::string{subscriptions},
        {{200,
          R"({"data":{"fetchSubscriptions":{"subscriptions":[{"active":true,"endDate":"2031-01-01T00:00:00.000Z",)"
          R"("id":"IP9101-NPIA90006_00-PLUS","productName":"PlayStation Plus","subscriptionType":"PS_PLUS",)"
          R"("tierLabel":")" +
              std::string{label} + R"("}]}}})"}});
  }

  void cloud_streaming_allowed() {
    http.on(
        "GET", std::string{cloud_benefits},
        {{200,
          R"({"authorization":true,"cloudgameOwnerInfos":[{"ownerAccountId":"1","tierLabel":"TIER_30","benefitPlanIds":["PLAN-PREMIUM"]}]})"}});
  }

  void cloud_streaming_denied(std::string_view plus_tier) {
    http.on("GET", std::string{cloud_benefits},
            {{200, R"({"authorization":false,"cloudgameOwnerInfos":[]})"}});
    http.on(
        "GET", std::string{plus_services},
        {{200,
          R"([{"type":"PSPLUS","state":{"active":true,"status":"SUBSCRIBED"},"currentTier":{"tierLabel":")" +
              std::string{plus_tier} + R"("}}])"}});
  }

  void cloud_streaming_unreadable(int status) {
    http.on("GET", std::string{cloud_benefits}, {{status, ""}});
  }

  void library_answers(int lookup_status, std::string lookup,
                       std::string whole = R"({"entitlements":[]})") {
    http.on("GET", std::string{library_token},
            {{302, "",
              test_support::catalog::ScriptedPs::location(
                  "https://x/"
                  "grc-response.html#access_token=TOKEN1&expires_in=3600")}});
    http.on("GET", std::string{owned_lookup},
            {{lookup_status, std::move(lookup)}});
    http.on("GET", std::string{owned_list}, {{200, std::move(whole)}});
  }

  void nothing_excluded() { excluding(""); }

  void excluding(std::string_view titles) {
    http.on("GET", std::string{feature_config},
            {{200, R"({"excludedTitleIds":[)" + std::string{titles} + "]}"}});
  }

  void resolver_lists_nothing() {
    resolver_answers(200, R"({"productInfo":[]})");
  }

  void resolver_answers(int status, std::string body) {
    http.on("GET", std::string{resolver}, {{status, std::move(body)}});
  }

  void ps5_record_answers(int status, std::string body) {
    http.on("GET", std::string{ps5_record}, {{status, std::move(body)}});
  }

  void ps5_library_holds(std::string entitlements = {}, std::size_t total = 0) {
    http.on("GET", std::string{ps5_copies},
            {{200, R"({"revisionId":1,"start":0,"totalResults":)" +
                       std::to_string(total) + R"(,"entitlements":[)" +
                       entitlements + "]}"}});
  }

  void copy_streams(std::string_view id, bool streams) {
    ps5_record_answers(200, ps5_game(id, "Owned", streams ? "true" : "false"));
  }

  void storefront_says(std::string_view id, std::string_view facts) {
    const std::string quoted = "\"" + std::string{id} + "\"";
    http.on(
        "GET", std::string{store_app},
        {{200,
          R"({"data":{"productRetrieve":{"id":)" + quoted +
              R"(,"concept":{"id":"900002","ownedTitles":[],"selectableProducts":{"purchasedTitles":[],"purchasableProducts":[{"id":)" +
              quoted + std::string{facts} + "}]}}}}}"}});
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  test_support::FakeClock clock{0};
  test_support::FakeClock wall{0};
  test_support::catalog::ScriptedPs http;
  test_support::catalog::ServerTime server;
  libchiaking::testing::InMemoryRecordStore records{threading};
  test_support::users::SignedIn signed_in{http, clock, wall};
  libchiaking::catalog::HeldAnswers held{records, threading, clock, wall};
  libchiaking::catalog::usecases::ListGames list{http,    clock,   wall,
                                                 &server, nullptr, held};
  libchiaking::catalog::usecases::LaunchChecks checks{http, clock, &server,
                                                      nullptr, list};
};

}  // namespace catalog_test
