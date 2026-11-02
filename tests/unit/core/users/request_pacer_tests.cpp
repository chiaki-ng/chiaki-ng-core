#include "src/core/users/request_pacer.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::RequestPacer;
using Reason = libchiaking::users::Unanswered::Reason;

class RequestPacing : public ::testing::Test {
 protected:
  void spend_the_burst(AccountId account) {
    for (int request = 0; request < 5; ++request)
      EXPECT_TRUE(pacer.admit(account, {})) << request;
  }

  test_support::FakeClock clock;
  test_support::FakeWaiter waiter{&clock};
  RequestPacer pacer{
      test_support::process_threading(), clock, waiter.factory(),
      RequestPacer::Limits{.burst = 5,
                           .window_requests = 300,
                           .window = std::chrono::minutes{15},
                           .cooldown = std::chrono::minutes{15}}};
};

TEST_F(RequestPacing, SendsABurstAtOnceAndMakesTheRestWaitTheirTurn) {
  spend_the_burst(AccountId{1});
  EXPECT_TRUE(waiter.waits.empty());
  EXPECT_TRUE(pacer.admit(AccountId{1}, {}));
  EXPECT_EQ(waiter.waits, std::vector{200});
  EXPECT_TRUE(pacer.admit(AccountId{1}, {}));
  EXPECT_EQ(waiter.waits, (std::vector{200, 200}));
}

TEST_F(RequestPacing, PacesAccountsApart) {
  spend_the_burst(AccountId{1});
  spend_the_burst(AccountId{2});
  EXPECT_TRUE(waiter.waits.empty());
}

TEST_F(RequestPacing, RefusesRatherThanWaitsOnceTheWindowIsSpent) {
  int admitted = 0;
  for (;;) {
    auto turn = pacer.admit(AccountId{1}, {});
    if (!turn) {
      EXPECT_EQ(turn.error().reason, Reason::rate_limited);
      break;
    }
    ++admitted;
    ASSERT_LT(admitted, 400);
  }
  EXPECT_GE(admitted, 300);
  EXPECT_LT(admitted, 330);
  EXPECT_FALSE(pacer.admit(AccountId{1}, {}));
  clock.advance_ms(3000);
  EXPECT_TRUE(pacer.admit(AccountId{1}, {}));
  EXPECT_FALSE(pacer.admit(AccountId{1}, {}));
}

TEST_F(RequestPacing, CoolsAThrottledAccountDownForFifteenMinutes) {
  pacer.throttled(AccountId{1});
  const auto refused = pacer.admit(AccountId{1}, {});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().reason, Reason::rate_limited);
  EXPECT_TRUE(pacer.admit(AccountId{2}, {}));
  clock.advance_ms(15 * 60 * 1000 - 1);
  EXPECT_FALSE(pacer.admit(AccountId{1}, {}));
  clock.advance_ms(1);
  EXPECT_TRUE(pacer.admit(AccountId{1}, {}));
}

TEST_F(RequestPacing, ForgetsTheCooldownOfAForgottenAccount) {
  pacer.throttled(AccountId{1});
  pacer.forget(AccountId{1});
  EXPECT_TRUE(pacer.admit(AccountId{1}, {}));
}

TEST_F(RequestPacing, CancelsATurnStoppedDuringItsWait) {
  spend_the_burst(AccountId{1});
  auto stop = std::make_shared<test_support::StopState>();
  waiter.stop_on_wait = 1;
  waiter.stop_state = stop.get();
  const auto turn =
      pacer.admit(AccountId{1}, libchiaking::platform::StopToken{stop});
  ASSERT_FALSE(turn);
  EXPECT_EQ(turn.error().reason, Reason::cancelled);
}

}  // namespace
}  // namespace users_test
