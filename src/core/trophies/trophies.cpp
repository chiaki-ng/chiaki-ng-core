#include "libchiaking/detail/trophies/trophies.hpp"

#include "src/core/net/transport/http/sleep_waiter.hpp"
#include "src/core/trophies/trophies_core.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <memory>

namespace libchiaking::trophies {

static_assert(Trophies::titles_per_request ==
              ps::TrophyRequests::titles_per_request);

Trophies::Trophies(platform::HttpProvider& http, platform::RecordStore& records,
                   platform::ThreadingProvider& threading,
                   const platform::ClockProvider& clock,
                   const platform::ClockProvider& wall_clock,
                   users::RequestBudget& budget,
                   platform::DiagnosticsProvider* diagnostics)
    : core_(std::make_unique<Core>(
          http, records, threading, clock, wall_clock, diagnostics,
          [&threading, &clock]() -> std::unique_ptr<net::http::Waiter> {
            return std::make_unique<net::http::SleepWaiter>(threading, clock);
          },
          budget)) {}

Trophies::~Trophies() = default;

auto Trophies::summary_of(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<TrophySummary, users::Unanswered> {
  return core_->read().summary(user, stop);
}

auto Trophies::titles_of(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<TrophyTitle>, users::Unanswered> {
  return core_->read().titles(user, stop);
}

auto Trophies::trophies_of(users::PsUser& user, const TrophyTitle& title,
                           const platform::StopToken& stop)
    -> Expected<TitleTrophies, users::Unanswered> {
  return core_->read().trophies(user, title, stop);
}

auto Trophies::titles_for(users::PsUser& user,
                          std::span<const NpTitleId> titles,
                          const platform::StopToken& stop)
    -> Expected<std::vector<TitleSets>, users::Unanswered> {
  return core_->read().title_sets(user, titles, stop);
}

auto Trophies::current_trophies(users::PsUser& user, const NpTitleId& title,
                                const platform::StopToken& stop)
    -> Expected<std::optional<CurrentTrophies>, users::Unanswered> {
  return core_->read().current(user, title, stop);
}

void Trophies::start_over(users::PsUser& user) {
  core_->read().start_over(user.account_id());
}

void Trophies::forget(AccountId account) { core_->read().forget(account); }

}  // namespace libchiaking::trophies
