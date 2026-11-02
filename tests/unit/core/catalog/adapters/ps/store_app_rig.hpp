#pragma once

#include "src/core/catalog/adapters/ps/store_app_title.hpp"
#include "src/core/psn/graphql/authenticated/client.hpp"
#include "src/core/psn/graphql/authenticated/title_details.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace catalog_test {

class StoreAppRig {
 public:
  using Answer = libchiaking::Expected<libchiaking::catalog::StorefrontAnswer,
                                       libchiaking::catalog::Unanswered>;
  using TitleDetails = libchiaking::psn::graphql::TitleDetails;
  using TitleProduct = libchiaking::psn::graphql::TitleProduct;
  using Reply = PsRig::Reply;

  static constexpr std::string_view remaster =
      "EP0109-PPSA17001_00-FAKE0056TITLE000";
  static constexpr std::string_view ps4_edition =
      "EP0109-CUSA17003_00-FAKE0059TITLE000";
  static constexpr std::string_view standard =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";
  static constexpr std::string_view by_product =
      "operationName=metGetConceptByProductIdQuery";
  static constexpr std::string_view by_concept =
      "operationName=metGetConceptById";
  static constexpr std::string_view pricing =
      "operationName=metGetPricingDataByConceptId";
  static constexpr std::string_view token_exchange = "/oauth/token";

  static auto enveloped(std::string_view data) -> std::string {
    return R"({"data":)" + std::string{data} + "}";
  }

  static auto fixture(std::string_view name) -> std::string {
    return test_support::psn::Fixtures::text(name);
  }

  static auto skyline_concept() -> libchiaking::catalog::ConceptId {
    auto parsed = libchiaking::catalog::ConceptId::parse("900002");
    EXPECT_TRUE(parsed);
    return std::move(parsed).value();
  }

  static auto concept_of(std::string body) -> std::optional<TitleDetails> {
    StoreAppRig store;
    store.replies(by_concept, std::move(body));
    return store.concept_reply();
  }

  static auto priced_of(TitleDetails details, std::string body)
      -> std::optional<TitleDetails> {
    StoreAppRig store;
    store.replies(pricing, std::move(body));
    return store.priced_reply(std::move(details));
  }

  static auto skyline() -> TitleDetails {
    auto details = concept_of(fixture("concept_900002.json"));
    EXPECT_TRUE(details.has_value());
    auto priced =
        priced_of(std::move(details).value(), fixture("pricing_900002.json"));
    EXPECT_TRUE(priced.has_value());
    return std::move(priced).value();
  }

  static auto edition(const TitleDetails& details, std::string_view id)
      -> const TitleProduct& {
    const auto found =
        std::ranges::find(details.products(), id, &TitleProduct::id);
    EXPECT_NE(found, details.products().end()) << id;
    if (found == details.products().end())
      throw std::out_of_range{std::string{id}};
    return *found;
  }

  void replies(std::string_view operation, std::string body) {
    rig.http.on("GET", std::string{operation}, {Reply{200, std::move(body)}});
  }

  void sells_skyline(std::optional<std::string> priced = {}) {
    replies(by_concept, fixture("concept_900002.json"));
    replies(pricing,
            priced ? std::move(*priced) : fixture("pricing_900002.json"));
  }

  void holds_a_refresh_token() {
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{8}, "player",
        libchiaking::psn::auth::Npsso{"NPSSO"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app"},
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{PsRig::wall_start_ms + 3'600'000}});
    saved.device = libchiaking::users::DeviceId::parse(PsRig::device);
    saved.store_refresh.emplace(
        libchiaking::psn::auth::AppRefreshToken{"refresh"});
    user = rig.signed_in.as(std::move(saved));
  }

  void ps_renews(std::string_view token) {
    rig.http.on(
        "POST", std::string{token_exchange},
        {Reply{200, R"({"access_token":")" + std::string{token} +
                        R"(","refresh_token":"next","expires_in":3600})"}});
  }

  void ps_issues_a_token_for_the_seed(std::string_view token) {
    ps_renews(token);
  }

  void later(std::chrono::seconds by) {
    rig.wall.advance_ms(std::chrono::milliseconds{by}.count());
    rig.clock.advance_ms(std::chrono::milliseconds{by}.count());
  }

  auto requests() const -> std::size_t { return rig.http.seen.size(); }

  auto renewals() const -> int { return rig.http.count(token_exchange); }

  auto last_bearer() const -> std::string {
    EXPECT_FALSE(rig.http.seen.empty());
    if (rig.http.seen.empty()) return {};
    for (const auto& header : rig.http.seen.back().headers)
      if (header.name == "Authorization") return header.value;
    return {};
  }

  auto asked(std::string_view id) -> Answer {
    return libchiaking::catalog::ps::StoreAppTitle{*user, rig.client, rig.wall}
        .answer_for(PsRig::product(id));
  }

  auto asked_of_concept(std::string_view id) -> Answer {
    return libchiaking::catalog::ps::StoreAppTitle{*user, rig.client, rig.wall}
        .answer_for(PsRig::product(id), skyline_concept());
  }

  auto concept_reply() -> std::optional<TitleDetails> {
    auto store = libchiaking::users::CatalogCalls{*user}.store_app(rig.client);
    EXPECT_TRUE(store);
    auto details = store.value().concept_by_id("900002", {});
    return details ? std::optional<TitleDetails>{std::move(details.value())}
                   : std::nullopt;
  }

  auto priced_reply(TitleDetails details) -> std::optional<TitleDetails> {
    auto store = libchiaking::users::CatalogCalls{*user}.store_app(rig.client);
    EXPECT_TRUE(store);
    auto priced = store.value().concept_pricing(std::move(details), {});
    return priced ? std::optional<TitleDetails>{std::move(priced.value())}
                  : std::nullopt;
  }

  PsRig rig;
  std::shared_ptr<libchiaking::users::PsUser> user{rig.user(7)};
};

}  // namespace catalog_test
