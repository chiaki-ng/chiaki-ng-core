#include "libchiaking/detail/users/request_budget.hpp"

#include "src/core/net/transport/http/sleep_waiter.hpp"
#include "src/core/users/request_budget_core.hpp"

#include <memory>
#include <utility>

namespace libchiaking::users {

RequestBudget::Core::Core(platform::ThreadingProvider& threading,
                          const platform::ClockProvider& clock)
    : pacer_(
          threading, clock,
          [&threading, &clock]() -> std::unique_ptr<net::http::Waiter> {
            return std::make_unique<net::http::SleepWaiter>(threading, clock);
          },
          limits()) {}

RequestBudget::Core::Core(platform::ThreadingProvider& threading,
                          const platform::ClockProvider& clock,
                          net::http::WaiterFactory waiters)
    : pacer_(threading, clock, std::move(waiters), limits()) {}

RequestBudget::RequestBudget(platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock)
    : core_(std::make_unique<Core>(threading, clock)) {}

RequestBudget::RequestBudget(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

RequestBudget::~RequestBudget() = default;

void RequestBudget::forget(AccountId account) {
  core_->pacer().forget(account);
}

}  // namespace libchiaking::users
