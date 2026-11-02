#include "src/core/users/request_budget_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::RequestBudget;
using Reason = libchiaking::users::Unanswered::Reason;

static_assert(RequestBudget::Core::limits().burst ==
              RequestBudget::burst_requests);
static_assert(RequestBudget::Core::limits().window_requests ==
              RequestBudget::window_requests);

class RequestBudgetPacing : public ::testing::Test {
 protected:
  auto pacer() -> libchiaking::users::RequestPacer& {
    return budget.core().pacer();
  }

  test_support::FakeClock clock;
  test_support::FakeWaiter waiter{&clock};
  RequestBudget budget{std::make_unique<RequestBudget::Core>(
      test_support::process_threading(), clock, waiter.factory())};
};

TEST_F(RequestBudgetPacing, PacesAsItDocuments) {
  for (int request = 0; request < 5; ++request)
    EXPECT_TRUE(pacer().admit(AccountId{1}, {})) << request;
  EXPECT_TRUE(waiter.waits.empty());
  EXPECT_TRUE(pacer().admit(AccountId{1}, {}));
  EXPECT_EQ(waiter.waits, std::vector{200});
}

TEST_F(RequestBudgetPacing, EndsTheCooldownOfAForgottenAccount) {
  pacer().throttled(AccountId{1});
  const auto cooling = pacer().admit(AccountId{1}, {});
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error().reason, Reason::rate_limited);
  budget.forget(AccountId{1});
  EXPECT_TRUE(pacer().admit(AccountId{1}, {}));
}

TEST(StockRequestBudget, AdmitsAFirstRequestAtOnce) {
  libchiaking::platform::SteadyClock clock;
  RequestBudget budget{test_support::process_threading(), clock};
  EXPECT_TRUE(budget.core().pacer().admit(AccountId{3}, {}));
}

}  // namespace
}  // namespace users_test
