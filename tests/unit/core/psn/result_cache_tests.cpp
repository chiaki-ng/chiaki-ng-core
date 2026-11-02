#include "src/core/psn/result_cache.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace psn_test {
namespace {

class ResultCache : public ::testing::Test {
 protected:
  struct Answer {
    std::string text;
    bool cached{};
    std::int64_t fetched_at_ms{};
  };

  class Refused {
   public:
    explicit Refused(bool cancelled) noexcept : cancelled_(cancelled) {}

    static auto cancelled() noexcept -> Refused { return Refused{true}; }

    auto is_cancelled() const noexcept -> bool { return cancelled_; }

   private:
    bool cancelled_;
  };

  using Cache = libchiaking::psn::ResultCache<Answer, Refused>;

  static auto any(const Answer&) -> bool { return true; }

  auto fetch(std::string text) -> Cache::Fetch {
    return [this, answer = std::move(text)]() -> Cache::Result {
      ++calls;
      return Answer{answer};
    };
  }

  static auto wall_at(std::uint64_t now_us)
      -> libchiaking::testing::ManualClock {
    libchiaking::testing::ManualClock started;
    started.now_us = now_us;
    return started;
  }

  libchiaking::testing::ManualClock clock;
  libchiaking::testing::ManualClock wall = wall_at(1'700'000'000'000'000);
  Cache cache{ClientRig::threading(), clock, wall, std::chrono::minutes{5}};
  int calls{};
};

TEST_F(ResultCache, ReusesAFreshAnswerUntilItExpires) {
  const auto first = cache.get("user/42/product", {}, any, fetch("first"));
  ASSERT_TRUE(first);
  EXPECT_FALSE(first.value().cached);
  EXPECT_EQ(first.value().fetched_at_ms, 1'700'000'000'000);
  const auto again = cache.get("user/42/product", {}, any, fetch("second"));
  ASSERT_TRUE(again);
  EXPECT_TRUE(again.value().cached);
  EXPECT_EQ(again.value().text, "first");
  EXPECT_EQ(calls, 1);
  const auto peeked = cache.peek("user/42/product");
  ASSERT_TRUE(peeked);
  EXPECT_EQ(peeked->text, "first");
  EXPECT_EQ(cache.counts().fresh, 1U);

  clock.advance(std::chrono::minutes{5});
  EXPECT_FALSE(cache.peek("user/42/product"));
  const auto held = cache.held("user/42/product");
  ASSERT_TRUE(held);
  EXPECT_FALSE(held->second);
  const auto renewed = cache.get("user/42/product", {}, any, fetch("third"));
  ASSERT_TRUE(renewed);
  EXPECT_FALSE(renewed.value().cached);
  EXPECT_EQ(renewed.value().text, "third");
  EXPECT_EQ(calls, 2);
}

TEST_F(ResultCache, ReturnsARefusalTypedByTheOwnerWithoutKeepingIt) {
  const auto refused = cache.get("key", {}, any, []() -> Cache::Result {
    return libchiaking::unexpected(Refused{false});
  });
  ASSERT_FALSE(refused);
  EXPECT_FALSE(refused.error().is_cancelled());
  EXPECT_FALSE(cache.peek("key"));
  EXPECT_EQ(cache.counts().entries, 0U);
}

TEST_F(ResultCache, ForgetsAnswersByKeyAndByPrefix) {
  for (const auto* key : {"user/42/a", "user/42/b", "user/421/a", "user/7/a"})
    EXPECT_TRUE(cache.get(key, {}, any, fetch(key))) << key;
  EXPECT_EQ(cache.counts().entries, 4U);

  cache.forget("user/7/a");
  EXPECT_FALSE(cache.peek("user/7/a"));
  EXPECT_TRUE(cache.peek("user/42/a"));
  EXPECT_EQ(cache.counts().entries, 3U);
  cache.forget("never/asked");

  cache.forget_prefix("user/42/");
  EXPECT_FALSE(cache.peek("user/42/a"));
  EXPECT_FALSE(cache.peek("user/42/b"));
  EXPECT_TRUE(cache.peek("user/421/a"));
  EXPECT_EQ(cache.counts().entries, 1U);
  const auto asked_again = cache.get("user/42/a", {}, any, fetch("again"));
  ASSERT_TRUE(asked_again);
  EXPECT_FALSE(asked_again.value().cached);
  EXPECT_EQ(asked_again.value().text, "again");

  cache.clear();
  EXPECT_EQ(cache.counts().entries, 0U);
  EXPECT_FALSE(cache.peek("user/421/a"));
}

}  // namespace
}  // namespace psn_test
