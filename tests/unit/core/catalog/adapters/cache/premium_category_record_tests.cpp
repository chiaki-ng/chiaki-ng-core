#include "tests/unit/core/catalog/adapters/cache/category_record_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

namespace catalog_test {
namespace {

using Record = CategoryRecordLoader::Record;
using Demand = CategoryRecordLoader::Demand;
using Step = CategoryRecordLoader::Step;
using Time = CategoryRecordLoader::Time;
using libchiaking::catalog::cloud_stream::PremiumCategory;

class PremiumCategoryRecordCases : public ::testing::Test {
 protected:
  static constexpr auto minute = CategoryRecordLoader::minute;
  static constexpr auto hour = CategoryRecordLoader::hour;

  static auto locale(std::string_view text) {
    return CategoryRecordLoader::locale(text);
  }

  static auto titles(const libchiaking::catalog::StoreLocale& store_locale,
                     Time fetched_at, std::size_t count) -> PremiumCategory {
    return CategoryRecordLoader::titles(store_locale, fetched_at, count);
  }

  auto absent(std::string_view text) -> testing::AssertionResult {
    const auto name =
        *libchiaking::platform::RecordName::parse("included/en-gb");
    if (!loader.store.write_atomic(
            name, std::as_bytes(std::span{text.data(), text.size()})))
      return testing::AssertionFailure() << "could not store " << text;
    const auto read = loader.cache.read<Record>(locale("en-GB"));
    if (!read) return testing::AssertionFailure() << "the read failed";
    if (read.value())
      return testing::AssertionFailure() << "a record was read from " << text;
    if (!loader.store.names().empty())
      return testing::AssertionFailure() << "the record was kept: " << text;
    return testing::AssertionSuccess();
  }

  CategoryRecordLoader loader;
};

class PremiumCategoryRecordBody : public PremiumCategoryRecordCases {};
class PremiumCategoryRecordTimes : public PremiumCategoryRecordCases {};
class PremiumCategoryRecordLoads : public PremiumCategoryRecordCases {};
class PremiumCategoryRecordFailures : public PremiumCategoryRecordCases {};

TEST_F(PremiumCategoryRecordBody, RoundTrips) {
  const auto gb = locale("en-GB");
  const Time fetched_at{std::chrono::milliseconds{1'000}};
  const auto entry = Record::nothing_held(gb).with_answer(
      titles(gb, fetched_at, 80), fetched_at);
  const auto decoded = Record::from_body(entry.body(), gb);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->key(), gb);
  ASSERT_TRUE(decoded->category());
  ASSERT_TRUE(entry.category());
  EXPECT_EQ(decoded->category()->fetched_at(), fetched_at);
  EXPECT_EQ(*decoded->category(), *entry.category());
  ASSERT_TRUE(decoded->category()->products()[1].facts().concept_id);
  EXPECT_EQ(decoded->category()->products()[1].facts().concept_id->text(), "2");
  EXPECT_FALSE(decoded->category()->products()[0].facts().concept_id);
  EXPECT_EQ(decoded->category()->locale().text(), "en-GB");
}

TEST_F(PremiumCategoryRecordBody, RejectsGarbageAndOtherSchemas) {
  EXPECT_TRUE(absent("not json"));
  EXPECT_TRUE(absent(R"({"region":"en-GB"})"));
  EXPECT_TRUE(absent(
      R"({"record":"included","writtenAtMs":1000,"region":"en-GB","body":{"fetchedAtMs":1000,"titles":[]}})"));
  EXPECT_TRUE(absent(
      R"({"record":"included","schema":1,"writtenAtMs":1000,"region":"en-GB","body":{"fetchedAtMs":1000,"titles":[{"title":"PPSA10001","tier":"premium"}]}})"));
  EXPECT_TRUE(absent(
      R"({"record":"included","schema":0,"writtenAtMs":1000,"region":"en-GB","body":{"fetchedAtMs":1000,"titles":[]}})"));
  EXPECT_TRUE(absent(
      R"({"storeLocale":"en-GB","fetchedAtMs":1000,"ruleVersion":2,"keys":["PPSA1"]})"));
}

TEST_F(PremiumCategoryRecordBody, IsSharedByEveryoneInAStoreLocale) {
  (void)loader.load(Demand::when_stale);
  const auto text = loader.store.read(
      *libchiaking::platform::RecordName::parse("included/en-gb"));
  ASSERT_TRUE(text);
  const std::string_view stored{
      reinterpret_cast<const char*>(text.value().data.data()),
      text.value().data.size()};
  EXPECT_EQ(stored.find("account"), std::string_view::npos);
  EXPECT_EQ(stored.find("user"), std::string_view::npos);
  EXPECT_NE(stored.find(R"("featureType":"full_game")"),
            std::string_view::npos);
  EXPECT_NE(stored.find(R"("masterImage":"https://image/1.png")"),
            std::string_view::npos);
  EXPECT_EQ(loader.store.names().size(), 1U);
}

TEST_F(PremiumCategoryRecordTimes, AreFreshForAWeekAndNeverFromTheFuture) {
  const auto gb = locale("en-GB");
  const Time fetched_at{std::chrono::milliseconds{1'000'000}};
  const auto entry = Record::nothing_held(gb).with_answer(
      titles(gb, fetched_at, 80), fetched_at);
  const auto one = std::chrono::milliseconds{1};
  EXPECT_TRUE(entry.is_fresh_at(fetched_at + Record::lifetime - one));
  EXPECT_FALSE(entry.is_fresh_at(fetched_at + Record::lifetime));
  EXPECT_FALSE(entry.is_fresh_at(fetched_at - one));
  EXPECT_TRUE(entry.was_fetched_recently_at(fetched_at + hour - one));
  EXPECT_FALSE(entry.was_fetched_recently_at(fetched_at + hour));
  EXPECT_FALSE(entry.was_fetched_recently_at(fetched_at - one));
  EXPECT_FALSE(entry.retry_after(fetched_at));
}

TEST_F(PremiumCategoryRecordTimes, WaitThirtyMinutesAfterAFailure) {
  const auto gb = locale("en-GB");
  const Time fetched_at{std::chrono::milliseconds{1'000'000}};
  const auto one = std::chrono::milliseconds{1};
  const auto failed = Record::nothing_held(gb).with_failure(fetched_at);
  EXPECT_EQ(failed.retry_after(fetched_at + 30 * minute - one), one);
  EXPECT_FALSE(failed.retry_after(fetched_at + 30 * minute));
  EXPECT_FALSE(failed.retry_after(fetched_at - one));
  EXPECT_EQ(failed.retry_after(fetched_at), 30 * minute);
}

TEST_F(PremiumCategoryRecordTimes, AreAWeekAnHourAndHalfAnHour) {
  EXPECT_EQ(Record::lifetime, std::chrono::days{7});
  EXPECT_EQ(Record::refetch_cooldown, std::chrono::hours{1});
  EXPECT_EQ(Record::failure_back_off, std::chrono::minutes{30});
}

TEST_F(PremiumCategoryRecordLoads, UseAFreshSavedLookupWithoutARequest) {
  loader.save(loader.now() - 2 * hour);
  const auto loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 0);
  EXPECT_EQ(loaded.step, Step::serve);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(*loaded.record.category(),
            loader.saved_titles(loader.now() - 2 * hour));
  EXPECT_EQ(loader.now() - loaded.record.category()->fetched_at(), 2 * hour);
}

TEST_F(PremiumCategoryRecordLoads, FetchAWeekOldLookupAgain) {
  loader.save(loader.now() - Record::lifetime);
  const auto loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(loaded.step, Step::fetch);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.record.category()->fetched_at(), loader.now());
  const auto saved = loader.stored();
  ASSERT_TRUE(saved);
  ASSERT_TRUE(saved->category());
  EXPECT_EQ(saved->category()->fetched_at(), loader.now());
  EXPECT_EQ(saved->category()->products().size(), 82U);
}

TEST_F(PremiumCategoryRecordLoads, StartOverOnlyAfterAnHour) {
  loader.save(loader.now() - 20 * minute);
  auto loaded = loader.load(Demand::start_over);
  EXPECT_EQ(loader.fetches, 0);
  EXPECT_EQ(loaded.step, Step::serve_recently_fetched);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loader.now() - loaded.record.category()->fetched_at(), 20 * minute);
  loader.save(loader.now() - hour);
  loaded = loader.load(Demand::start_over);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(loaded.step, Step::fetch);
}

TEST_F(PremiumCategoryRecordLoads, IgnoreALookupSavedForAnotherLocale) {
  loader.save(loader.now() - minute, "en-US");
  const auto loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 1);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.record.category()->products().size(), 82U);
  const auto american = loader.stored("en-US");
  ASSERT_TRUE(american);
  ASSERT_TRUE(american->category());
  EXPECT_EQ(american->category()->products().size(), 80U);
}

TEST_F(PremiumCategoryRecordLoads, IgnoreALookupMisfiledUnderThisLocale) {
  loader.save(loader.now() - minute, "en-US");
  (void)loader.load(Demand::when_stale);
  const auto misfiled = loader.store.read(
      *libchiaking::platform::RecordName::parse("included/en-us"));
  ASSERT_TRUE(misfiled);
  EXPECT_TRUE(loader.store.write_atomic(
      *libchiaking::platform::RecordName::parse("included/en-gb"),
      misfiled.value().data));
  const auto again = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 2);
  EXPECT_EQ(again.step, Step::fetch);
  ASSERT_TRUE(again.record.category());
  EXPECT_EQ(again.record.category()->products().size(), 82U);
  EXPECT_EQ(again.record.category()->locale(), locale("en-GB"));
}

TEST_F(PremiumCategoryRecordLoads, RefuseAnAnswerForAnotherLocale) {
  const auto gb = locale("en-GB");
  const auto foreign = titles(locale("en-US"), loader.now(), 90);
  const auto refused =
      Record::nothing_held(gb).with_answer(foreign, loader.now());
  EXPECT_FALSE(Record::nothing_held(gb).accepts(foreign));
  EXPECT_FALSE(refused.category());
  EXPECT_EQ(refused.failed_at(), loader.now());
}

TEST_F(PremiumCategoryRecordFailures, LeaveNoTitlesWithoutASavedLookup) {
  loader.next = CategoryRecordLoader::Failed{};
  const auto loaded = loader.load(Demand::when_stale);
  EXPECT_FALSE(loaded.record.category());
  EXPECT_EQ(loaded.step, Step::fetch);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(loaded.record.failed_at(), loader.now());
  const auto saved = loader.stored();
  ASSERT_TRUE(saved);
  EXPECT_FALSE(saved->category());
  EXPECT_EQ(saved->failed_at(), loader.now());
}

TEST_F(PremiumCategoryRecordFailures, KeepThePreviousSavedLookup) {
  loader.save(loader.now() - 8 * 24 * hour);
  loader.next = CategoryRecordLoader::Failed{};
  const auto loaded = loader.load(Demand::when_stale);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.step, Step::fetch);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(*loaded.record.category(),
            loader.saved_titles(loader.now() - 8 * 24 * hour));
  EXPECT_EQ(loader.now() - loaded.record.category()->fetched_at(),
            std::chrono::days{8});
  EXPECT_EQ(loaded.record.failed_at(), loader.now());
  const auto saved = loader.stored();
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->category(), loaded.record.category());
  EXPECT_EQ(saved->failed_at(), loader.now());
}

TEST_F(PremiumCategoryRecordFailures,
       PauseLookupsForThirtyMinutesUntilASuccessClearsThem) {
  loader.next = CategoryRecordLoader::Failed{};
  (void)loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 1);
  loader.clock.advance(std::chrono::minutes{10});
  auto loaded = loader.load(Demand::start_over);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(loaded.step, Step::wait_after_failure);
  EXPECT_FALSE(loaded.record.category());
  EXPECT_EQ(loaded.record.retry_after(loader.now()), 20 * minute);
  loader.save(loader.now() - 3 * hour);
  loaded = loader.load(Demand::start_over);
  EXPECT_EQ(loader.fetches, 1);
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(*loaded.record.category(),
            loader.saved_titles(loader.now() - 3 * hour));
  EXPECT_EQ(loaded.step, Step::wait_after_failure);
  loader.clock.advance(std::chrono::minutes{20});
  loader.next = std::size_t{80};
  loaded = loader.load(Demand::start_over);
  EXPECT_EQ(loader.fetches, 2);
  EXPECT_EQ(loaded.step, Step::fetch);
  EXPECT_FALSE(loaded.record.failed_at());
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.record.category()->products().size(), 80U);
  const auto saved = loader.stored();
  ASSERT_TRUE(saved);
  EXPECT_FALSE(saved->failed_at());
}

TEST_F(PremiumCategoryRecordFailures, ExcludeACancelledLookup) {
  loader.next = CategoryRecordLoader::Cancelled{};
  const auto loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loaded.step, Step::fetch);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_FALSE(loaded.record.failed_at());
  EXPECT_FALSE(loader.stored());
  EXPECT_EQ(loader.store.writes, 0U);
  EXPECT_EQ(loader.load(Demand::when_stale).step, Step::fetch);
  EXPECT_EQ(loader.fetches, 2);
}

TEST_F(PremiumCategoryRecordFailures, IncludeAPartialScan) {
  const auto gb = locale("en-GB");
  const Time when{std::chrono::milliseconds{1'000}};
  const auto full = titles(gb, when, 90);
  const PremiumCategory partial{
      gb,
      full.products(),
      {.server_total = 200, .products_read = 90, .complete = false},
      when};
  EXPECT_TRUE(full.is_plausible());
  EXPECT_FALSE(partial.is_plausible());
  const auto refused = Record::nothing_held(gb).with_answer(partial, when);
  EXPECT_FALSE(refused.category());
  EXPECT_EQ(refused.failed_at(), when);
}

TEST_F(PremiumCategoryRecordFailures, IncludeFewerThanEightyTitles) {
  EXPECT_EQ(PremiumCategory::fewest_plausible, 80U);
  loader.next = std::size_t{79};
  auto loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_FALSE(loaded.record.category());
  EXPECT_EQ(loaded.record.failed_at(), loader.now());
  EXPECT_EQ(loader.load(Demand::when_stale).step, Step::wait_after_failure);
  EXPECT_EQ(loader.fetches, 1);
  EXPECT_EQ(loader.load(Demand::start_over).step, Step::wait_after_failure);
  EXPECT_EQ(loader.fetches, 1);

  loader.clock.advance(std::chrono::minutes{30});
  loader.save(loader.now() - 8 * 24 * hour);
  loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 2);
  EXPECT_EQ(loaded.record.failed_at(), loader.now());
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.record.category()->products().size(), 80U);
  EXPECT_EQ(*loaded.record.category(),
            loader.saved_titles(loader.now() - 8 * 24 * hour));

  loader.clock.advance(std::chrono::minutes{30});
  loader.next = std::size_t{80};
  loaded = loader.load(Demand::when_stale);
  EXPECT_EQ(loader.fetches, 3);
  EXPECT_FALSE(loaded.record.failed_at());
  ASSERT_TRUE(loaded.record.category());
  EXPECT_EQ(loaded.record.category()->fetched_at(), loader.now());
  EXPECT_EQ(loader.load(Demand::when_stale).step, Step::serve);
  EXPECT_EQ(loader.fetches, 3);
}

}  // namespace
}  // namespace catalog_test
