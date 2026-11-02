#include "src/core/catalog/adapters/ps/cloud_game_resolver.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <libchiaking/detail/users/account_context.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace ps = libchiaking::catalog::ps;
namespace users = libchiaking::users;
using catalog::OwnershipId;
using catalog::TitleId;
using catalog::Unanswered;
using catalog::cloud_stream::CloudGameListing;
using Unasked = CloudGameListing::Unasked;

class CloudGameResolverQuestion : public ::testing::Test {
 protected:
  static constexpr std::string_view resolver =
      "api/catalog/v2/products/cloudgame";
  static constexpr std::string_view astro_bot =
      "EP0109-PPSA27019_00-ASTROBOTPS50000";
  static constexpr std::string_view listed_reply = R"({"productInfo":[
  {"productId":"EP0109-PPSA27019_00-ASTROBOTPS50000","titles":[
    {"titleId":"PPSA07055_00","entitlementId":"EP0109-PPSA07055_00-FAKE0086TITLE000","nativePlatform":"PS5","isDownloadable":true},
    {"titleId":"PPSA27019_00","entitlementId":"EP0109-PPSA27019_00-ASTROBOTPS50000","nativePlatform":"PS5","isDownloadable":false,"gameIntent":"PLAY"},
    {"titleId":"PPSA27019_01","entitlementId":"EP0109-PPSA27019_01-LATER","nativePlatform":"PS5"}]},
  {"productId":"EP0109-PPSA77003_00-SECOND","titles":[]}]})";

  static auto title(std::string_view text) -> TitleId {
    auto parsed = TitleId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto entitlement(std::string_view text) -> OwnershipId {
    auto parsed = OwnershipId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto plans(std::initializer_list<std::string_view> texts)
      -> std::vector<users::BenefitPlanId> {
    std::vector<users::BenefitPlanId> held;
    for (const auto text : texts) {
      auto plan = users::BenefitPlanId::parse(text);
      EXPECT_TRUE(plan) << text;
      held.push_back(std::move(plan).value());
    }
    return held;
  }

  auto signed_in(users::AccountContext::Stated stated = {.legal_country = "GB",
                                                         .locale = "en-GB",
                                                         .age = 34},
                 std::uint64_t account = 1) -> std::shared_ptr<users::PsUser> {
    users::SavedSignIn saved{libchiaking::AccountId{account}, "player",
                             libchiaking::psn::auth::Npsso{"NPSSO"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app"},
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{PsRig::wall_start_ms + 3'600'000}});
    saved.device = users::DeviceId::parse(PsRig::device);
    saved.context = users::AccountContext::stating(stated);
    return rig.signed_in.as(std::move(saved));
  }

  PsRig rig;
  const std::vector<users::BenefitPlanId> two_plans =
      plans({"SUB001-SUBC00001_00-CMS9ADD4PT9IXKZ6",
             "SUB001-SUBC00001_00-V1S3OGDR05QFMFPP"});
};

TEST_F(CloudGameResolverQuestion, IsBuiltExactly) {
  rig.http.on("GET", std::string{resolver}, {{200, R"({"productInfo":[]})"}});
  const auto user = signed_in();
  const auto answer = ps::CloudGameResolver{*user, rig.client}.for_product(
      PsRig::product(astro_bot), two_plans);
  EXPECT_TRUE(answer.is_not_listed());
  EXPECT_EQ(answer.http_status(), 200);
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& seen = rig.http.seen.front();
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url,
            "https://m.np.playstation.com/api/catalog/v2/products/cloudgame"
            "?productId=EP0109-PPSA27019_00-ASTROBOTPS50000&country=GB&"
            "language=en&age=99&limit=1000"
            "&benefitPlanIds=SUB001-SUBC00001_00-CMS9ADD4PT9IXKZ6,SUB001-"
            "SUBC00001_00-V1S3OGDR05QFMFPP"
            "&homeshare=false&storeFront=PS5");
  EXPECT_TRUE(std::ranges::any_of(seen.headers, [](const auto& header) {
    return header.name == "Authorization" && header.value == "Bearer app";
  }));
}

TEST_F(CloudGameResolverQuestion, KeepsTheTitlesOfAListedProduct) {
  rig.http.on("GET", std::string{resolver}, {{200, std::string{listed_reply}}});
  const auto user = signed_in();
  const auto answer = ps::CloudGameResolver{*user, rig.client}.for_product(
      PsRig::product(astro_bot), two_plans);
  EXPECT_TRUE(answer.is_listed());
  EXPECT_EQ(answer.http_status(), 200);
  EXPECT_FALSE(answer.cause());
  const auto product = answer.product();
  ASSERT_TRUE(product);
  ASSERT_TRUE(product->id);
  EXPECT_EQ(product->id->text(), astro_bot);
  ASSERT_TRUE(product->titles);
  ASSERT_EQ(product->titles->size(), 3U);
  EXPECT_FALSE(answer.unanswered());
  const auto& ours = (*product->titles)[1];
  EXPECT_EQ(ours.title_id, title("PPSA27019_00"));
  EXPECT_EQ(ours.entitlement, entitlement(astro_bot));
  EXPECT_EQ(ours.native_platform, "PS5");
  EXPECT_EQ(ours.downloadable, false);
  EXPECT_EQ(ours.game_intent, "PLAY");
  EXPECT_FALSE((*product->titles)[2].downloadable);
  EXPECT_FALSE((*product->titles)[2].game_intent);
}

TEST_F(CloudGameResolverQuestion, ListsAProductWithOrWithoutTitles) {
  rig.http.on(
      "GET", std::string{resolver},
      {{200, std::string{listed_reply}},
       {200,
        R"({"productInfo":[{"productId":"EP0109-PPSA27019_00-ASTROBOTPS50000","titles":[]}]})"},
       {200,
        R"({"productInfo":[{"productId":"EP0109-PPSA27019_00-ASTROBOTPS50000"}]})"}});
  const auto user = signed_in();
  ps::CloudGameResolver resolve{*user, rig.client};
  const auto other = PsRig::product(astro_bot);
  const auto listed = resolve.for_product(other, two_plans);
  EXPECT_TRUE(listed.is_listed());
  EXPECT_FALSE(listed.unanswered());
  const auto empty = resolve.for_product(other, two_plans);
  EXPECT_TRUE(empty.is_listed());
  ASSERT_TRUE(empty.product());
  ASSERT_TRUE(empty.product()->titles);
  EXPECT_TRUE(empty.product()->titles->empty());
  EXPECT_FALSE(empty.unanswered());
  const auto untitled = resolve.for_product(other, two_plans);
  EXPECT_TRUE(untitled.is_listed());
  ASSERT_TRUE(untitled.product());
  EXPECT_FALSE(untitled.product()->titles);
  EXPECT_FALSE(untitled.unanswered());
}

TEST_F(CloudGameResolverQuestion, CallsAnEmptyProductInfoNotListed) {
  rig.http.on("GET", std::string{resolver},
              {{200, R"({"productInfo":[]})"},
               {200, R"({})"},
               {200, R"({"productInfo":null})"}});
  const auto user = signed_in();
  ps::CloudGameResolver resolve{*user, rig.client};
  for (int round = 0; round < 3; ++round) {
    const auto answer =
        resolve.for_product(PsRig::product(astro_bot), two_plans);
    EXPECT_TRUE(answer.is_not_listed()) << round;
    EXPECT_FALSE(answer.is_listed()) << round;
    EXPECT_FALSE(answer.product()) << round;
    EXPECT_FALSE(answer.unanswered()) << round;
    EXPECT_EQ(answer.http_status(), 200) << round;
  }
}

TEST_F(CloudGameResolverQuestion, FailsAFailedRequestWithItsStatus) {
  rig.http.on("GET", std::string{resolver},
              {{500, "busy"},
               {403, R"({"error":"denied"})"},
               {200, R"({"productInfo":{}})"},
               {200, "not json"},
               {0, "", {}, true}});
  const auto user = signed_in();
  ps::CloudGameResolver resolve{*user, rig.client};
  const auto ask = [&] {
    return resolve.for_product(PsRig::product(astro_bot), two_plans);
  };
  const auto busy = ask();
  EXPECT_FALSE(busy.is_listed());
  EXPECT_FALSE(busy.is_not_listed());
  ASSERT_TRUE(busy.cause());
  EXPECT_EQ(busy.cause()->http_status(), 500);
  EXPECT_EQ(busy.http_status(), 500);
  EXPECT_EQ(busy.cause()->reason(), Unanswered::Reason::user);
  const auto denied = ask();
  ASSERT_TRUE(denied.cause());
  EXPECT_EQ(denied.cause()->http_status(), 403);
  const auto shapeless = ask();
  ASSERT_TRUE(shapeless.cause());
  ASSERT_TRUE(shapeless.cause()->cause());
  EXPECT_EQ(shapeless.cause()->cause()->reason,
            users::Unanswered::Reason::malformed);
  EXPECT_TRUE(ask().cause());
  const auto dropped = ask();
  EXPECT_TRUE(dropped.cause());
  EXPECT_FALSE(dropped.unasked());
}

TEST_F(CloudGameResolverQuestion, IsNotAskedWithoutPlanIds) {
  rig.http.on("GET", std::string{resolver}, {{200, std::string{listed_reply}}});
  const auto user = signed_in();
  const auto planless = ps::CloudGameResolver{*user, rig.client}.for_product(
      PsRig::product(astro_bot), {});
  EXPECT_EQ(planless.unasked(), Unasked::no_plan_ids);
  EXPECT_FALSE(planless.cause());
  EXPECT_FALSE(planless.http_status());
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(CloudGameResolverQuestion, IsNotAskedWithoutAnAccountLocale) {
  rig.http.on("GET", std::string{resolver}, {{200, std::string{listed_reply}}});
  const auto comet = PsRig::product(astro_bot);
  const auto nowhere = signed_in({.age = 34}, 2);
  ps::CloudGameResolver localeless{*nowhere, rig.client};
  EXPECT_EQ(localeless.for_product(comet, two_plans).unasked(),
            Unasked::no_account_locale);
  const auto countryless = signed_in({.locale = "en-GB", .age = 34}, 3);
  ps::CloudGameResolver stateless{*countryless, rig.client};
  EXPECT_EQ(stateless.for_product(comet, two_plans).unasked(),
            Unasked::no_account_locale);
  EXPECT_TRUE(rig.http.seen.empty());
}

}  // namespace
}  // namespace catalog_test
