#include "libchiaking/testing/service_builder.hpp"
#include "src/core/trophies/trophies_core.hpp"
#include "src/core/users/request_budget_core.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/trophies/rig.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/trophies.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trophies_test {
namespace {

using test_support::psn::Reply;
using CatalogReason = libchiaking::catalog::PlayedGamesReason;
using Reason = libchiaking::users::Unanswered::Reason;

enum class Budget : std::uint8_t { shared, separate };

class BudgetRig {
 public:
  static constexpr std::string_view summary_url =
      "https://m.np.playstation.com/api/trophy/v1/users/me/trophySummary";
  static constexpr std::string_view summary_body =
      R"({"trophyLevel":12,"tier":1,"progress":3,)"
      R"("earnedTrophies":{"bronze":4,"silver":1,"gold":0,"platinum":0}})";
  static constexpr std::string_view store_token = "SYNTHETIC_APP_TOKEN";

  static auto page_url(std::size_t offset) -> std::string {
    return "https://m.np.playstation.com/api/gamelist/v2/users/me/"
           "titles?limit=50&offset=" +
           std::to_string(offset);
  }

  static auto page_of(std::size_t first, std::size_t count,
                      std::optional<std::size_t> next) -> std::string {
    std::string rows;
    for (std::size_t number = first; number < first + count; ++number) {
      auto digits = std::to_string(number);
      digits.insert(0, 5 - digits.size(), '0');
      rows += std::string{rows.empty() ? "" : ","} + R"({"titleId":"PPSA)" +
              digits + R"(_00","name":"Game","playDuration":"PT2H"})";
    }
    std::string body = R"({"titles":[)" + rows + "]";
    if (next) body += R"(,"nextOffset":)" + std::to_string(*next);
    return body + "}";
  }

  explicit BudgetRig(Budget budgets) {
    wall.now_us = Rig::wall_start_us;
    const bool shared = budgets == Budget::shared;
    trophies = std::make_unique<libchiaking::trophies::Trophies::Core>(
        http, records, test_support::process_threading(), clock, wall, nullptr,
        trophy_waiter.factory(), shared ? budget : trophy_budget);
    catalog = libchiaking::testing::ServiceBuilder::build_unique<
        libchiaking::catalog::Catalog>(http, records,
                                       test_support::process_threading(), clock,
                                       wall, shared ? budget : catalog_budget);
    user = signed_in.as(Rig::store_sign_in(store_token));
  }

  void answers_summary(Reply reply) {
    http.on("GET", std::string{summary_url}, {std::move(reply)});
  }

  void script_five_pages() {
    for (std::size_t page = 0; page < 4; ++page)
      http.on("GET", page_url(page * 50),
              {Reply{200, page_of(page * 50 + 1, 50, (page + 1) * 50)}});
    http.on("GET", page_url(200), {Reply{200, page_of(201, 3, {})}});
  }

  auto budget_waits() const -> std::size_t {
    return waiter.waits.size() + catalog_waiter.waits.size() +
           trophy_waiter.waits.size();
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp http;
  libchiaking::testing::InMemoryRecordStore records{
      test_support::process_threading()};
  test_support::users::SignedIn signed_in{http, clock, wall};
  test_support::FakeWaiter waiter{&clock};
  libchiaking::users::RequestBudget budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, waiter.factory())};
  test_support::FakeWaiter catalog_waiter{&clock};
  libchiaking::users::RequestBudget catalog_budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, catalog_waiter.factory())};
  test_support::FakeWaiter trophy_waiter{&clock};
  libchiaking::users::RequestBudget trophy_budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, trophy_waiter.factory())};
  std::unique_ptr<libchiaking::trophies::Trophies::Core> trophies;
  std::unique_ptr<libchiaking::catalog::Catalog> catalog;
  std::shared_ptr<libchiaking::users::PsUser> user;
};

TEST(SharedRequestBudget, LetsBothServicesDrawOnOneBurst) {
  BudgetRig rig{Budget::shared};
  rig.answers_summary(Reply{200, std::string{BudgetRig::summary_body}});
  rig.script_five_pages();
  EXPECT_TRUE(rig.trophies->read().summary(*rig.user, {}));
  const auto played = rig.catalog->played_games(*rig.user);
  ASSERT_TRUE(played);
  EXPECT_EQ(played->size(), 203U);
  EXPECT_EQ(rig.waiter.waits, (std::vector{200}));
  EXPECT_EQ(rig.budget_waits(), 1U);
}

TEST(SharedRequestBudget, CountsSeparateBudgetsApart) {
  BudgetRig rig{Budget::separate};
  rig.answers_summary(Reply{200, std::string{BudgetRig::summary_body}});
  rig.script_five_pages();
  EXPECT_TRUE(rig.trophies->read().summary(*rig.user, {}));
  EXPECT_TRUE(rig.catalog->played_games(*rig.user));
  EXPECT_EQ(rig.budget_waits(), 0U);
}

TEST(SharedRequestBudget, HoldsBackThePlayHistoryAfterA429FromTrophies) {
  BudgetRig rig{Budget::shared};
  rig.answers_summary(Reply{429, "{}"});
  rig.script_five_pages();
  const auto summary = rig.trophies->read().summary(*rig.user, {});
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().reason, Reason::rate_limited);
  const auto played = rig.catalog->played_games(*rig.user);
  ASSERT_FALSE(played);
  EXPECT_EQ(played.error().reason(), CatalogReason::user);
  ASSERT_TRUE(played.error().cause());
  EXPECT_EQ(played.error().cause()->reason, Reason::rate_limited);
  EXPECT_EQ(rig.http.count("/gamelist/"), 0);
}

TEST(SharedRequestBudget, HoldsBackTrophiesAfterA429FromTheGameList) {
  BudgetRig rig{Budget::shared};
  rig.answers_summary(Reply{200, std::string{BudgetRig::summary_body}});
  rig.http.on("GET", BudgetRig::page_url(0), {Reply{429, "{}"}});
  EXPECT_FALSE(rig.catalog->played_games(*rig.user));
  const auto summary = rig.trophies->read().summary(*rig.user, {});
  ASSERT_FALSE(summary);
  EXPECT_EQ(summary.error().reason, Reason::rate_limited);
  EXPECT_EQ(rig.http.count("/trophy/"), 0);
  rig.clock.advance_ms(
      std::chrono::milliseconds{std::chrono::minutes{15}}.count());
  EXPECT_TRUE(rig.trophies->read().summary(*rig.user, {}));
}

class AtomicClock final : public libchiaking::platform::ClockProvider {
 public:
  auto now_microseconds() const noexcept -> std::uint64_t override {
    return now_us.load();
  }
  void advance_ms(int milliseconds) {
    now_us += static_cast<std::uint64_t>(milliseconds) * 1000U;
  }
  std::atomic<std::uint64_t> now_us{1'000'000'000};
};

class LockedHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    const libchiaking::platform::Lock lock{mutex};
    return inner.perform(request, stop);
  }
  libchiaking::platform::Mutex mutex{test_support::process_threading()};
  test_support::psn::FakeHttp inner;
};

class GuardedWaiters {
 public:
  explicit GuardedWaiters(AtomicClock& clock) : clock_(clock) {}

  auto factory() -> libchiaking::net::http::WaiterFactory {
    return [this]() -> std::unique_ptr<libchiaking::net::http::Waiter> {
      ++made;
      return std::make_unique<Guarded>(*this);
    };
  }

  static inline thread_local int worker = 0;

  std::atomic<int> made{};
  std::atomic<int> waits{};
  libchiaking::platform::Mutex mutex{test_support::process_threading()};
  std::set<int> workers;

 private:
  class Guarded final : public libchiaking::net::http::Waiter {
   public:
    explicit Guarded(GuardedWaiters& owner) : owner_(owner) {}

    auto wait_milliseconds(int milliseconds,
                           const libchiaking::platform::StopToken& stop)
        -> bool override {
      EXPECT_NE(worker, 0);
      if (used_by_ == 0) used_by_ = worker;
      EXPECT_EQ(used_by_, worker);
      EXPECT_FALSE(busy_.exchange(true));
      {
        const libchiaking::platform::Lock lock{owner_.mutex};
        owner_.workers.insert(worker);
      }
      ++owner_.waits;
      owner_.clock_.advance_ms(milliseconds);
      busy_ = false;
      return !stop.stop_requested();
    }

   private:
    GuardedWaiters& owner_;
    int used_by_{};
    std::atomic<bool> busy_{};
  };

  AtomicClock& clock_;
};

class ConcurrentRig {
 public:
  static constexpr std::size_t history_pages = 7;
  static constexpr std::size_t set_batches = 6;

  ConcurrentRig() {
    wall.now_us = Rig::wall_start_us;
    user = signed_in.as(Rig::store_sign_in(BudgetRig::store_token));
    for (std::size_t page = 0; page < history_pages; ++page)
      http.inner.on(
          "GET", BudgetRig::page_url(page * 50),
          {Reply{200, BudgetRig::page_of(page * 50 + 1, 50, (page + 1) * 50)}});
    http.inner.on("GET", BudgetRig::page_url(history_pages * 50),
                  {Reply{200, R"({"titles":[]})"}});
    for (std::size_t batch = 0; batch < set_batches; ++batch)
      script_set_batch(batch);
  }

  AtomicClock clock;
  test_support::FakeClock wall;
  LockedHttp http;
  libchiaking::testing::InMemoryRecordStore records{
      test_support::process_threading()};
  test_support::users::SignedIn signed_in{http, clock, wall};
  GuardedWaiters waiters{clock};
  libchiaking::users::RequestBudget budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, waiters.factory())};
  libchiaking::trophies::Trophies::Core trophies{
      http,
      records,
      test_support::process_threading(),
      clock,
      wall,
      nullptr,
      waiters.factory(),
      budget};
  libchiaking::catalog::Catalog catalog{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::catalog::Catalog>(http, records,
                                         test_support::process_threading(),
                                         clock, wall, budget)};
  std::shared_ptr<libchiaking::users::PsUser> user;
  std::vector<libchiaking::NpTitleId> titles;

 private:
  void script_set_batch(std::size_t batch) {
    std::string ids;
    std::string rows;
    for (std::size_t index = 0; index < 5; ++index) {
      const auto number = 90000 + batch * 5 + index;
      const auto id = "PPSA" + std::to_string(number) + "_00";
      titles.push_back(libchiaking::NpTitleId::parse(id).value());
      ids += std::string{ids.empty() ? "" : ","} + id;
      rows += std::string{rows.empty() ? "" : ","} + R"({"npTitleId":")" + id +
              R"(","trophyTitles":[{"npCommunicationId":"NPWR)" +
              std::to_string(number) + R"(_00"}]})";
    }
    http.inner.on("GET",
                  "https://m.np.playstation.com/api/trophy/v1/users/me/titles/"
                  "trophyTitles?npTitleIds=" +
                      ids,
                  {Reply{200, R"({"titles":[)" + rows + "]}"}});
  }
};

TEST(SharedRequestBudget, NeverSharesAWaiterBetweenConcurrentServices) {
  ConcurrentRig rig;
  std::atomic<int> ready{};
  std::atomic<bool> played{};
  std::atomic<bool> mapped{};
  {
    auto history = libchiaking::platform::Thread::start(
        test_support::process_threading(), "history",
        libchiaking::platform::ThreadRole::unspecified, [&] {
          GuardedWaiters::worker = 1;
          ++ready;
          while (ready.load() < 2) {
          }
          const auto read = rig.catalog.played_games(*rig.user);
          played = read && read->size() == 350;
        });
    auto trophy_sets = libchiaking::platform::Thread::start(
        test_support::process_threading(), "trophy-sets",
        libchiaking::platform::ThreadRole::unspecified, [&] {
          GuardedWaiters::worker = 2;
          ++ready;
          while (ready.load() < 2) {
          }
          const auto read =
              rig.trophies.read().title_sets(*rig.user, rig.titles, {});
          mapped = read && read->size() == 30 && read->back().sets.size() == 1;
        });
    ASSERT_TRUE(history);
    ASSERT_TRUE(trophy_sets);
  }
  EXPECT_TRUE(played);
  EXPECT_TRUE(mapped);
  EXPECT_TRUE(rig.http.inner.unmatched.empty());
  EXPECT_EQ(rig.http.inner.seen.size(), 14U);
  EXPECT_GE(rig.waiters.waits.load(), 9);
  EXPECT_GE(rig.waiters.made.load(), 2);
  EXPECT_EQ(rig.waiters.workers.size(), 2U);
}

}  // namespace
}  // namespace trophies_test
