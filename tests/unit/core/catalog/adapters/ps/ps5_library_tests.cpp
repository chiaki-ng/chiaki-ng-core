#include "src/core/catalog/adapters/ps/ps5_library.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fixtures.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace ps = libchiaking::catalog::ps;
using catalog::HeldAs;
using catalog::LibraryState;
using catalog::NotAGame;
using catalog::OwnershipId;
using catalog::Platform;
using catalog::Ps5Entitlement;
using catalog::ReleaseDate;
using catalog::ServiceGeneration;
using catalog::Unanswered;

class Ps5Library : public ::testing::Test {
 protected:
  static constexpr std::string_view ps5_games =
      "internal/entitlements?isActive=true&gameMetaPackageType=PSGD";
  static constexpr std::string_view ps5_game = "internal/entitlements/";

  static auto id(std::string_view text) -> OwnershipId {
    auto parsed = OwnershipId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  static auto at(std::string_view time) -> ReleaseDate {
    auto parsed = ReleaseDate::parse_time(time);
    EXPECT_TRUE(parsed) << time;
    return ReleaseDate::at(std::move(parsed).value());
  }

  static auto record(const ps::Ps5Library::Scanned& scan, std::string_view code)
      -> const Ps5Entitlement& {
    for (const auto& held : scan.records)
      if (held.id().text().ends_with(code)) return held;
    ADD_FAILURE() << "no record ends with " << code;
    return scan.records.front();
  }

  static auto scanned(PsRig& rig) -> ps::Ps5Library::Scanned {
    const auto user = rig.user();
    auto scan = ps::Ps5Library{*user, rig.client, rig.wall}.scan();
    EXPECT_TRUE(scan);
    return std::move(scan).value();
  }

  auto scanned() -> ps::Ps5Library::Scanned { return scanned(rig); }

  void serves(std::string body) {
    rig.http.on("GET", std::string{ps5_games}, {{200, std::move(body)}});
  }

  PsRig rig;
};

class Ps5LibrarySyntheticPage : public Ps5Library {
 protected:
  void SetUp() override {
    serves(test_support::psn::Fixtures::text(
        "entitlements_v2_psgd_synthetic.json"));
    scan = scanned();
  }

  ps::Ps5Library::Scanned scan;
};

TEST_F(Ps5LibrarySyntheticPage, ReadsEveryRecordInOneFetch) {
  EXPECT_EQ(scan.records.size(), 13U);
  EXPECT_EQ(scan.read, 13U);
  EXPECT_EQ(scan.ownership, LibraryState::fetched);
  EXPECT_EQ(scan.revision_restarts, 0U);
  EXPECT_EQ(scan.fetched_at, rig.now());
}

TEST_F(Ps5LibrarySyntheticPage, KeepsTheFactsOfAFullGame) {
  const auto& full = record(scan, "FAKE0044TITLE000");
  EXPECT_FALSE(full.why_not_a_game());
  EXPECT_TRUE(full.release().is_unrestricted());
  const auto& facts = full.facts();
  ASSERT_TRUE(facts.product);
  EXPECT_EQ(facts.product->text(), "EP0109-PPSA07015_00-FAKE0044TITLE000");
  ASSERT_TRUE(facts.sku);
  EXPECT_EQ(facts.sku->text(), "EP0109-PPSA07015_00-FAKE0044TITLE000-E001");
  ASSERT_TRUE(facts.title);
  EXPECT_EQ(facts.title->text(), "PPSA07015_00");
  ASSERT_TRUE(facts.concept_id);
  EXPECT_EQ(facts.concept_id->text(), "90000001");
  EXPECT_EQ(facts.name, "Full Game Streams");
  EXPECT_EQ(facts.cover_url,
            "https://image.api.playstation.com/concept/1001.png");
}

TEST_F(Ps5LibrarySyntheticPage, OwnsAFullGameThatStreams) {
  const auto& full = record(scan, "FAKE0044TITLE000");
  const auto& facts = full.facts();
  const auto title = full.as_owned_title();
  ASSERT_TRUE(title);
  EXPECT_EQ(title->id().text(), "EP0109-PPSA07015_00-FAKE0044TITLE000");
  EXPECT_EQ(title->service(), ServiceGeneration::ps5_cloud);
  EXPECT_EQ(title->platform(), Platform::ps5);
  EXPECT_EQ(title->held_as(), HeldAs::full_game);
  EXPECT_EQ(title->streams(), true);
  EXPECT_EQ(title->product_id(), facts.product);
  EXPECT_EQ(title->concept_id(), facts.concept_id);
  EXPECT_EQ(title->cover_url(), facts.cover_url);
}

TEST_F(Ps5LibrarySyntheticPage, RefusesRecordsTheStreamingRuleRulesOut) {
  EXPECT_EQ(record(scan, "FAKE0080TITLE000").why_not_a_game(),
            NotAGame::streaming_not_supported);
  EXPECT_EQ(record(scan, "FAKE0124TITLE000").why_not_a_game(),
            NotAGame::streaming_unknown);
  EXPECT_EQ(record(scan, "FAKE0035TITLE000").why_not_a_game(),
            NotAGame::disc_required);
  EXPECT_EQ(record(scan, "FAKE0082TITLE000").why_not_a_game(),
            NotAGame::not_unified);
  EXPECT_EQ(record(scan, "FAKE0065TITLE000").why_not_a_game(),
            NotAGame::inactive);
  for (const auto* code :
       {"FAKE0080TITLE000", "FAKE0124TITLE000", "FAKE0035TITLE000",
        "FAKE0082TITLE000", "FAKE0065TITLE000"})
    EXPECT_FALSE(record(scan, code).as_owned_title()) << code;
}

TEST_F(Ps5LibrarySyntheticPage, HoldsTrialsAsTrialAccess) {
  EXPECT_FALSE(record(scan, "FAKE0129TITLE000").why_not_a_game());
  const auto trial = record(scan, "FAKE0129TITLE000").as_owned_title();
  ASSERT_TRUE(trial);
  EXPECT_EQ(trial->held_as(), HeldAs::trial_access);
  EXPECT_EQ(trial->streams(), true);
  const auto play_first = record(scan, "FAKE0091TITLE000").as_owned_title();
  ASSERT_TRUE(play_first);
  EXPECT_EQ(play_first->held_as(), HeldAs::trial_access);
}

TEST_F(Ps5LibrarySyntheticPage, DatesReleasesAndKeepsThemOnTheOwnedTitle) {
  EXPECT_EQ(record(scan, "FAKE0101TITLE000").release(),
            at("2031-05-01T00:00:00Z"));
  EXPECT_EQ(record(scan, "FAKE0098TITLE000").release(),
            at("2031-06-01T00:00:00Z"));
  EXPECT_TRUE(record(scan, "FAKE0099TITLE000").release().is_unannounced());
  EXPECT_TRUE(record(scan, "FAKE0100TITLE000").release().is_unannounced());
  EXPECT_TRUE(record(scan, "FAKE0045TITLE000").release().is_unrestricted());
  for (const auto* code :
       {"FAKE0101TITLE000", "FAKE0098TITLE000", "FAKE0099TITLE000",
        "FAKE0100TITLE000", "FAKE0045TITLE000"}) {
    EXPECT_FALSE(record(scan, code).why_not_a_game()) << code;
    const auto dated = record(scan, code).as_owned_title();
    ASSERT_TRUE(dated) << code;
    EXPECT_EQ(dated->release(), record(scan, code).release()) << code;
  }
}

TEST_F(Ps5LibrarySyntheticPage, MakesTitlesOfTheGames) {
  EXPECT_EQ(Ps5Entitlement::titles_of(scan.records).size(), 8U);
}

TEST_F(Ps5Library, NeverDecidesARecordByNameOrPackageHeuristics) {
  serves(
      R"({"revisionId":1,"totalResults":5,"entitlements":[)"
      R"({"id":"EP0100-PPSA00001_00-FAKE0031TITLE000","productId":"EP0100-PPSA00001_00-FAKE0031TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":3,"skuMeta":{"playabilityDate":null},)"
      R"("gameMeta":{"name":"Comet Demo","packageType":"PSGD","iconUrl":"https://game/icon.png"},)"
      R"("productMeta":{"imageUrl":"https://product/image.png"},"streamingSupported":true},)"
      R"({"id":"EP0100-PPSA00002_00-FAKE0004TITLE000","productId":"EP0100-PPSA00002_00-FAKE0004TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":3,"titleMeta":{"name":"Art Book Trial"},)"
      R"("gameMeta":{"packageType":"PSGD"},"productMeta":{"imageUrl":"https://product/art.png"},"streamingSupported":true},)"
      R"({"id":"EP0100-PPSA00003_00-FAKE0070TITLE000","productId":"EP0100-PPSA00003_00-FAKE0070TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"name":"Media","packageType":"PSMEDIA"},"streamingSupported":true},)"
      R"({"id":"EP0100-PPSA07001_00-FAKE0078TITLE000","activeFlag":true,"entitlementType":5,"featureType":3,)"
      R"("gameMeta":{"packageType":"PSGD"},"streamingSupported":true},)"
      R"({"id":"EP0100-PPSA07002_00-FAKE0002TITLE000","productId":"EP0100-PPSA07002_00-FAKE0002TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":0,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true}]})");
  const auto scan = scanned();
  const auto& demo = record(scan, "FAKE0031TITLE000");
  EXPECT_FALSE(demo.why_not_a_game());
  EXPECT_TRUE(demo.is_named_like_an_extra());
  EXPECT_EQ(demo.facts().cover_url, "https://game/icon.png");
  ASSERT_TRUE(demo.as_owned_title());
  EXPECT_EQ(demo.as_owned_title()->name(), "Comet Demo");
  const auto& art = record(scan, "FAKE0004TITLE000");
  EXPECT_FALSE(art.why_not_a_game());
  EXPECT_EQ(art.facts().name, "Art Book Trial");
  EXPECT_EQ(art.facts().cover_url, "https://product/art.png");
  EXPECT_EQ(record(scan, "FAKE0070TITLE000").why_not_a_game(),
            NotAGame::other_package);
  EXPECT_EQ(record(scan, "FAKE0078TITLE000").why_not_a_game(),
            NotAGame::no_product);
  EXPECT_EQ(record(scan, "FAKE0002TITLE000").why_not_a_game(),
            NotAGame::other_feature);
}

TEST_F(Ps5Library, GatesAReleaseOnlyByTheCoreAndPlaceholderFlags) {
  const auto entry = [](std::string_view code, std::string_view flags) {
    return R"({"id":"EP0100-PPSA0000)" + std::string{code.substr(0, 1)} +
           R"(_00-)" + std::string{code} + R"(","productId":"EP0100-PPSA0000)" +
           std::string{code.substr(0, 1)} + R"(_00-)" + std::string{code} +
           R"(","activeFlag":true,"entitlementType":5,"featureType":3,)"
           R"("gameMeta":{"packageType":"PSGD"},"streamingSupported":true,)"
           R"("activeDate":"2031-03-01T00:00:00Z",)" +
           std::string{flags} +
           R"("skuMeta":{"playabilityDate":"2031-04-01T00:00:00Z"}})";
  };
  serves(
      R"({"revisionId":1,"totalResults":4,"entitlements":[)" +
      entry(
          "1PREORDERFLAGONLY",
          R"("preorderFlag":true,"preorderCoreFlag":false,"preorderPlaceholderFlag":false,)") +
      "," + entry("2COREFLAG0000000", R"("preorderCoreFlag":true,)") + "," +
      entry("3PLACEHOLDER0000", R"("preorderPlaceholderFlag":true,)") + "," +
      entry("4NOFLAGS00000000", "") + "]}");
  const auto scan = scanned();
  ASSERT_EQ(scan.records.size(), 4U);
  EXPECT_TRUE(record(scan, "1PREORDERFLAGONLY").release().is_unrestricted());
  EXPECT_EQ(record(scan, "2COREFLAG0000000").release(),
            at("2031-04-01T00:00:00Z"));
  EXPECT_EQ(record(scan, "3PLACEHOLDER0000").release(),
            at("2031-04-01T00:00:00Z"));
  EXPECT_TRUE(record(scan, "4NOFLAGS00000000").release().is_unrestricted());
  const auto only_flagged = record(scan, "1PREORDERFLAGONLY").as_owned_title();
  ASSERT_TRUE(only_flagged);
  EXPECT_TRUE(only_flagged->release().is_unrestricted());
}

TEST_F(Ps5Library, MakesTheScanPartialWhenTheRevisionNeverSettles) {
  const auto page = [](int revision) {
    return R"({"revisionId":)" + std::to_string(revision) +
           R"(,"totalResults":200,"entitlements":[{"id":"EP0100-PPSA00001_00-GAME000000000000"}]})";
  };
  rig.http.on("GET", std::string{ps5_games},
              {{200, page(1)},
               {200, page(2)},
               {200, page(3)},
               {200, page(4)},
               {200, page(5)},
               {200, page(6)}});
  const auto scan = scanned();
  EXPECT_EQ(scan.ownership, LibraryState::partial);
  EXPECT_EQ(scan.revision_restarts, 2U);
  EXPECT_EQ(scan.records.size(), 1U);
}

TEST_F(Ps5Library, LeavesAnUnreadableScanUnanswered) {
  rig.http.on("GET", std::string{ps5_games}, {{503, ""}});
  const auto user = rig.user();
  const auto scan = ps::Ps5Library{*user, rig.client, rig.wall}.scan();
  ASSERT_FALSE(scan);
  EXPECT_EQ(scan.error().reason(), Unanswered::Reason::user);
}

TEST_F(Ps5Library, FindsOneOwnedRecord) {
  const std::string owned_body =
      R"({"id":"EP0109-PPSA07028_00-FAKE0110TITLE000","productId":"EP0109-PPSA07028_00-FAKE0110TITLE000",)"
      R"("activeFlag":true,"entitlementType":5,"featureType":3,"gameMeta":{"packageType":"PSGD"},"streamingSupported":true,)"
      R"("preorderCoreFlag":true,"skuMeta":{"playabilityDate":"2031-02-01T00:00:00Z"}})";
  rig.http.on("GET", std::string{ps5_game}, {{200, owned_body}});
  const auto user = rig.user();
  const auto found = ps::Ps5Library{*user, rig.client, rig.wall}.one(
      id("EP0109-PPSA07028_00-FAKE0110TITLE000"));
  ASSERT_TRUE(found);
  ASSERT_TRUE(found.value());
  EXPECT_FALSE(found.value()->why_not_a_game());
  EXPECT_EQ(found.value()->release(), at("2031-02-01T00:00:00Z"));
  EXPECT_EQ(rig.http.count("internal/entitlements/"
                           "EP0109-PPSA07028_00-FAKE0110TITLE000?fields="),
            1);
}

TEST_F(Ps5Library, FindsNoRecordWhenEveryLookupIsAbsent) {
  for (const auto& gone :
       {PsRig::Reply{404, "{}"},
        PsRig::Reply{400, R"({"error":{"code":3161955}})"}}) {
    PsRig absent;
    absent.http.on("GET", std::string{ps5_game}, {gone});
    const auto asker = absent.user();
    const auto none = ps::Ps5Library{*asker, absent.client, absent.wall}.one_of(
        std::vector{id("EP-PPSA00001_00-A"), id("EP-PPSA00002_00-B"),
                    id("EP-PPSA00001_00-A"), id("EP-PPSA00003_00-C"),
                    id("EP-PPSA07001_00-D")});
    ASSERT_TRUE(none) << gone.status;
    EXPECT_FALSE(none.value()) << gone.status;
    EXPECT_EQ(absent.http.count(ps5_game),
              static_cast<int>(ps::Ps5Library::lookup_limit))
        << gone.status;
  }
}

TEST_F(Ps5Library, LeavesAFailedLookupUnanswered) {
  rig.http.on("GET", std::string{ps5_game}, {{500, ""}});
  const auto unlucky = rig.user();
  const auto failed = ps::Ps5Library{*unlucky, rig.client, rig.wall}.one(
      id("EP-PPSA00001_00-A"));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason(), Unanswered::Reason::user);
}

TEST_F(Ps5Library, AsksOnlyAboutPs5Ids) {
  EXPECT_TRUE(
      ps::Ps5Library::asks_about(id("EP0109-PPSA07028_00-FAKE0110TITLE000")));
  EXPECT_TRUE(ps::Ps5Library::asks_about(id("EP0100-BUNDLE0000_00-NOTITLE")));
  EXPECT_FALSE(
      ps::Ps5Library::asks_about(id("EP0109-CUSA37002_00-FAKE0107TITLE000")));
}

}  // namespace
}  // namespace catalog_test
