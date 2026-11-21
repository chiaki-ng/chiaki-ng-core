#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/unanswered.hpp"

namespace libchiaking::catalog::usecases {

class GroupedDetails {
 public:
  using Stop = platform::StopToken;

  GroupedDetails(platform::HttpProvider& http,
                 platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 platform::DiagnosticsProvider* log, HeldAnswers& held);
  GroupedDetails(const GroupedDetails&) = delete;
  auto operator=(const GroupedDetails&) -> GroupedDetails& = delete;

  [[nodiscard]] auto details(const users::PsUser& user,
                             const ConceptId& grouped_under, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  [[nodiscard]] auto details(const StoreLocale& locale,
                             const ConceptId& grouped_under, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;

 private:
  platform::HttpProvider& http_;
  platform::ThreadingProvider& threading_;
  const platform::ClockProvider& clock_;
  const platform::ClockProvider& wall_;
  platform::DiagnosticsProvider* log_;
  HeldAnswers& held_;
};

}  // namespace libchiaking::catalog::usecases
