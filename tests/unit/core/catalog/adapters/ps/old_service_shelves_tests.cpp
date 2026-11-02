#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/store_walk_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace catalog_test {
namespace {

using libchiaking::catalog::CloudAccess;
using libchiaking::catalog::ListOrigin;
using libchiaking::catalog::Shelf;
using libchiaking::catalog::Unanswered;
using libchiaking::catalog::ps::OldServiceShelves;
using libchiaking::users::Region;
using Availability = CloudAccess::Availability;
using UserReason = libchiaking::users::Unanswered::Reason;

class OldServiceShelvesWalk : public ::testing::Test, public StoreWalkRig {
 protected:
  void two_genre_shelves() {
    session_opens();
    http.on(
        "GET", "/user/stores",
        {{200,
          R"({"header":{"status_code":"0x0000"},"data":{"base_url":"https://psnow.playstation.com/store/container/GB/en/19/ROOT"}})"}});
    http.on(
        "GET", "ROOT?size=100",
        {{200,
          R"({"links":[{"name":"Action","url":"https://psnow.playstation.com/store/cat/ACTION"},{"name":"H - L","url":"https://psnow.playstation.com/store/cat/HL"}]})"}});
    http.on(
        "GET", "cat/ACTION?start=0&size=500",
        {{200,
          R"({"links":[{"id":"EP0109-CUSA17003_00-FAKE0059TITLE000","name":"Skyline Zero Ember™ Complete Edition"},{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"}]})"}});
  }

  static auto only_the_authorise_request_carries_npsso(
      const std::vector<FragmentHttp::Seen>& requests)
      -> ::testing::AssertionResult {
    for (const auto& request : requests)
      for (const auto& header : request.headers)
        if (header.name == "Cookie" && header.value.find("npsso=") == 0 &&
            request.url.find("oauth/authorize") == std::string::npos)
          return ::testing::AssertionFailure()
                 << request.url << " carries the npsso cookie";
    return ::testing::AssertionSuccess();
  }
};

TEST_F(OldServiceShelvesWalk, ListsAGenreOnlyTitleOnce) {
  const auto signed_in_user = user();
  two_genre_shelves();
  http.on(
      "GET", "cat/HL?start=0&size=500",
      {{200,
        R"({"links":[{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"}]})"}});
  const auto result = walked(*signed_in_user);
  ASSERT_TRUE(result);
  const auto& listing = result.value();
  EXPECT_EQ(listed_times(listing, "EP0109-CUSA17003_00-FAKE0059TITLE000"), 1);
  EXPECT_EQ(listed_times(listing, "EP-CUSA00001_00-LEGACY_CLOUD_STREAM"), 1);
  EXPECT_EQ(http.count("cat/ACTION"), 1);
  EXPECT_EQ(http.count("cat/HL"), 1);
  ASSERT_EQ(listing.entries().size(), 2U);
  EXPECT_EQ(listing.entries()[0].name, "Skyline Zero Ember™ Complete Edition");
  EXPECT_EQ(listing.entries()[0].shelf, Shelf::old_service);
  EXPECT_EQ(listing.entries()[1].shelf, Shelf::old_service);
  EXPECT_EQ(listing.source().origin, ListOrigin::account_service);
  EXPECT_TRUE(listing.source().complete);
  EXPECT_EQ(listing.source().region, locale("en-GB"));
  EXPECT_EQ(listing.source().settled_locale, locale("en-GB"));
  EXPECT_EQ(listing.source().fetched_at, now());
  EXPECT_EQ(signed_in_user->region(), (Region{"GB", "en"}));
}

TEST_F(OldServiceShelvesWalk, LeavesTheListingIncompleteAfterAFailedShelf) {
  const auto signed_in_user = user();
  two_genre_shelves();
  http.on("GET", "cat/HL?start=0&size=500", {{500, "{}"}, {500, "{}"}});
  const auto fetched = walked(*signed_in_user);
  ASSERT_TRUE(fetched);
  const auto& listing = fetched.value();
  EXPECT_FALSE(listing.source().complete);
  ASSERT_EQ(listing.entries().size(), 2U);
  EXPECT_EQ(listing.entries()[0].product,
            "EP0109-CUSA17003_00-FAKE0059TITLE000");
  EXPECT_EQ(http.count("cat/ACTION"), 1);
  EXPECT_EQ(http.count("cat/HL"), 2);
  EXPECT_EQ(http.seen.back().url,
            "https://psnow.playstation.com/store/cat/HL?start=0&size=500");
  const auto access =
      OldServiceShelves{*signed_in_user, client, wall}.access_shown_by(fetched);
  EXPECT_TRUE(access.is_available());
  EXPECT_EQ(access.account(), signed_in_user->account_id());
  ASSERT_TRUE(access.listing());
  EXPECT_EQ(*access.listing(),
            (CloudAccess::Listing{2, false, ListOrigin::account_service}));
}

TEST_F(OldServiceShelvesWalk, AnswersARefusedSignInAsTheUser) {
  const auto signed_in_user = user();
  http.on("GET", "response_type=code", {{200, "<html>"}});
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason(), Unanswered::Reason::user);
  EXPECT_TRUE(result.error().needs_sign_in());
  ASSERT_TRUE(result.error().cause());
  EXPECT_EQ(result.error().cause()->reason, UserReason::sign_in_expired);
  EXPECT_TRUE(signed_in_user->is_expired());
  EXPECT_EQ(http.count("/user/stores"), 0);
  EXPECT_EQ(http.count("APOLLOROOT"), 0);
  const auto access =
      OldServiceShelves{*signed_in_user, client, wall}.access_shown_by(result);
  EXPECT_EQ(access.availability(), Availability::sign_in_expired);
  EXPECT_FALSE(access.listing());
  const auto asked = http.seen.size();
  const auto again = walked(*signed_in_user);
  ASSERT_FALSE(again);
  EXPECT_TRUE(again.error().needs_sign_in());
  EXPECT_TRUE(signed_in_user->is_expired());
  EXPECT_EQ(http.seen.size(), asked + 1);
  EXPECT_NE(http.seen.back().url.find("response_type=code"), std::string::npos);
  EXPECT_EQ(http.count("/user/session"), 0);
  EXPECT_EQ(http.count("/user/stores"), 0);
}

TEST_F(OldServiceShelvesWalk, LeavesThePublicStoreWhenUnavailable) {
  const auto signed_in_user = user();
  http.on("GET", "response_type=code", {{503, ""}});
  http.on(
      "GET", "STORE-MSF192014-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"EP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().cause());
  EXPECT_EQ(result.error().cause()->reason, UserReason::unavailable);
  EXPECT_FALSE(result.error().needs_sign_in());
  EXPECT_FALSE(signed_in_user->is_expired());
  const OldServiceShelves shelves{*signed_in_user, client, wall};
  EXPECT_EQ(shelves.access_shown_by(result).availability(),
            Availability::unavailable);
  const auto walked_publicly = fallback("en-GB");
  ASSERT_TRUE(walked_publicly);
  EXPECT_TRUE(walked_publicly.value().source().complete);
  EXPECT_EQ(walked_publicly.value().source().origin,
            ListOrigin::regional_store);
  EXPECT_EQ(walked_publicly.value().source().region, locale("en-GB"));
  ASSERT_EQ(walked_publicly.value().entries().size(), 1U);
  EXPECT_EQ(walked_publicly.value().entries()[0].product, "EP-CUSA00002_00-X");
  EXPECT_EQ(walked_publicly.value().entries()[0].shelf, Shelf::public_store);
  const auto access = shelves.access_shown_by(walked_publicly);
  ASSERT_TRUE(access.listing());
  EXPECT_EQ(*access.listing(),
            (CloudAccess::Listing{1, true, ListOrigin::regional_store}));
}

TEST_F(OldServiceShelvesWalk, WalksTheAmericasStoreForAnUnsupportedRegion) {
  const auto signed_in_user = user();
  region_unsupported("BR", "pt-BR");
  http.on("GET", "container/BR/pt/19/STORE-MSF192018-APOLLOROOT",
          {{404, "{}"}});
  http.on(
      "GET", "container/US/en/19/STORE-MSF192018-APOLLOROOT",
      {{200,
        R"({"total_results":1,"links":[{"id":"UP-CUSA00002_00-X","name":"X","container_type":"product"}]})"}});
  store_lists_ok();
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason(), Unanswered::Reason::region_unsupported);
  EXPECT_EQ(result.error().http_status(), 404);
  EXPECT_FALSE(result.error().needs_sign_in());
  EXPECT_EQ(availability_shown(*signed_in_user, result),
            Availability::region_unsupported);
  ASSERT_EQ(signed_in_user->region(), (Region{"BR", "pt-BR"}));
  const auto region =
      libchiaking::catalog::StoreLocale::for_account(*signed_in_user->region());
  ASSERT_TRUE(region);
  EXPECT_EQ(region->text(), "pt-BR");
  const auto walked_publicly = fallback(region->text());
  ASSERT_TRUE(walked_publicly);
  EXPECT_TRUE(walked_publicly.value().source().complete);
  EXPECT_EQ(walked_publicly.value().entries().size(), 1U);
  EXPECT_EQ(walked_publicly.value().source().settled_locale, locale("en-US"));
  const auto lists =
      libchiaking::catalog::ps::Ps5StoreLists{client, wall}.fetch(*region);
  ASSERT_TRUE(lists);
  EXPECT_EQ(lists.value().source().settled_locale.text(), "pt-BR");
  EXPECT_EQ(http.count("locale=pt-br&"), 6);
}

TEST_F(OldServiceShelvesWalk, ReadsAShelfLongerThanAPageToItsTotal) {
  const auto signed_in_user = user();
  old_service_ok(
      R"({"total_results":600,"links":[{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"}]})");
  http.on(
      "GET", "cat/AB?start=500&size=500",
      {{200,
        R"({"total_results":600,"links":[{"id":"EP-CUSA00002_00-LEGACY_CLOUD_STREAM","name":"Sequel"}]})"}});
  const auto result = walked(*signed_in_user);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().source().complete);
  EXPECT_EQ(result.value().entries().size(), 2U);
  EXPECT_EQ(http.count("cat/AB?"), 2);
  EXPECT_EQ(http.count("cat/AB?start=1000"), 0);
}

TEST_F(OldServiceShelvesWalk, RetriesAFailedShelfAfterAWait) {
  const auto signed_in_user = user();
  old_service_ok();
  for (auto& rule : http.rules)
    if (rule.fragment == "cat/AB?start=0&size=500")
      rule.replies = {Reply{0, "", {}, true},
                      Reply{200, R"({"links":[]})", {}, false}};
  const auto result = walked(*signed_in_user);
  ASSERT_TRUE(result);
  EXPECT_EQ(http.count("cat/AB"), 2);
  ASSERT_FALSE(waiter.waits.empty());
  EXPECT_EQ(waiter.waits.front(), 500);
  EXPECT_EQ(waiter.waits.back(), 100);
  EXPECT_TRUE(result.value().source().complete);
}

TEST_F(OldServiceShelvesWalk, CarriesTheReferenceHeaders) {
  const auto signed_in_user = user();
  old_service_ok();
  EXPECT_TRUE(walked(*signed_in_user));
  ASSERT_EQ(http.seen.size(), 6U);
  const auto& oauth = http.seen.front();
  EXPECT_NE(oauth.url.find("smcid=pc%3Apsnow"), std::string::npos);
  EXPECT_NE(oauth.url.find("scope=kamaji%3Acommerce_native%20kamaji%"
                           "3Acommerce_container"),
            std::string::npos);
  EXPECT_NE(
      oauth.url.find(
          "redirect_uri=https%3A%2F%2Fpsnow.playstation.com%2Fapp%2F2.2.0%"
          "2F133%2F5cdcc037d%2Fgrc-response.html"),
      std::string::npos);
  EXPECT_NE(oauth.url.find("&duid=" + std::string{device} + "&"),
            std::string::npos);
  ASSERT_GE(oauth.headers.size(), 2U);
  EXPECT_EQ(oauth.headers.at(1).name, "Cookie");
  EXPECT_EQ(oauth.headers.at(1).value, "npsso=NPSSO");
  EXPECT_EQ(http.seen.at(1).body,
            "code=CODE1&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989&duid=" +
                std::string{device});
  ASSERT_GE(http.seen.at(2).headers.size(), 2U);
  EXPECT_EQ(http.seen.at(2).headers.at(1).value, "JSESSIONID=JS1");
  EXPECT_EQ(
      http.seen.at(3).url,
      "https://psnow.playstation.com/store/container/GB/en/19/ROOT?size=100");
  EXPECT_EQ(http.seen.at(4).url,
            "https://psnow.playstation.com/store/cat/AB?start=0&size=500");
  EXPECT_EQ(http.seen.at(5).url, "https://x/f?start=0&size=500");
  EXPECT_TRUE(only_the_authorise_request_carries_npsso(http.seen));
}

TEST_F(OldServiceShelvesWalk, TouchesNothingWhenCancelledBeforeStart) {
  const auto signed_in_user = user();
  stop->stopped = true;
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_TRUE(http.seen.empty());
}

TEST_F(OldServiceShelvesWalk, AnswersASignedOutUserWithoutARequest) {
  const auto home = home_user();
  const auto result = walked(*home);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason(), Unanswered::Reason::user);
  EXPECT_TRUE(result.error().needs_sign_in());
  ASSERT_TRUE(result.error().cause());
  EXPECT_EQ(result.error().cause()->reason, UserReason::signed_out);
  EXPECT_TRUE(http.seen.empty());
  EXPECT_EQ(availability_shown(*home, result), Availability::sign_in_expired);
}

TEST_F(OldServiceShelvesWalk, ReportsARejectedServiceAsRejectedAccess) {
  const auto signed_in_user = user();
  http.on("GET", "response_type=code",
          {{302, "",
            location("https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                     "grc-response.html?code=C")}});
  http.on("POST", "/user/session",
          {{403, R"({"header":{"status_code":"0x0003"}})"}});
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  ASSERT_TRUE(result.error().cause());
  EXPECT_EQ(result.error().cause()->reason, UserReason::old_service_refused);
  EXPECT_EQ(result.error().http_status(), 403);
  EXPECT_EQ(http.seen.size(), 2U);
  EXPECT_EQ(availability_shown(*signed_in_user, result),
            Availability::access_rejected);
}

TEST_F(OldServiceShelvesWalk, PrefersTheCoverImage) {
  const auto signed_in_user = user();
  old_service_ok(R"({"links":[
        {"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic","imageUrl":"fallback","images":[{"type":13,"url":"land"}]},
        {"id":"EP-CUSA00003_00-PLAIN","name":"Plain","imageUrl":"only"},
        {"id":"EP-CUSA07001_00-BOTH","name":"Both","imageUrl":"fallback","images":[{"type":12,"url":"wide"},{"type":10,"url":"portrait"}]},
        {"id":"EP-CUSA07002_00-NONE","name":"None","images":[{"type":1,"url":"other"}]},
        {"id":"bad id","name":"Dropped"},
        {"name":"No id"}]})");
  const auto result = walked(*signed_in_user);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value().entries().size(), 4U);
  EXPECT_EQ(result.value().entries()[0].cover_url, "land");
  EXPECT_EQ(result.value().entries()[1].cover_url, "only");
  EXPECT_EQ(result.value().entries()[2].cover_url, "portrait");
  EXPECT_TRUE(result.value().entries()[3].cover_url.empty());
  EXPECT_FALSE(result.value().entries()[0].streaming_supported);
  EXPECT_FALSE(result.value().entries()[0].on_ps4);
}

TEST_F(OldServiceShelvesWalk, AnswersUnavailableWhenEverySourceFails) {
  const auto signed_in_user = user();
  http.on("GET", "response_type=code", {{503, ""}});
  http.on("GET", "APOLLOROOT", {{500, ""}});
  http.on("GET", "imagic", {{404, ""}});
  EXPECT_FALSE(walked(*signed_in_user));
  const auto walked_publicly = fallback("en-GB");
  ASSERT_TRUE(walked_publicly);
  EXPECT_FALSE(walked_publicly.value().source().complete);
  EXPECT_TRUE(walked_publicly.value().entries().empty());
  const auto lists = ps5_lists("en-GB");
  ASSERT_FALSE(lists);
  EXPECT_EQ(lists.error().reason(), Unanswered::Reason::unavailable);
  EXPECT_EQ(lists.error().http_status(), 404);
  EXPECT_EQ(http.count("imagic"), 6 * 2);
}

TEST_F(OldServiceShelvesWalk, StopsEverySourceWhenCancelledDuringAWait) {
  const auto signed_in_user = user();
  old_service_ok();
  http.rules.back().replies = {Reply{0, "", {}, true},
                               Reply{200, R"({"links":[]})", {}, false}};
  waiter.stop_on_wait = 1;
  const auto result = walked(*signed_in_user);
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  const auto lists = ps5_lists("en-GB");
  ASSERT_FALSE(lists);
  EXPECT_TRUE(lists.error().is_cancelled());
  const auto walked_publicly = fallback("en-GB");
  ASSERT_FALSE(walked_publicly);
  EXPECT_TRUE(walked_publicly.error().is_cancelled());
}

TEST(OldServiceStoreRegion, IsReadFromTheContainerUrl) {
  auto region = OldServiceShelves::store_region_of(
      "https://store.example/container/US/en/19/PPSA01234_00?x=1");
  EXPECT_EQ(region.country, "US");
  EXPECT_EQ(region.language, "en");
  region = OldServiceShelves::store_region_of(
      "https://store.example/container/FI/fi/19/EP0109-NPEA_00");
  EXPECT_EQ(region.country, "FI");
  EXPECT_EQ(region.language, "fi");
}

TEST(OldServiceStoreRegion, IsEmptyWithoutAContainerCountry) {
  auto region =
      OldServiceShelves::store_region_of("https://store.example/foo/bar/baz");
  EXPECT_TRUE(region.country.empty());
  EXPECT_TRUE(region.language.empty());
  region = OldServiceShelves::store_region_of(
      "https://store.example/container//en/19/x");
  EXPECT_TRUE(region.country.empty());
  EXPECT_TRUE(region.language.empty());
}

TEST(OldServiceStoreRegion, KeepsTheCountryWithoutAUsableLanguage) {
  auto region = OldServiceShelves::store_region_of(
      "https://store.example/container/US/en");
  EXPECT_EQ(region.country, "US");
  EXPECT_TRUE(region.language.empty());
  region = OldServiceShelves::store_region_of(
      "https://store.example/container/US/abcdefgh/19/x");
  EXPECT_EQ(region.country, "US");
  EXPECT_TRUE(region.language.empty());
}

TEST(OldServiceStoreRegion, RefusesACountryLongerThanSevenLetters) {
  auto region = OldServiceShelves::store_region_of(
      "https://store.example/container/ABCDEFG/en/19/x");
  EXPECT_EQ(region.country, "ABCDEFG");
  EXPECT_EQ(region.language, "en");
  region = OldServiceShelves::store_region_of(
      "https://store.example/container/ABCDEFGH/en/19/x");
  EXPECT_TRUE(region.country.empty());
  EXPECT_TRUE(region.language.empty());
}

}  // namespace
}  // namespace catalog_test
