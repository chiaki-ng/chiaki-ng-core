#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/usecases/cloud_stream/check_exclusion.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalog_test {
namespace {

namespace catalog = libchiaking::catalog;
using catalog::HeldAnswers;
using catalog::TitleId;
using catalog::Unanswered;
using catalog::cloud_stream::ExclusionList;
using catalog::usecases::cloud_stream::CheckExclusion;
using Reply = PsRig::Reply;
using Rule = ExclusionList::Rule;

class ExclusionService {
 public:
  static constexpr std::string_view config_url =
      "https://feature.api.playstation.com/portal/np/portal-config.json";
  static constexpr std::string_view config = "portal/np/portal-config.json";
  static constexpr std::chrono::hours hour{1};

  static auto title(std::string_view text) -> TitleId {
    auto parsed = TitleId::parse(text);
    EXPECT_TRUE(parsed) << text;
    return std::move(parsed).value();
  }

  void serves(std::vector<Reply> replies) {
    rig.http.rules.clear();
    rig.http.on("GET", std::string{config}, std::move(replies));
  }

  auto check(const std::optional<TitleId>& title_id,
             std::string_view product_id = "EP0109-PPSA01234_00-GAME")
      -> CheckExclusion::Verdict {
    CheckExclusion check{rig.http, rig.clock, rig.wall, nullptr, *held};
    return check.of(title_id, PsRig::product(product_id),
                    libchiaking::platform::StopToken{rig.stop});
  }

  auto fetches() const -> int { return rig.http.count(config); }

  void advance(std::chrono::milliseconds by) {
    rig.wall.advance_ms(by.count());
  }

  void restart() {
    held =
        std::make_unique<HeldAnswers>(records, threading, rig.clock, rig.wall);
  }

  PsRig rig;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryRecordStore records{threading};
  std::unique_ptr<HeldAnswers> held =
      std::make_unique<HeldAnswers>(records, threading, rig.clock, rig.wall);
};

class CloudExclusionCheck : public ::testing::Test {
 protected:
  static constexpr std::chrono::hours hour = ExclusionService::hour;

  static auto title(std::string_view text) -> TitleId {
    return ExclusionService::title(text);
  }

  ExclusionService service;
};

TEST_F(CloudExclusionCheck, ReadsTheConfigAnonymously) {
  service.serves({{200, R"({"excludedTitleIds":[]})"}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_EQ(verdict.snapshot_at(), service.rig.now());
  ASSERT_EQ(service.rig.http.seen.size(), 1U);
  const auto& seen = service.rig.http.seen.front();
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url, ExclusionService::config_url);
  for (const auto& header : seen.headers) {
    EXPECT_NE(header.name, "Authorization");
    EXPECT_NE(header.name, "Cookie");
  }
}

TEST_F(CloudExclusionCheck, ExcludesNothingWithAnEmptyList) {
  service.serves({{200, R"({"excludedTitleIds":[]})"}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_FALSE(verdict.refuses_launch());
  EXPECT_FALSE(verdict.is_excluded());
  EXPECT_FALSE(verdict.is_unavailable());
}

TEST_F(CloudExclusionCheck, ExcludesAnExactTitleIdByTheTitleRule) {
  service.serves(
      {{200, R"({"excludedTitleIds":["PPSA01234_00","CUSA00001_00"]})"}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_TRUE(verdict.is_excluded());
  EXPECT_TRUE(verdict.refuses_launch());
  ASSERT_TRUE(verdict.match());
  EXPECT_EQ(verdict.match()->rule, Rule::exact_title_id);
  EXPECT_EQ(verdict.match()->excluded, title("PPSA01234_00"));
  EXPECT_FALSE(service.check(title("PPSA01234")).refuses_launch());
  EXPECT_FALSE(service.check(title("PPSA01234_00X"), "EP0109-PPSA01234_00-GAME")
                   .refuses_launch());
  EXPECT_EQ(service.fetches(), 1);
}

TEST_F(CloudExclusionCheck,
       ExcludesTheProductIdContainingTheTextWithoutATitleId) {
  service.serves({{200, R"({"excludedTitleIds":["CUSA0","PPSA01234"]})"}});
  const auto verdict = service.check(std::nullopt, "EP0109-PPSA01234_00-GAME");
  EXPECT_TRUE(verdict.is_excluded());
  ASSERT_TRUE(verdict.match());
  EXPECT_EQ(verdict.match()->rule, Rule::product_id_contains);
  EXPECT_EQ(verdict.match()->excluded, title("PPSA01234"));
  const auto first = service.check(std::nullopt, "UP0112-CUSA07007_00-GAME");
  ASSERT_TRUE(first.match());
  EXPECT_EQ(first.match()->excluded, title("CUSA0"));
  EXPECT_FALSE(
      service.check(std::nullopt, "EP0109-PPSA07055_00-GAME").refuses_launch());
}

TEST_F(CloudExclusionCheck, UsesAFreshSnapshotWithoutAskingAgain) {
  service.serves({{200, R"({"excludedTitleIds":["PPSA01234_00"]})"}});
  EXPECT_TRUE(service.check(title("PPSA01234_00")).is_excluded());
  service.advance(23 * hour);
  service.serves({{200, R"({"excludedTitleIds":[]})"}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_TRUE(verdict.is_excluded());
  EXPECT_EQ(service.fetches(), 1);
  EXPECT_EQ(verdict.snapshot_at(), service.rig.now());
}

TEST_F(CloudExclusionCheck, RefreshesAStaleSnapshotAndKeepsTheRefresh) {
  service.serves({{200, R"({"excludedTitleIds":["PPSA01234_00"]})"}});
  EXPECT_TRUE(service.check(title("PPSA01234_00")).is_excluded());
  service.advance(ExclusionList::fresh_for);
  service.serves({{200, R"({"excludedTitleIds":["PPSA07038_00"]})"}});
  const auto refreshed = service.check(title("PPSA01234_00"));
  EXPECT_FALSE(refreshed.refuses_launch());
  EXPECT_EQ(service.fetches(), 2);
  const auto refreshed_at = service.rig.now() + ExclusionList::fresh_for;
  EXPECT_EQ(refreshed.snapshot_at(), refreshed_at);
  service.advance(hour);
  EXPECT_TRUE(service.check(title("PPSA07038_00")).is_excluded());
  EXPECT_EQ(service.fetches(), 2);
}

TEST_F(CloudExclusionCheck, IsUnavailableWhenAStaleSnapshotFailsToRefresh) {
  service.serves({{200, R"({"excludedTitleIds":[]})"}});
  EXPECT_FALSE(service.check(title("PPSA01234_00")).refuses_launch());
  service.advance(25 * hour);
  service.serves({{503, ""}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_TRUE(verdict.is_unavailable());
  EXPECT_TRUE(verdict.refuses_launch());
  EXPECT_FALSE(verdict.is_excluded());
  ASSERT_TRUE(verdict.cause());
  EXPECT_EQ(verdict.cause()->reason(), Unanswered::Reason::unavailable);
  EXPECT_EQ(verdict.cause()->http_status(), 503);
  EXPECT_FALSE(verdict.snapshot_at());
  EXPECT_FALSE(verdict.match());
  service.serves({{200, R"({"excludedTitleIds":[]})"}});
  EXPECT_FALSE(service.check(title("PPSA01234_00")).refuses_launch());
}

TEST(CloudExclusionFirstFetch, IsUnavailableWhenTheFetchFails) {
  for (const auto& reply :
       {Reply{0, "", {}, true}, Reply{404, "missing"}, Reply{200, "not json"},
        Reply{200, R"([])"}, Reply{200, R"({})"},
        Reply{200, R"({"excludedTitleIds":null})"},
        Reply{200, R"({"excludedTitleIds":["PPSA01234_00",7]})"},
        Reply{200, R"({"excludedTitleIds":[""]})"}}) {
    ExclusionService service;
    service.serves({reply});
    const auto verdict = service.check(std::nullopt);
    EXPECT_TRUE(verdict.is_unavailable()) << reply.body;
    EXPECT_TRUE(verdict.refuses_launch()) << reply.body;
    EXPECT_TRUE(service.records.names().empty()) << reply.body;
  }
}

TEST_F(CloudExclusionCheck, KeepsTheSnapshotAcrossANewCatalogue) {
  service.serves({{200, R"({"excludedTitleIds":["PPSA01234_00"]})"}});
  EXPECT_TRUE(service.check(title("PPSA01234_00")).is_excluded());
  ASSERT_EQ(service.records.names().size(), 1U);
  EXPECT_EQ(service.records.names().front().text(), "exclusions/cloud");
  service.restart();
  service.advance(2 * hour);
  service.serves({{503, ""}});
  const auto verdict = service.check(title("PPSA01234_00"));
  EXPECT_TRUE(verdict.is_excluded());
  EXPECT_EQ(service.fetches(), 1);
  EXPECT_EQ(verdict.snapshot_at(), service.rig.now());
  service.restart();
  service.advance(23 * hour);
  EXPECT_TRUE(service.check(title("PPSA01234_00")).is_unavailable());
  EXPECT_EQ(service.fetches(), 2);
}

TEST_F(CloudExclusionCheck, ForgetsTheSnapshotStoredUnderItsEarlierName) {
  const auto earlier =
      libchiaking::platform::RecordName::parse("exclusions/portal");
  ASSERT_TRUE(earlier);
  const std::array snapshot{std::byte{'{'}, std::byte{'}'}};
  ASSERT_TRUE(service.records.write_atomic(*earlier, snapshot));
  service.restart();
  EXPECT_TRUE(service.records.names().empty());
}

TEST_F(CloudExclusionCheck, ReportsTheSnapshotAgeAndFreshnessInTheFetchReport) {
  service.serves(
      {{200, R"({"excludedTitleIds":["PPSA01234_00","CUSA00001_00"]})"}});
  static_cast<void>(service.check(title("PPSA01234_00")));
  const auto user = service.rig.user();
  const auto owned =
      catalog::Library::of(user->account_id(), {}, {}, service.rig.now())
          .value();
  static_cast<void>(service.held->kept(
      std::make_shared<const catalog::Library>(owned),
      std::make_shared<const catalog::OwnedPs5Games>(
          catalog::OwnedPs5Games::unread(service.rig.now()))));
  catalog::usecases::ListGames list{service.rig.http, service.rig.clock,
                                    service.rig.wall, nullptr,
                                    nullptr,          *service.held};
  service.advance(2 * hour);
  const auto report = list.how_fetched(*user);
  ASSERT_TRUE(report);
  ASSERT_TRUE(report->cloud_stream.exclusions);
  EXPECT_EQ(report->cloud_stream.exclusions->fetched_at, service.rig.now());
  EXPECT_EQ(report->cloud_stream.exclusions->age, 2 * hour);
  EXPECT_TRUE(report->cloud_stream.exclusions->fresh);
  EXPECT_EQ(report->cloud_stream.exclusions->titles, 2U);
  ASSERT_TRUE(report->cloud_stream.owned);
  EXPECT_EQ(report->cloud_stream.owned->state,
            catalog::LibraryState::unavailable);
  service.advance(23 * hour);
  const auto stale = list.how_fetched(*user);
  ASSERT_TRUE(stale);
  ASSERT_TRUE(stale->cloud_stream.exclusions);
  EXPECT_FALSE(stale->cloud_stream.exclusions->fresh);
}

}  // namespace
}  // namespace catalog_test
