#include "src/core/net/protocol/senkusha/mtu_search.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;

class ConservativeMtuSearch : public ::testing::Test {
 protected:
  static auto default_search()
      -> std::optional<senkusha::ConservativeMtuSearch> {
    auto search = senkusha::ConservativeMtuSearch::create(576, 1454, 3, 1454);
    if (!search) return std::nullopt;
    return std::move(search).value();
  }
};

TEST_F(ConservativeMtuSearch, SettlesOnTheStartWhenTheFirstProbeSucceeds) {
  auto success = default_search();
  ASSERT_TRUE(success);
  const auto first = success->next_attempt();
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value().candidate, 1454U);
  EXPECT_TRUE(success->record_success(first.value().id));
  ASSERT_TRUE(success->snapshot().result.has_value());
  EXPECT_EQ(success->snapshot().result, 1454U);
}

TEST_F(ConservativeMtuSearch, SettlesOnTheFloorWhenEveryProbeFails) {
  auto failure = default_search();
  ASSERT_TRUE(failure);
  while (!failure->snapshot().result.has_value()) {
    const auto attempt = failure->next_attempt();
    ASSERT_TRUE(attempt);
    ASSERT_TRUE(failure->record_failure(attempt.value().id));
  }
  EXPECT_EQ(failure->snapshot().result, 576U);
  EXPECT_EQ(failure->snapshot().upper, 577U);
}

TEST_F(ConservativeMtuSearch, RetriesACandidateThenBisectsTheExactBounds) {
  auto created = senkusha::ConservativeMtuSearch::create(576, 1454, 3, 1000);
  ASSERT_TRUE(created);
  auto search = std::move(created).value();
  for (std::uint32_t attempt_number = 1; attempt_number <= 3;
       ++attempt_number) {
    const auto attempt = search.next_attempt();
    ASSERT_TRUE(attempt) << "attempt " << attempt_number;
    EXPECT_EQ(attempt.value().candidate, 1000U) << "attempt " << attempt_number;
    EXPECT_EQ(attempt.value().attempt_number, attempt_number);
    EXPECT_TRUE(search.record_failure(attempt.value().id))
        << "attempt " << attempt_number;
  }
  const auto midpoint = search.next_attempt();
  ASSERT_TRUE(midpoint);
  EXPECT_EQ(midpoint.value().candidate, 788U);
  EXPECT_TRUE(search.record_success(midpoint.value().id));
  EXPECT_EQ(search.snapshot().lower, 788U);
  const auto upper_midpoint = search.next_attempt();
  ASSERT_TRUE(upper_midpoint);
  EXPECT_EQ(upper_midpoint.value().candidate, 894U);
}

TEST_F(ConservativeMtuSearch, NeverLetsAStaleAttemptChangeTheSearch) {
  auto search = default_search();
  ASSERT_TRUE(search);
  const auto initial = search->snapshot();
  EXPECT_FALSE(search->record_success(1));
  EXPECT_EQ(search->snapshot().lower, initial.lower);
  const auto attempt = search->next_attempt();
  ASSERT_TRUE(attempt);
  const auto outstanding = search->snapshot();
  EXPECT_FALSE(search->next_attempt());
  EXPECT_FALSE(search->record_failure(attempt.value().id + 1));
  EXPECT_EQ(search->snapshot().attempts_used, outstanding.attempts_used);
  EXPECT_TRUE(search->record_failure(attempt.value().id));
  const auto after = search->snapshot();
  EXPECT_FALSE(search->record_success(attempt.value().id));
  EXPECT_EQ(search->snapshot().lower, after.lower);
  EXPECT_EQ(search->snapshot().upper, after.upper);
}

TEST_F(ConservativeMtuSearch, RefusesAnInvalidConfiguration) {
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(1, 1, 1, 1));
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(2, 1, 1, 1));
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(1, 2, 0, 1));
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(10, 20, 1, 9));
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(10, 20, 1, 21));
  EXPECT_FALSE(senkusha::ConservativeMtuSearch::create(10, 20, 1, 10, 0));
}

TEST_F(ConservativeMtuSearch, ReportsAnOverflowingAttemptId) {
  auto created = senkusha::ConservativeMtuSearch::create(
      10, 20, 2, 15, std::numeric_limits<std::uint64_t>::max());
  ASSERT_TRUE(created);
  auto search = std::move(created).value();
  const auto final_id = search.next_attempt();
  ASSERT_TRUE(final_id);
  EXPECT_EQ(final_id.value().id, std::numeric_limits<std::uint64_t>::max());
  EXPECT_TRUE(search.record_failure(final_id.value().id));
  const auto overflow = search.next_attempt();
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), senkusha::Error::arithmetic_overflow);
}

}  // namespace
}  // namespace senkusha_test
