#include "src/core/catalog/adapters/ps/ownership_records.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
using catalog::HeldAs;
using catalog::OwnedRecord;
using catalog::OwnershipId;
using catalog::ServiceGeneration;
using catalog::Unanswered;

class OwnershipRecords : public ::testing::Test {
 protected:
  static constexpr std::string_view bus_claim_id =
      "EP0106-PPSA07037_00-FAKE0014TITLE000";
  static constexpr std::string_view bus_claim =
      R"({"id":"EP0106-PPSA07037_00-FAKE0014TITLE000","product_id":"EP0106-PPSA07037_00-FAKE0014TITLE000","active_flag":true,"feature_type":3,"active_date":"2026-10-02T09:14:00.000Z","entitlement_attributes":[{"platform_id":"ps5"}],"game_meta":{"name":"Coach Simulator 21 Next Stop","package_type":"PS5GD","type":"PS5_GAME"}})";
  static constexpr std::string_view other_owned =
      R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000","product_id":"EP0109-PPSA07028_00-FAKE0110TITLE000","active_flag":true,"feature_type":3,"entitlement_attributes":[{"platform_id":"ps5"}],"game_meta":{"name":"Rivet & Bolt: Split Worlds","package_type":"PS5GD","type":"PS5_GAME"}})";
  static constexpr std::string_view title_entitlement =
      R"({"id":"EP-PPSA10001_00-GAME","product_id":"EP-PPSA10001_00-GAME","active_flag":true,"feature_type":3,"entitlement_attributes":[{"platform_id":"ps5"}],"game_meta":{"name":"Comet","package_type":"PS5GD"}})";

  static auto owned_page(std::initializer_list<std::string_view> entries)
      -> std::string {
    std::string page = R"({"entitlements":[)";
    bool first = true;
    for (const auto entry : entries) {
      if (!first) page += ",";
      page += entry;
      first = false;
    }
    return page + "]}";
  }

  static auto full_owned_page() -> std::string {
    std::string full = R"({"entitlements":[)";
    for (int index = 0;
         index < libchiaking::psn::rest::commerce_entitlements_page_size;
         ++index)
      full += std::string{index ? "," : ""} + R"({"id":"E)" +
              std::to_string(index) + R"(","product_id":"P"})";
    return full + "]}";
  }

  static auto id(std::string_view text) -> OwnershipId {
    auto parsed = OwnershipId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static void authorized(PsRig& rig, std::string_view lifetime = "1") {
    rig.http.on(
        "GET", "response_type=token",
        {{302, "",
          PsRig::location("https://x/grc-response.html#access_token=TOKEN1&"
                          "expires_in=" +
                          std::string{lifetime})}});
  }

  static void quickly_authorized(PsRig& rig) {
    rig.http.on("GET", "response_type=token",
                {{302, "", PsRig::location("https://x/#access_token=T")}});
  }

  static auto records(PsRig& rig, users::PsUser& user) {
    return ps::OwnershipRecords{user, rig.client, rig.wall, 1024 * 1024}.all();
  }

  static auto one(PsRig& rig, users::PsUser& user,
                  std::initializer_list<std::string_view> ids) {
    std::vector<OwnershipId> asked;
    for (const auto text : ids) asked.push_back(id(text));
    return ps::OwnershipRecords{user, rig.client, rig.wall, 1024 * 1024}.one_of(
        asked);
  }

  PsRig rig;
};

class OwnershipRecordsOfOnePage : public OwnershipRecords {
 protected:
  void SetUp() override {
    authorized(rig);
    rig.http.on(
        "GET", "internal_entitlements",
        {{200,
          owned_page(
              {bus_claim, other_owned,
               R"({"id":"EP1-CUSA07004_00-ADDON","product_id":"EP1-CUSA07004_00-BUNDLE","active_flag":false,"feature_type":0,"sku_type":"TRIAL","preorder_flag":true,"name":"Loose name","entitlement_attributes":[{"platform_id":"web"},{"platform_id":"PS4"}],"images":[{"type":12,"url":"https://image.example/row.png"}]})",
               R"({"id":"EP1-CUSA07005_00-ICON","product_id":"EP1-CUSA07004_00-BUNDLE","active_flag":true,"feature_type":5,"active_date":17,"game_meta":{"name":"Icon","sku_type":"FULL","icon_url":"https://image.example/icon.png","images":[{"type":10,"url":"https://image.example/meta.png"}]}})",
               R"({"id":"EP1-CUSA07006_00-META","product_id":"EP1-CUSA07006_00-META","active_flag":true,"feature_type":1,"active_date":"","game_meta":{"name":"Meta","images":[{"type":10,"url":"https://image.example/meta.png"}]}})",
               "7"})}});
    auto listed = records(rig, *user);
    ASSERT_TRUE(listed);
    fetched_at = listed.value().fetched_at;
    all = std::move(listed.value().records);
    ASSERT_EQ(all.size(), 5U);
  }

  std::shared_ptr<users::PsUser> user = rig.user();
  std::vector<OwnedRecord> all;
  std::chrono::sys_time<std::chrono::milliseconds> fetched_at{};
};

TEST_F(OwnershipRecordsOfOnePage, AreAskedOnceWithTheLibraryToken) {
  EXPECT_EQ(fetched_at, rig.now());
  ASSERT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(
      rig.http.seen.back().url,
      "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/"
      "internal_entitlements"
      "?fields=game_meta%2Csku_meta&entitlement_type=5&start=0&size=300");
  ASSERT_FALSE(rig.http.seen.back().headers.empty());
  EXPECT_EQ(rig.http.seen.back().headers.front().value, "Bearer TOKEN1");
}

TEST_F(OwnershipRecordsOfOnePage, KeepWhatPsSaysOfAFullGame) {
  const auto& bus = all[0];
  EXPECT_EQ(bus.fields().id, bus_claim_id);
  EXPECT_EQ(bus.fields().product_id, bus_claim_id);
  EXPECT_EQ(bus.fields().name, "Coach Simulator 21 Next Stop");
  EXPECT_TRUE(bus.fields().has_game_details);
  EXPECT_TRUE(bus.fields().active);
  EXPECT_EQ(bus.fields().feature_type, OwnedRecord::full_game_feature);
  EXPECT_EQ(bus.fields().package_type, "PS5GD");
  EXPECT_TRUE(bus.fields().sku_type.empty());
  EXPECT_EQ(bus.fields().platform_id, "ps5");
  EXPECT_FALSE(bus.fields().preorder);
  EXPECT_TRUE(bus.fields().cover_url.empty());
  EXPECT_EQ(bus.fields().active_date, "2026-10-02T09:14:00.000Z");
  EXPECT_EQ(bus.held_as(), HeldAs::full_game);
  EXPECT_TRUE(bus.release().time());
  EXPECT_EQ(bus.service(), ServiceGeneration::ps5_cloud);
}

TEST_F(OwnershipRecordsOfOnePage, KeepWhatPsSaysOfAnAddOnOutsideGameMeta) {
  const auto& add_on = all[2];
  EXPECT_EQ(add_on.fields().name, "Loose name");
  EXPECT_FALSE(add_on.fields().has_game_details);
  EXPECT_FALSE(add_on.fields().active);
  EXPECT_EQ(add_on.fields().feature_type, OwnedRecord::add_on_feature);
  EXPECT_EQ(add_on.fields().sku_type, "TRIAL");
  EXPECT_EQ(add_on.fields().platform_id, "ps4");
  EXPECT_TRUE(add_on.fields().preorder);
  EXPECT_FALSE(add_on.fields().active_date);
  EXPECT_EQ(add_on.fields().cover_url, "https://image.example/row.png");
  EXPECT_TRUE(add_on.release().is_unannounced());
  EXPECT_EQ(add_on.service(), ServiceGeneration::old_service);
}

TEST_F(OwnershipRecordsOfOnePage, PreferTheIconAsTheCover) {
  const auto& icon = all[3];
  EXPECT_EQ(icon.fields().cover_url, "https://image.example/icon.png");
  EXPECT_EQ(icon.fields().sku_type, "FULL");
  EXPECT_EQ(icon.fields().active_date, "17");
  EXPECT_TRUE(icon.release().is_unannounced());
  EXPECT_EQ(icon.held_as(), HeldAs::disc_upgrade);
  EXPECT_TRUE(icon.fields().platform_id.empty());
}

TEST_F(OwnershipRecordsOfOnePage, FallBackToTheMetaImageAsTheCover) {
  const auto& meta = all[4];
  EXPECT_EQ(meta.fields().cover_url, "https://image.example/meta.png");
  EXPECT_EQ(meta.held_as(), HeldAs::trial_access);
  EXPECT_EQ(meta.fields().active_date, "");
  EXPECT_TRUE(meta.release().is_unrestricted());
}

TEST_F(OwnershipRecordsOfOnePage, BundleRecordsSharingAProduct) {
  const auto library =
      OwnedRecord::library_of(libchiaking::AccountId{1}, all, fetched_at);
  const auto bundles = library.bundles();
  ASSERT_EQ(bundles.size(), 4U);
  EXPECT_EQ(bundles[0].product, bus_claim_id);
  EXPECT_EQ(bundles[0].siblings.size(), 1U);
  EXPECT_EQ(bundles[2].product, "EP1-CUSA07004_00-BUNDLE");
  ASSERT_EQ(bundles[2].siblings.size(), 2U);
  EXPECT_EQ(bundles[2].siblings[0], "EP1-CUSA07004_00-ADDON");
  EXPECT_EQ(bundles[2].siblings[1], "EP1-CUSA07005_00-ICON");
}

TEST_F(OwnershipRecords, KeepTheSkuAndTheConcept) {
  const auto user = rig.user();
  authorized(rig);
  rig.http.on(
      "GET", "internal_entitlements",
      {{200,
        owned_page(
            {R"({"id":"A","product_id":"A","sku_id":"A-E001","conceptId":90000014,"game_meta":{"name":"a","conceptId":"7"}})",
             R"({"id":"B","product_id":"B","concept_id":"900002","game_meta":{"name":"b","conceptId":"7"}})",
             R"({"id":"C","product_id":"C","game_meta":{"name":"c","conceptId":"7"}})",
             R"({"id":"D","product_id":"D","conceptId":"not digits","game_meta":{"name":"d"}})"})}});
  const auto listed = records(rig, *user);
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().records.size(), 4U);
  const auto& all = listed.value().records;
  EXPECT_EQ(all[0].fields().sku_id, "A-E001");
  EXPECT_EQ(all[0].fields().concept_id, "90000014");
  EXPECT_TRUE(all[1].fields().sku_id.empty());
  EXPECT_EQ(all[1].fields().concept_id, "900002");
  EXPECT_EQ(all[2].fields().concept_id, "7");
  EXPECT_TRUE(all[3].fields().concept_id.empty());
  EXPECT_TRUE(all[3].fields().sku_id.empty());
}

TEST_F(OwnershipRecords, PageByThreeHundredWithAPause) {
  const auto user = rig.user();
  quickly_authorized(rig);
  rig.http.on("GET", "start=0&size=300", {{200, full_owned_page()}});
  rig.http.on("GET", "start=300&size=300",
              {{200, R"({"entitlements":[{"id":"LAST","product_id":"P"}]})"}});
  const auto listed = records(rig, *user);
  ASSERT_TRUE(listed);
  EXPECT_EQ(rig.http.count("users/me/internal_entitlements"), 2);
  ASSERT_FALSE(rig.waiter.waits.empty());
  EXPECT_EQ(rig.waiter.waits.back(), 100);
  EXPECT_EQ(listed.value().records.size(), 301U);
  const auto library =
      OwnedRecord::library_of(libchiaking::AccountId{1}, listed.value().records,
                              listed.value().fetched_at);
  ASSERT_EQ(library.bundles().size(), 1U);
  EXPECT_EQ(library.bundles()[0].product, "P");
  const auto& siblings = library.bundles()[0].siblings;
  ASSERT_EQ(siblings.size(), 301U);
  EXPECT_EQ(siblings.front(), "E0");
  EXPECT_EQ(siblings.back(), "LAST");
}

TEST_F(OwnershipRecords, StopAtThePageLimit) {
  const auto user = rig.user();
  std::string full = R"({"entitlements":[)";
  for (int index = 0;
       index < libchiaking::psn::rest::commerce_entitlements_page_size; ++index)
    full += std::string{index ? "," : ""} + "{}";
  full += "]}";
  authorized(rig);
  rig.http.on("GET", "internal_entitlements", {{200, full}});
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  ASSERT_TRUE(listed.error().cause());
  EXPECT_EQ(listed.error().cause()->reason,
            users::Unanswered::Reason::malformed);
  EXPECT_EQ(rig.http.count("users/me/internal_entitlements"),
            libchiaking::psn::rest::commerce_entitlements_page_limit);
  EXPECT_GE(rig.waiter.waits.size(),
            static_cast<std::size_t>(
                libchiaking::psn::rest::commerce_entitlements_page_limit - 1));
  EXPECT_FALSE(listed.error().needs_sign_in());
  EXPECT_FALSE(user->is_expired());
}

TEST_F(OwnershipRecords, TreatAServerErrorOnALaterPageAsUnavailable) {
  const auto user = rig.user();
  quickly_authorized(rig);
  rig.http.on("GET", "start=0&size=300", {{200, full_owned_page()}});
  rig.http.on("GET", "start=300&size=300",
              {{500, R"({"error":{"code":3000,"message":"Internal error"}})"}});
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().http_status(), 500);
  ASSERT_TRUE(listed.error().cause());
  EXPECT_EQ(listed.error().cause()->reason,
            users::Unanswered::Reason::unavailable);
  EXPECT_EQ(rig.http.count("users/me/internal_entitlements"), 2);
}

TEST_F(OwnershipRecords, TreatARejectedLaterPageAsUnansweredWithoutSignOut) {
  const auto user = rig.user();
  quickly_authorized(rig);
  rig.http.on("GET", "start=0&size=300", {{200, full_owned_page()}});
  rig.http.on("GET", "start=300&size=300", {{429, "{}"}});
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  EXPECT_FALSE(listed.error().needs_sign_in());
  EXPECT_FALSE(user->is_expired());
}

TEST_F(OwnershipRecords, TreatAnUnreadablePageAsMalformedNotEmpty) {
  const auto user = rig.user();
  authorized(rig);
  rig.http.on("GET", "internal_entitlements", {{200, "not json"}});
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  ASSERT_TRUE(listed.error().cause());
  EXPECT_EQ(listed.error().cause()->reason,
            users::Unanswered::Reason::malformed);
  EXPECT_FALSE(user->is_expired());
}

TEST_F(OwnershipRecords, ExpireTheSignInOnAnUnauthorizedPage) {
  const auto user = rig.user();
  quickly_authorized(rig);
  rig.http.on("GET", "internal_entitlements", {{403, ""}});
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().reason(), Unanswered::Reason::user);
  EXPECT_TRUE(listed.error().needs_sign_in());
  ASSERT_TRUE(listed.error().cause());
  EXPECT_EQ(listed.error().cause()->reason,
            users::Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(listed.error().http_status(), 403);
  EXPECT_TRUE(user->is_expired());
}

TEST_F(OwnershipRecords, AskOneRecordForAnOwnedTitle) {
  const auto user = rig.user();
  authorized(rig);
  rig.http.on(
      "GET",
      "internal_entitlements/EP-PPSA10001_00-GAME?fields=game_meta%2Csku_meta",
      {{200, std::string{title_entitlement}}});
  const auto found =
      one(rig, *user, {"EP-PPSA10001_00-GAME", "EP-PPSA10001_00-STORE"});
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value());
  EXPECT_EQ(found.value()->fields().id, "EP-PPSA10001_00-GAME");
  EXPECT_EQ(found.value()->fields().name, "Comet");
  EXPECT_EQ(found.value()->held_as(), HeldAs::full_game);
  ASSERT_EQ(rig.http.seen.size(), 2U);
  const auto& entitlement = rig.http.seen.back();
  EXPECT_NE(
      entitlement.url.find("users/me/internal_entitlements/"
                           "EP-PPSA10001_00-GAME?fields=game_meta%2Csku_meta"),
      std::string::npos);
  ASSERT_FALSE(entitlement.headers.empty());
  EXPECT_EQ(entitlement.headers.front().value, "Bearer TOKEN1");
  EXPECT_EQ(rig.http.count("entitlement_type=5"), 0);
  EXPECT_EQ(rig.http.count("imagic"), 0);
  EXPECT_EQ(rig.http.count("kamaji/api"), 0);
  EXPECT_EQ(rig.http.count("web.np.playstation.com"), 0);
}

TEST_F(OwnershipRecords, TryEachDistinctIdForATitleNotOwned) {
  const auto user = rig.user();
  authorized(rig, "3600");
  rig.http.on(
      "GET", "internal_entitlements/",
      {{404, R"({"error":{"code":2181,"message":"Entitlement not found"}})"}});
  const auto found = one(
      rig, *user,
      {"EP-PPSA10001_00-GAME", "EP-PPSA10001_00-STORE", "EP-PPSA10001_00-GAME",
       "EP-PPSA10001_00-THIRD", "EP-PPSA10001_00-FOURTH"});
  ASSERT_TRUE(found);
  EXPECT_FALSE(found.value());
  EXPECT_EQ(rig.http.count("internal_entitlements/EP-PPSA10001_00-GAME"), 1);
  EXPECT_EQ(rig.http.count("internal_entitlements/EP-PPSA10001_00-STORE"), 1);
  EXPECT_EQ(rig.http.count("internal_entitlements/EP-PPSA10001_00-THIRD"), 1);
  EXPECT_EQ(rig.http.count("internal_entitlements/EP-PPSA10001_00-FOURTH"), 0);
  EXPECT_EQ(rig.http.count("internal_entitlements/"),
            static_cast<int>(ps::OwnershipRecords::lookup_limit));
  EXPECT_EQ(rig.http.count("response_type=token"), 1);
  EXPECT_EQ(rig.http.count("entitlement_type=5"), 0);
}

TEST_F(OwnershipRecords, ExpireTheSignInWhenOneRecordIsRefused) {
  const auto turned_away = rig.user();
  authorized(rig);
  rig.http.on("GET", "internal_entitlements/", {{403, ""}});
  const auto found =
      one(rig, *turned_away, {"EP-PPSA10001_00-GAME", "EP-PPSA10001_00-STORE"});
  ASSERT_FALSE(found);
  EXPECT_TRUE(found.error().needs_sign_in());
  EXPECT_EQ(found.error().http_status(), 403);
  EXPECT_EQ(rig.http.count("internal_entitlements/"), 1);
  EXPECT_TRUE(turned_away->is_expired());
}

TEST_F(OwnershipRecords, ExpireTheSignInWhenTheLibraryTokenIsRefused) {
  const auto signed_off = rig.user();
  rig.http.on(
      "GET", "response_type=token",
      {{302, "",
        PsRig::location("https://x/grc-response.html?error=login_required")}});
  const auto found = one(rig, *signed_off, {"EP-PPSA10001_00-GAME"});
  ASSERT_FALSE(found);
  EXPECT_TRUE(found.error().needs_sign_in());
  EXPECT_EQ(found.error().http_status(), 302);
  ASSERT_TRUE(found.error().cause());
  EXPECT_EQ(found.error().cause()->reason,
            users::Unanswered::Reason::sign_in_expired);
  EXPECT_EQ(rig.http.count("users/me/internal_entitlements"), 0);
  EXPECT_TRUE(signed_off->is_expired());
}

TEST_F(OwnershipRecords, ReportOneRecordFailingAsUnavailable) {
  const auto unlucky = rig.user();
  authorized(rig);
  rig.http.on("GET", "internal_entitlements/", {{500, ""}});
  const auto found = one(rig, *unlucky, {"EP-PPSA10001_00-GAME"});
  ASSERT_FALSE(found);
  EXPECT_FALSE(found.error().needs_sign_in());
  EXPECT_EQ(found.error().http_status(), 500);
  ASSERT_TRUE(found.error().cause());
  EXPECT_EQ(found.error().cause()->reason,
            users::Unanswered::Reason::unavailable);
  EXPECT_FALSE(unlucky->is_expired());
}

TEST_F(OwnershipRecords, ReportAMismatchedRecordAsMalformed) {
  const auto mismatched = rig.user();
  authorized(rig);
  rig.http.on("GET", "internal_entitlements/",
              {{200, owned_page({other_owned})}});
  const auto found = one(rig, *mismatched, {"EP-PPSA10001_00-GAME"});
  ASSERT_FALSE(found);
  EXPECT_EQ(found.error().reason(), Unanswered::Reason::malformed);
}

TEST_F(OwnershipRecords, AnswerASignedOutUserWithoutARequest) {
  const auto home = rig.home_user();
  const auto listed = records(rig, *home);
  ASSERT_FALSE(listed);
  EXPECT_TRUE(listed.error().needs_sign_in());
  ASSERT_TRUE(listed.error().cause());
  EXPECT_EQ(listed.error().cause()->reason,
            users::Unanswered::Reason::signed_out);
  const auto found = one(rig, *home, {"EP-PPSA10001_00-GAME"});
  ASSERT_FALSE(found);
  ASSERT_TRUE(found.error().cause());
  EXPECT_EQ(found.error().cause()->reason,
            users::Unanswered::Reason::signed_out);
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(OwnershipRecords, NeverReachTheLibraryForAnExpiredUser) {
  const auto user = rig.user();
  rig.http.on(
      "GET", "response_type=token",
      {{302, "",
        PsRig::location("https://x/grc-response.html?error=login_required")}});
  EXPECT_FALSE(records(rig, *user));
  EXPECT_TRUE(user->is_expired());
  const auto asked = rig.http.seen.size();
  const auto again = records(rig, *user);
  ASSERT_FALSE(again);
  EXPECT_TRUE(again.error().needs_sign_in());
  ASSERT_TRUE(again.error().cause());
  EXPECT_EQ(again.error().cause()->reason,
            users::Unanswered::Reason::sign_in_expired);
  EXPECT_TRUE(user->is_expired());
  ASSERT_EQ(rig.http.seen.size(), asked + 1);
  EXPECT_NE(rig.http.seen.back().url.find("response_type=token"),
            std::string::npos);
  EXPECT_EQ(rig.http.count("users/me/internal_entitlements"), 0);
}

TEST_F(OwnershipRecords, TreatCancellationAsNoRefusal) {
  const auto user = rig.user();
  rig.stop->stopped = true;
  const auto listed = records(rig, *user);
  ASSERT_FALSE(listed);
  EXPECT_TRUE(listed.error().is_cancelled());
  EXPECT_FALSE(listed.error().needs_sign_in());
  EXPECT_TRUE(rig.http.seen.empty());
  EXPECT_FALSE(user->is_expired());
}

TEST_F(OwnershipRecords, SetTheReleaseFromThePlayabilityDate) {
  const auto user = rig.user();
  authorized(rig);
  rig.http.on(
      "GET", "internal_entitlements",
      {{200,
        owned_page(
            {R"({"id":"H","product_id":"H","preorder_flag":true,"active_date":"2023-05-05T04:00:00Z","sku_meta":{"playability_date":"2023-02-07T05:00:00Z"},"game_meta":{"name":"h"}})",
             R"({"id":"I","product_id":"I","preorder_flag":true,"active_date":"2023-05-05T04:00:00Z","game_meta":{"name":"i"}})"})}});
  const auto listed = records(rig, *user);
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().records.size(), 2U);
  const auto& all = listed.value().records;
  const auto playable =
      catalog::ReleaseDate::parse_time("2023-02-07T05:00:00Z");
  const auto active = catalog::ReleaseDate::parse_time("2023-05-05T04:00:00Z");
  ASSERT_TRUE(playable);
  ASSERT_TRUE(active);
  EXPECT_EQ(all[0].fields().playability_date, "2023-02-07T05:00:00Z");
  EXPECT_EQ(all[0].release(), catalog::ReleaseDate::at(*playable));
  EXPECT_FALSE(all[1].fields().playability_date);
  EXPECT_EQ(all[1].release(), catalog::ReleaseDate::at(*active));
}

}  // namespace
}  // namespace catalog_test
