#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::platform::Thread;
using libchiaking::platform::ThreadRole;
using libchiaking::psn::auth::AuthError;
using libchiaking::psn::auth::AuthErrorKind;
using libchiaking::psn::auth::BearerToken;
using libchiaking::psn::auth::Exchanged;
using libchiaking::psn::auth::Npsso;
using libchiaking::psn::auth::TokenCache;

class ConcurrentTokenExchange : public ::testing::Test {
 protected:
  static auto now() -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::time_point{std::chrono::microseconds{
        ClientRig::steady_clock().now_microseconds()}};
  }

  void wait_for_arrivals(const std::atomic<int>& arrived, int callers) {
    const auto deadline = now() + std::chrono::seconds{5};
    while (arrived.load() < callers && now() < deadline)
      sleeper.sleep_for(std::chrono::milliseconds{1});
    sleeper.sleep_for(std::chrono::milliseconds{50});
  }

  template <class Body>
  static auto start(Body body) -> Thread {
    return Thread::start(ClientRig::threading(), "test",
                         ThreadRole::unspecified, std::move(body))
        .value();
  }

  static void join(std::vector<Thread>& threads) {
    for (auto& thread : threads) static_cast<void>(thread.join());
  }

  libchiaking::platform::Sleeper sleeper{ClientRig::threading(),
                                         ClientRig::steady_clock()};
  test_support::FakeClock clock;
  TokenCache cache{Npsso{"NPSSO-A"}, ClientRig::threading(), clock, clock};
};

TEST_F(ConcurrentTokenExchange, LetsCallersShareOneExchange) {
  const ClientRig::StopToken stop{std::make_shared<test_support::StopState>()};
  std::atomic<int> exchanges{0};
  std::atomic<int> arrived{0};
  constexpr int callers = 8;
  const TokenCache::Exchange exchange =
      [&](const Npsso& npsso) -> libchiaking::Expected<Exchanged, AuthError> {
    EXPECT_EQ(npsso.view(), "NPSSO-A");
    exchanges.fetch_add(1);
    wait_for_arrivals(arrived, callers);
    return Exchanged{BearerToken{"SHARED"}, 0, 302};
  };
  std::vector<Thread> threads;
  std::atomic<int> fresh{0};
  std::atomic<int> reused{0};
  for (int index = 0; index < callers; ++index)
    threads.push_back(start([&] {
      arrived.fetch_add(1);
      const auto result =
          cache.acquire(AuthRig::owned_grant(), {}, stop, exchange);
      ASSERT_TRUE(result);
      EXPECT_EQ(result.value().token.view(), "SHARED");
      (result.value().cached ? reused : fresh).fetch_add(1);
    }));
  join(threads);
  EXPECT_EQ(exchanges.load(), 1);
  EXPECT_EQ(fresh.load(), 1);
  EXPECT_EQ(reused.load(), callers - 1);
}

TEST_F(ConcurrentTokenExchange, SharesAFailureRatherThanRepeatingIt) {
  const ClientRig::StopToken stop{std::make_shared<test_support::StopState>()};
  std::atomic<int> exchanges{0};
  std::atomic<int> arrived{0};
  constexpr int callers = 4;
  const TokenCache::Exchange exchange =
      [&](const Npsso&) -> libchiaking::Expected<Exchanged, AuthError> {
    exchanges.fetch_add(1);
    wait_for_arrivals(arrived, callers);
    return libchiaking::unexpected(AuthError{AuthErrorKind::refused, 302});
  };
  std::vector<Thread> threads;
  for (int index = 0; index < callers; ++index)
    threads.push_back(start([&] {
      arrived.fetch_add(1);
      const auto result =
          cache.acquire(AuthRig::owned_grant(), {}, stop, exchange);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().kind(), AuthErrorKind::refused);
      EXPECT_EQ(result.error().http_status(), 302);
    }));
  join(threads);
  EXPECT_EQ(exchanges.load(), 1);
}

TEST_F(ConcurrentTokenExchange, ReleasesItsWaitersWhenTheExchangeThrows) {
  const ClientRig::StopToken stop;
  EXPECT_THROW(
      static_cast<void>(cache.acquire(
          AuthRig::owned_grant(), {}, stop,
          [](const Npsso&) -> libchiaking::Expected<Exchanged, AuthError> {
            throw std::runtime_error{"boom"};
          })),
      std::runtime_error);
  const auto next =
      cache.acquire(AuthRig::owned_grant(), {}, stop, AuthRig::issued("AFTER"));
  ASSERT_TRUE(next);
  EXPECT_EQ(next.value().token.view(), "AFTER");
  EXPECT_FALSE(next.value().cached);
  const auto tokens = cache.snapshot().tokens;
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.front().usage.exchanges, 2U);
}

TEST_F(ConcurrentTokenExchange, LetsAWaitingCallerHonourItsOwnStop) {
  const ClientRig::StopToken leader_stop{
      std::make_shared<test_support::StopState>()};
  auto waiter_state = std::make_shared<test_support::StopState>();
  const ClientRig::StopToken waiter_stop{waiter_state};
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  auto leader = start([&] {
    const auto result = cache.acquire(
        AuthRig::owned_grant(), {}, leader_stop,
        [&](const Npsso&) -> libchiaking::Expected<Exchanged, AuthError> {
          entered.store(true);
          while (!release.load())
            sleeper.sleep_for(std::chrono::milliseconds{1});
          return Exchanged{BearerToken{"T"}, 0, 302};
        });
    EXPECT_TRUE(result);
  });
  while (!entered.load()) sleeper.sleep_for(std::chrono::milliseconds{1});
  waiter_state->stopped = true;
  const auto waited = cache.acquire(
      AuthRig::owned_grant(), {}, waiter_stop,
      [](const Npsso&) -> libchiaking::Expected<Exchanged, AuthError> {
        ADD_FAILURE() << "a stopped waiter started its own exchange";
        return libchiaking::unexpected(AuthError{AuthErrorKind::transport});
      });
  release.store(true);
  static_cast<void>(leader.join());
  EXPECT_EQ(AuthRig::kind_of(waited), AuthErrorKind::cancelled);
}

TEST_F(ConcurrentTokenExchange, CountsConcurrentUsesExactly) {
  constexpr int threads_count = 8;
  constexpr int per_thread = 500;
  std::vector<Thread> threads;
  for (int index = 0; index < threads_count; ++index)
    threads.push_back(start([this, index] {
      for (int use = 0; use < per_thread; ++use)
        cache.record_use(AuthRig::owned_grant(),
                         (index & 1) != 0 && use % 5 == 0 ? std::uint16_t{401}
                                                          : std::uint16_t{200});
    }));
  join(threads);
  const auto view = cache.snapshot();
  EXPECT_EQ(view.uses, threads_count * per_thread);
  EXPECT_EQ(view.refusals, (threads_count / 2) * (per_thread / 5));
  ASSERT_EQ(view.tokens.size(), 1U);
  EXPECT_EQ(view.tokens.front().usage.uses, threads_count * per_thread);
}

}  // namespace
}  // namespace psn_test
